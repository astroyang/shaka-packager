// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/formats/mp4/segmenter.h"

#include <algorithm>

#include "packager/base/stl_util.h"
#include "packager/media/base/buffer_writer.h"
#include "packager/media/base/key_source.h"
#include "packager/media/base/media_sample.h"
#include "packager/media/base/media_stream.h"
#include "packager/media/base/muxer_options.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/event/muxer_listener.h"
#include "packager/media/event/progress_listener.h"
#include "packager/media/formats/mp4/box_definitions.h"
#include "packager/media/formats/mp4/key_rotation_fragmenter.h"

namespace edash_packager {
namespace media {
namespace mp4 {

namespace {

// Generate 64bit IV by default.
const size_t kDefaultIvSize = 8u;
const size_t kCencKeyIdSize = 16u;

// The version of cenc implemented here. CENC 4.
const int kCencSchemeVersion = 0x00010000;

// The default KID for key rotation is all 0s.
const uint8_t kKeyRotationDefaultKeyId[] = {
  0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0
};

COMPILE_ASSERT(arraysize(kKeyRotationDefaultKeyId) == kCencKeyIdSize,
               cenc_key_id_must_be_size_16);

uint64_t Rescale(uint64_t time_in_old_scale,
                 uint32_t old_scale,
                 uint32_t new_scale) {
  return static_cast<double>(time_in_old_scale) / old_scale * new_scale;
}

void GenerateSinf(const EncryptionKey& encryption_key,
                  FourCC old_type,
                  EncryptionMode encryption_mode,
                  ProtectionSchemeInfo* sinf) {
  sinf->format.format = old_type;

  if (encryption_mode == kEncryptionModeAesCtr){
    sinf->type.type = FOURCC_CENC;
  } else if (encryption_mode == kEncryptionModeAesCbc) {
    sinf->type.type = FOURCC_CBC1;
  }

  sinf->type.version = kCencSchemeVersion;
  sinf->info.track_encryption.is_encrypted = true;
  sinf->info.track_encryption.default_iv_size =
      encryption_key.iv.empty() ? kDefaultIvSize : encryption_key.iv.size();
  sinf->info.track_encryption.default_kid = encryption_key.key_id;
}

void GenerateEncryptedSampleEntry(const EncryptionKey& encryption_key,
                                  double clear_lead_in_seconds,
                                  EncryptionMode encryption_mode,
                                  SampleDescription* description) {
  DCHECK(description);
  if (description->type == kVideo) {
    DCHECK_EQ(1u, description->video_entries.size());

    // Add a second entry for clear content if needed.
    if (clear_lead_in_seconds > 0)
      description->video_entries.push_back(description->video_entries[0]);

    // Convert the first entry to an encrypted entry.
    VideoSampleEntry& entry = description->video_entries[0];
    GenerateSinf(encryption_key, entry.format, encryption_mode, &entry.sinf);
    entry.format = FOURCC_ENCV;
  } else {
    DCHECK_EQ(kAudio, description->type);
    DCHECK_EQ(1u, description->audio_entries.size());

    // Add a second entry for clear content if needed.
    if (clear_lead_in_seconds > 0)
      description->audio_entries.push_back(description->audio_entries[0]);

    // Convert the first entry to an encrypted entry.
    AudioSampleEntry& entry = description->audio_entries[0];
    GenerateSinf(encryption_key, entry.format, encryption_mode, &entry.sinf);
    entry.format = FOURCC_ENCA;
  }
}

KeySource::TrackType GetTrackTypeForEncryption(const StreamInfo& stream_info,
                                               uint32_t max_sd_pixels) {
  if (stream_info.stream_type() == kStreamAudio)
    return KeySource::TRACK_TYPE_AUDIO;

  DCHECK_EQ(kStreamVideo, stream_info.stream_type());
  const VideoStreamInfo& video_stream_info =
      static_cast<const VideoStreamInfo&>(stream_info);
  uint32_t pixels = video_stream_info.width() * video_stream_info.height();
  return (pixels > max_sd_pixels) ? KeySource::TRACK_TYPE_HD
                                  : KeySource::TRACK_TYPE_SD;
}

}  // namespace

Segmenter::Segmenter(const MuxerOptions& options,
                     scoped_ptr<FileType> ftyp,
                     scoped_ptr<Movie> moov)
    : options_(options),
      ftyp_(ftyp.Pass()),
      moov_(moov.Pass()),
      moof_(new MovieFragment()),
      fragment_buffer_(new BufferWriter()),
      sidx_(new SegmentIndex()),
      muxer_listener_(NULL),
      progress_listener_(NULL),
      progress_target_(0),
      accumulated_progress_(0),
      sample_duration_(0u) {}

Segmenter::~Segmenter() { STLDeleteElements(&fragmenters_); }

Status Segmenter::Initialize(const std::vector<MediaStream*>& streams,
                             MuxerListener* muxer_listener,
                             ProgressListener* progress_listener,
                             KeySource* encryption_key_source,
                             uint32_t max_sd_pixels,
                             double clear_lead_in_seconds,
                             double crypto_period_duration_in_seconds,
                             EncryptionMode encryption_mode) {
  DCHECK_LT(0u, streams.size());
  muxer_listener_ = muxer_listener;
  progress_listener_ = progress_listener;
  moof_->header.sequence_number = 0;

  moof_->tracks.resize(streams.size());
  segment_durations_.resize(streams.size());
  fragmenters_.resize(streams.size());
  const bool key_rotation_enabled = crypto_period_duration_in_seconds != 0;
  const bool kInitialEncryptionInfo = true;

  for (uint32_t i = 0; i < streams.size(); ++i) {
    stream_map_[streams[i]] = i;
    moof_->tracks[i].header.track_id = i + 1;
    if (streams[i]->info()->stream_type() == kStreamVideo) {
      // Use the first video stream as the reference stream (which is 1-based).
      if (sidx_->reference_id == 0)
        sidx_->reference_id = i + 1;
    }
    if (!encryption_key_source) {
      fragmenters_[i] = new Fragmenter(&moof_->tracks[i]);
      continue;
    }

    KeySource::TrackType track_type =
        GetTrackTypeForEncryption(*streams[i]->info(), max_sd_pixels);
    SampleDescription& description =
        moov_->tracks[i].media.information.sample_table.description;

    if (key_rotation_enabled) {
      // Fill encrypted sample entry with default key.
      EncryptionKey encryption_key;
      encryption_key.key_id.assign(
          kKeyRotationDefaultKeyId,
          kKeyRotationDefaultKeyId + arraysize(kKeyRotationDefaultKeyId));
      GenerateEncryptedSampleEntry(encryption_key, clear_lead_in_seconds,
                                   encryption_mode, &description);
      if (muxer_listener_) {
        muxer_listener_->OnEncryptionInfoReady(
            kInitialEncryptionInfo, encryption_key.key_id,
            encryption_key.key_system_info);
      }

      fragmenters_[i] = new KeyRotationFragmenter(
          moof_.get(), streams[i]->info(), &moof_->tracks[i],
          encryption_key_source, track_type,
          crypto_period_duration_in_seconds * streams[i]->info()->time_scale(),
          clear_lead_in_seconds * streams[i]->info()->time_scale(),
          muxer_listener_, encryption_mode);
      continue;
    }

    scoped_ptr<EncryptionKey> encryption_key(new EncryptionKey());
    Status status =
        encryption_key_source->GetKey(track_type, encryption_key.get());
    if (!status.ok())
      return status;

    GenerateEncryptedSampleEntry(*encryption_key, clear_lead_in_seconds,
                                 encryption_mode, &description);

    if (moov_->pssh.empty()) {
      moov_->pssh.resize(encryption_key->key_system_info.size());
      for (size_t i = 0; i < encryption_key->key_system_info.size(); i++) {
        moov_->pssh[i].raw_box = encryption_key->key_system_info[i].CreateBox();
      }

      if (muxer_listener_) {
        muxer_listener_->OnEncryptionInfoReady(kInitialEncryptionInfo,
                                               encryption_key->key_id,
                                               encryption_key->key_system_info);
      }
    }

    fragmenters_[i] = new EncryptingFragmenter(
        streams[i]->info(), &moof_->tracks[i], encryption_key.Pass(),
        clear_lead_in_seconds * streams[i]->info()->time_scale(),
        encryption_mode);
  }

  // Choose the first stream if there is no VIDEO.
  if (sidx_->reference_id == 0)
    sidx_->reference_id = 1;
  sidx_->timescale = streams[GetReferenceStreamId()]->info()->time_scale();

  // Use media duration as progress target.
  progress_target_ = streams[GetReferenceStreamId()]->info()->duration();

  // Use the reference stream's time scale as movie time scale.
  moov_->header.timescale = sidx_->timescale;
  moof_->header.sequence_number = 1;

  // Fill in version information.
  moov_->metadata.handler.handler_type = FOURCC_ID32;
  moov_->metadata.id3v2.language.code = "eng";
  moov_->metadata.id3v2.private_frame.owner =
      "https://github.com/google/edash-packager";
  moov_->metadata.id3v2.private_frame.value = options_.packager_version_string;
  return DoInitialize();
}

Status Segmenter::Finalize() {
  for (std::vector<Fragmenter*>::iterator it = fragmenters_.begin();
       it != fragmenters_.end();
       ++it) {
    Status status = FinalizeFragment(true, *it);
    if (!status.ok())
      return status;
  }

  // Set tracks and moov durations.
  // Note that the updated moov box will be written to output file for VOD case
  // only.
  for (std::vector<Track>::iterator track = moov_->tracks.begin();
       track != moov_->tracks.end();
       ++track) {
    track->header.duration = Rescale(track->media.header.duration,
                                     track->media.header.timescale,
                                     moov_->header.timescale);
    if (track->header.duration > moov_->header.duration)
      moov_->header.duration = track->header.duration;
  }
  moov_->extends.header.fragment_duration = moov_->header.duration;

  return DoFinalize();
}

Status Segmenter::AddSample(const MediaStream* stream,
                            scoped_refptr<MediaSample> sample) {
  // Find the fragmenter for this stream.
  DCHECK(stream);
  DCHECK(stream_map_.find(stream) != stream_map_.end());
  uint32_t stream_id = stream_map_[stream];
  Fragmenter* fragmenter = fragmenters_[stream_id];

  // Set default sample duration if it has not been set yet.
  if (moov_->extends.tracks[stream_id].default_sample_duration == 0) {
    moov_->extends.tracks[stream_id].default_sample_duration =
        sample->duration();
  }

  if (fragmenter->fragment_finalized()) {
    return Status(error::FRAGMENT_FINALIZED,
                  "Current fragment is finalized already.");
  }

  bool finalize_fragment = false;
  if (fragmenter->fragment_duration() >=
      options_.fragment_duration * stream->info()->time_scale()) {
    if (sample->is_key_frame() || !options_.fragment_sap_aligned) {
      finalize_fragment = true;
    }
  }
  bool finalize_segment = false;
  if (segment_durations_[stream_id] >=
      options_.segment_duration * stream->info()->time_scale()) {
    if (sample->is_key_frame() || !options_.segment_sap_aligned) {
      finalize_segment = true;
      finalize_fragment = true;
    }
  }

  Status status;
  if (finalize_fragment) {
    status = FinalizeFragment(finalize_segment, fragmenter);
    if (!status.ok())
      return status;
  }

  status = fragmenter->AddSample(sample);
  if (!status.ok())
    return status;

  if (sample_duration_ == 0)
    sample_duration_ = sample->duration();
  moov_->tracks[stream_id].media.header.duration += sample->duration();
  segment_durations_[stream_id] += sample->duration();
  DCHECK_GE(segment_durations_[stream_id], fragmenter->fragment_duration());
  return Status::OK;
}

uint32_t Segmenter::GetReferenceTimeScale() const {
  return moov_->header.timescale;
}

double Segmenter::GetDuration() const {
  if (moov_->header.timescale == 0) {
    // Handling the case where this is not properly initialized.
    return 0.0;
  }

  return static_cast<double>(moov_->header.duration) / moov_->header.timescale;
}

void Segmenter::UpdateProgress(uint64_t progress) {
  accumulated_progress_ += progress;

  if (!progress_listener_) return;
  if (progress_target_ == 0) return;
  // It might happen that accumulated progress exceeds progress_target due to
  // computation errors, e.g. rounding error. Cap it so it never reports > 100%
  // progress.
  if (accumulated_progress_ >= progress_target_) {
    progress_listener_->OnProgress(1.0);
  } else {
    progress_listener_->OnProgress(static_cast<double>(accumulated_progress_) /
                                   progress_target_);
  }
}

void Segmenter::SetComplete() {
  if (!progress_listener_) return;
  progress_listener_->OnProgress(1.0);
}

Status Segmenter::FinalizeSegment() {
  Status status = DoFinalizeSegment();

  // Reset segment information to initial state.
  sidx_->references.clear();
  std::vector<uint64_t>::iterator it = segment_durations_.begin();
  for (; it != segment_durations_.end(); ++it)
    *it = 0;

  return status;
}

uint32_t Segmenter::GetReferenceStreamId() {
  DCHECK(sidx_);
  return sidx_->reference_id - 1;
}

Status Segmenter::FinalizeFragment(bool finalize_segment,
                                   Fragmenter* fragmenter) {
  fragmenter->FinalizeFragment();

  // Check if all tracks are ready for fragmentation.
  for (std::vector<Fragmenter*>::iterator it = fragmenters_.begin();
       it != fragmenters_.end();
       ++it) {
    if (!(*it)->fragment_finalized())
      return Status::OK;
  }

  MediaData mdat;
  // Data offset relative to 'moof': moof size + mdat header size.
  // The code will also update box sizes for moof_ and its child boxes.
  uint64_t data_offset = moof_->ComputeSize() + mdat.HeaderSize();
  // 'traf' should follow 'mfhd' moof header box.
  uint64_t next_traf_position = moof_->HeaderSize() + moof_->header.box_size();
  for (size_t i = 0; i < moof_->tracks.size(); ++i) {
    TrackFragment& traf = moof_->tracks[i];
    if (traf.auxiliary_offset.offsets.size() > 0) {
      DCHECK_EQ(traf.auxiliary_offset.offsets.size(), 1u);
      DCHECK(!traf.sample_encryption.sample_encryption_entries.empty());

      next_traf_position += traf.box_size();
      // SampleEncryption 'senc' box should be the last box in 'traf'.
      // |auxiliary_offset| should point to the data of SampleEncryption.
      traf.auxiliary_offset.offsets[0] =
          next_traf_position - traf.sample_encryption.box_size() +
          traf.sample_encryption.HeaderSize() +
          sizeof(uint32_t);  // for sample count field in 'senc'
    }
    traf.runs[0].data_offset = data_offset + mdat.data_size;
    mdat.data_size += fragmenters_[i]->data()->Size();
  }

  // Generate segment reference.
  sidx_->references.resize(sidx_->references.size() + 1);
  fragmenters_[GetReferenceStreamId()]->GenerateSegmentReference(
      &sidx_->references[sidx_->references.size() - 1]);
  sidx_->references[sidx_->references.size() - 1].referenced_size =
      data_offset + mdat.data_size;

  // Write the fragment to buffer.
  moof_->Write(fragment_buffer_.get());
  mdat.WriteHeader(fragment_buffer_.get());
  for (Fragmenter* fragmenter : fragmenters_)
    fragment_buffer_->AppendBuffer(*fragmenter->data());

  // Increase sequence_number for next fragment.
  ++moof_->header.sequence_number;

  if (finalize_segment)
    return FinalizeSegment();

  return Status::OK;
}

}  // namespace mp4
}  // namespace media
}  // namespace edash_packager

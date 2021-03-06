#!/usr/bin/python
#
# Copyright 2014 Google Inc. All Rights Reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file or at
# https://developers.google.com/open-source/licenses/bsd

"""Test wrapper for the sample packager binary."""


import os
import subprocess

import test_env


class PackagerApp(object):
  """Main integration class for testing the packager binary."""

  def __init__(self):
    self.build_dir = os.path.join(test_env.SRC_DIR, 'out',
                                  test_env.options.build_type)
    self.binary = os.path.join(self.build_dir, 'packager')

  def BuildSrc(self):
    return subprocess.call(['ninja', '-C', self.build_dir])

  def DumpStreamInfo(self, stream):
    input_str = 'input=%s' % stream
    cmd = [self.binary, input_str, '--dump_stream_info']
    return subprocess.check_output(cmd)

  def Version(self):
    output = subprocess.check_output([self.binary])
    # The output should of the form:
    #   edash-packager version xxx: Description...
    # We consider everything before ':' part of version.
    return output.split(':')[0]

  def Package(self, streams, flags=None):
    if flags is None:
      flags = []
    cmd = [self.binary]
    cmd.extend(streams)
    cmd.extend(flags)
    assert 0 == subprocess.call(cmd)

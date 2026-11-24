# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Groq text-to-speech pure pieces: the chunker, WAV parser, request-body
builder and >voice= command parser (muse_tts_groq.c, host build), checked
against hand-built WAV headers and JSON. No network, no credentials, no
device: the fetch task itself is ESP-IDF only and is covered by the build,
not by this test."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from host_cjson import cjson_source

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class GroqTts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        result = subprocess.run(
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
             '-DMUSE_TTS_GROQ_HOST_TEST',
             '-I', str(ROOT / 'components/muse'),
             '-include', str(ROOT / 'tests/host_compat.h'),
             *cjson_source(JSON),
             str(ROOT / 'components/muse/muse_tts_groq.c'),
             str(ROOT / 'tests/test_muse_tts_groq_harness.c'),
             '-o', str(out / 'harness')],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'harness'

    def test_chunker_wav_body_and_voice_command(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('tts groq harness: PASS', result.stdout)

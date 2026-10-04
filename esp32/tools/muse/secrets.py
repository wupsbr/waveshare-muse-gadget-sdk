#!/usr/bin/env python3
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

"""Puts the build's secrets into a generated sdkconfig, never into git.

    tools/muse/secrets.py BUILD_DIR/sdkconfig

Each secret comes from an environment variable or, failing that, a one-line
file in the repository's secrets/ directory (gitignored; see
secrets/README.md). A missing secret is left as it is. Values are never
printed, only which ones were set.
"""

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SECRETS = (
    # (Kconfig option, environment variable, file in secrets/)
    ("CONFIG_GADGET_SDK_TOKEN", "MUSE_SDK_TOKEN", "muse_sdk_token"),
    ("CONFIG_MUSE_ELEVENLABS_API_KEY", "ELEVENLABS_API_KEY", "elevenlabs_api_key"),
    ("CONFIG_MUSE_ELEVENLABS_VOICE_ID", "ELEVENLABS_VOICE_ID", "elevenlabs_voice_id"),
)


def value(env, name):
    v = os.environ.get(env, "").strip()
    if not v:
        f = ROOT / "secrets" / name
        v = f.read_text().strip() if f.is_file() else ""
    if '"' in v or "\n" in v:
        sys.exit(f"secrets: {name} must be one line without quotes")
    return v


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    cfg = Path(sys.argv[1])
    cfg.parent.mkdir(parents=True, exist_ok=True)
    text = cfg.read_text() if cfg.exists() else ""
    done = []
    for opt, env, name in SECRETS:
        v = value(env, name)
        if not v:
            continue
        line = f'{opt}="{v}"'
        pattern = re.compile(rf"^(# )?{opt}[= ].*$", re.M)
        text = pattern.sub(line, text, count=1) if pattern.search(text) else text + line + "\n"
        done.append(opt)
    if done:
        cfg.write_text(text)
        cfg.chmod(0o600)
    print("secrets: " + (", ".join(done) if done else "none found (see secrets/README.md)"))


if __name__ == "__main__":
    main()

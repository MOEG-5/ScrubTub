#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Run inside Dockerfile.linux with /repo read-only and a fresh /work bind mount.
set -euo pipefail
export SCRUBTUB_MEDIA_ROOT=/work/media
export PYTHONDONTWRITEBYTECODE=1
export QT_QPA_PLATFORM=offscreen
export XDG_DATA_HOME=/work/test-profile/data
export XDG_CONFIG_HOME=/work/test-profile/config
export XDG_CACHE_HOME=/work/test-profile/cache
[[ ! -e /work/stage ]] || { echo 'Use a fresh work directory; stage already exists.' >&2; exit 1; }
bash /repo/scripts/release/build-media.sh
cmake -S /repo -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSCRUBTUB_DEPLOY_QT=ON -DQT_DEPLOY_USE_PATCHELF=ON
cmake --build /work/build -j "${SCRUBTUB_BUILD_JOBS:-8}"
cmake --install /work/build --prefix /work/stage
python3 /repo/scripts/release/stage-linux-qt.py /work/stage
python3 /repo/scripts/release/stage-linux-media.py /work/stage/bin/media --prefix /work/media/prefix
python3 /repo/scripts/release/smoke-linux-bundle.py /work/stage
export SCRUBTUB_RELEASE_MEDIA_DIR=/work/stage/bin/media
export SCRUBTUB_FFMPEG_PATH=/work/stage/bin/media/ffmpeg
export SCRUBTUB_FFPROBE_PATH=/work/stage/bin/media/ffprobe
export SCRUBTUB_MPV_PATH=/work/stage/bin/media/mpv
ctest --test-dir /work/build --output-on-failure -j 4
python3 -m unittest discover -s /repo/scripts/release/tests
python3 /repo/scripts/release/collect-linux-compliance.py /work/stage /work/sources --media-root /work/media
python3 /repo/scripts/release/export-source.py /work/sources/scrubtub
# Raw test logs can contain names from the developer's private media corpus.
printf '%s\n' 'CTest, release-gate tests and packaged GUI startup passed.' \
    'Corpus-dependent cases skip when local test media is unavailable.' \
    > /work/sources/VALIDATION.txt
python3 /repo/scripts/release/package-release.py /work/stage /work/sources /work/artifacts

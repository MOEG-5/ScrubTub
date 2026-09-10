#!/usr/bin/env python3
"""Collect the exact private-media source archives, notices and build recipes.

Additional packaged dependencies (Qt, libass, etc.) require their own materials.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
root = repo / "out/media"
dest = args.destination.resolve()
dest.mkdir(parents=True, exist_ok=True)
sources = dest / "media-sources"
sources.mkdir(exist_ok=False)
components = {
    "ffmpeg-9.0.1": "ffmpeg-9.0.1.tar.xz",
    "mpv-0.41.0": "mpv-0.41.0.tar.gz",
    "dav1d-1.5.4": "dav1d-1.5.4.tar.gz",
    "libplacebo-7.360.1": "libplacebo-7.360.1.tar.gz",
}
checksums = {}
for name, archive in components.items():
    source = root / "sources" / archive
    shutil.copy2(source, sources / archive)
    checksums[archive] = hashlib.sha256(source.read_bytes()).hexdigest()
    notices = dest / "licenses" / name
    notices.mkdir(parents=True, exist_ok=False)
    for file in (root / name).iterdir():
        if file.is_file() and file.name.startswith(("LICENSE", "COPYING", "Copyright", "COPYRIGHT")):
            shutil.copy2(file, notices / file.name)
    subprocess.run([sys.executable, str(Path(__file__).with_name('collect-embedded-notices.py')),
                    str(root / name), str(notices / 'SOURCE-NOTICES.txt')], check=True)
    if not list(notices.iterdir()):
        raise RuntimeError(f"Missing license texts for {name}")
shutil.copytree(root / "manifests", sources / "build-manifests")
shutil.copy2(repo / "scripts/release/build-media.sh", sources / "build-media.sh")
(sources / "SHA256.json").write_text(json.dumps(checksums, indent=2) + "\n")
(dest / "README.txt").write_text(
    "ScrubTub private media build materials\n\n"
    "These source archives are unmodified. Build commands and configurations are\n"
    "in media-sources/build-manifests. The build script belongs at\n"
    "scripts/release/build-media.sh in the matching ScrubTub source checkout.\n"
    "See docs/RELEASING.md for prerequisites and reproduction instructions.\n\n"
    "This is a validation artifact, not a completed public release. These archives\n"
    "cover FFmpeg, mpv, dav1d and libplacebo only. Before public distribution,\n"
    "complete the corresponding-source and notices inventory for Qt, libass,\n"
    "zlib and all other packaged runtimes recorded in the runtime manifest.\n"
)
print(f"Collected private media sources and notices in {dest}")

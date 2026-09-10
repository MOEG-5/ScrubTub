#!/usr/bin/env python3
"""Stage private Linux media tools and their runtime closure, without system writes.

This is a local validation bundle, not a license-complete public release.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path, help="New staging directory (must not exist)")
parser.add_argument("--prefix", type=Path, default=repo / "out/media/prefix")
args = parser.parse_args()
prefix = args.prefix.resolve()
dest = args.destination.resolve()
dest.mkdir(parents=True, exist_ok=False)
libdir = dest / "lib"
libdir.mkdir()
env = os.environ | {"LD_LIBRARY_PATH": str(prefix / "lib")}
# Leave the platform C runtime and loader to the target Linux system.
system = re.compile(r"^(ld-linux.*|lib(c|m|dl|pthread|rt|resolv)\.so\..*)$")
queue = []
manifest = []
for name in ("ffmpeg", "ffprobe", "mpv"):
    source = prefix / "bin" / name
    target = dest / name
    shutil.copy2(source, target)
    queue.append((source, target))
seen = set()
while queue:
    source, target = queue.pop(0)
    manifest.append({"file": str(target.relative_to(dest)), "origin": str(source.resolve())})
    result = subprocess.run(["ldd", str(source)], env=env, text=True, capture_output=True, check=True)
    if "not found" in result.stdout:
        raise RuntimeError(result.stdout)
    for line in result.stdout.splitlines():
        match = re.match(r"\s*([^/\s]+) => (/\S+) \(", line)
        if not match:
            continue
        name, location = match.groups()
        if system.match(name) or name in seen:
            continue
        seen.add(name)
        dependency = Path(location)
        copied = libdir / name
        shutil.copy2(dependency, copied)
        queue.append((dependency, copied))
    subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/lib" if target.parent == dest else "$ORIGIN", str(target)], check=True)
(dest / "runtime-origins.json").write_text(json.dumps(manifest, indent=2) + "\n")
for name in ("ffmpeg", "ffprobe", "mpv"):
    subprocess.run([str(dest / name), *(["--no-config", "--version"] if name == "mpv" else ["-version"])],
                   env=os.environ | {"LD_LIBRARY_PATH": ""}, check=True, capture_output=True)
print(f"Staged {len(manifest)} files, {sum((dest / x['file']).stat().st_size for x in manifest) / 1024**2:.1f} MiB")

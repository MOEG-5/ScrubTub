#!/usr/bin/env python3
"""Complete a Windows validation stage with private media tools and DLLs.

Run with the native UCRT64 Python in MSYS2 after cmake --install. This does
not publish a release; runtime origins still need a corresponding-source audit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("stage", type=Path)
parser.add_argument("--dependencies", type=Path, required=True, help="Native path to UCRT64 prefix")
args = parser.parse_args()
if os.name != "nt":
    parser.error("Run on Windows with native MSYS2 UCRT64 Python")
repo = Path(__file__).resolve().parents[2]
private = repo / "out/media/prefix/bin"
stage = args.stage.resolve()
app_bin = stage / "bin"
if not (app_bin / "scrubtub.exe").is_file():
    parser.error("Install ScrubTub and Qt to the stage first")
media = app_bin / "media"
media.mkdir(exist_ok=False)
for name in ("ffmpeg.exe", "ffprobe.exe", "mpv.exe"):
    shutil.copy2(private / name, media / name)
search = [private, args.dependencies / "bin"]
indexes = [{p.name.lower(): p for p in root.glob("*.dll")} for root in search]
system32 = Path(os.environ["SystemRoot"]) / "System32"
origins = []
# Each executable has its own DLL directory. Qt plugin DLL dependencies go
# next to scrubtub.exe, where the Windows loader can find them.
for root, output in ((stage, app_bin), (media, media)):
    queue = [p for p in root.rglob("*") if p.suffix.lower() in (".exe", ".dll")
             and (root == media or media not in p.parents)]
    visited = set()
    while queue:
        binary = queue.pop(0)
        if binary in visited:
            continue
        visited.add(binary)
        imports = subprocess.check_output(["objdump", "-p", str(binary)], text=True)
        for name in re.findall(r"DLL Name:\s*(\S+)", imports):
            key = name.lower()
            existing = next((p for p in output.glob("*.dll") if p.name.lower() == key), None)
            if existing:
                queue.append(existing)
                continue
            source = next((index[key] for index in indexes if key in index), None)
            if source is None:
                if key.startswith(("api-ms-win-", "ext-ms-win-")) or (system32 / name).is_file():
                    continue
                raise RuntimeError(f"Unresolved DLL {name} needed by {binary}")
            target = output / name
            shutil.copy2(source, target)
            origins.append({"file": str(target.relative_to(stage)), "origin": str(source),
                            "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
            queue.append(target)
(stage / "runtime-origins.json").write_text(json.dumps(origins, indent=2) + "\n")
# The compliance collector adds notices for the files actually shipped.
# Prove the media executables start without the build environment's DLL PATH.
clean = os.environ | {"PATH": str(system32)}
for name in ("ffmpeg", "ffprobe", "mpv"):
    subprocess.run([str(media / (name + ".exe")), *(["--no-config", "--version"] if name == "mpv" else ["-version"])],
                   env=clean, check=True, capture_output=True)
print(f"Windows validation stage: {stage}")

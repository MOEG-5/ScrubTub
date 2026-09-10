#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify and create paired binary/source downloads plus SHA256SUMS.txt."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tarfile
import zipfile

spec = importlib.util.spec_from_file_location('release_gate', Path(__file__).with_name('verify-release.py'))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def tar(source, target, name):
    with target.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0, filename='') as compressed:
        with tarfile.open(fileobj=compressed, mode='w|') as archive:
            def normalize(info):
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ''
                return info
            archive.add(source, arcname=name, filter=normalize)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('stage', type=Path)
parser.add_argument('sources', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--version', default='0.1.0')
args = parser.parse_args()
if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[a-zA-Z0-9.]+)?', args.version):
    parser.error('Invalid release version')
gate.verify(args.stage.resolve(), args.sources.resolve())
manifest = json.loads((args.stage / 'share/scrubtub/DEPENDENCIES.json').read_text())
platform = manifest['platform']
if platform not in ('linux-x86_64', 'windows-x86_64'):
    parser.error('Unknown platform')
base = f'scrubtub-{args.version}-{platform}'
args.output.mkdir(parents=True, exist_ok=False)
if platform.startswith('windows'):
    binary = args.output / (base + '.zip')
    with zipfile.ZipFile(binary, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(args.stage.rglob('*')):
            if path.is_file():
                archive.write(path, base + '/' + path.relative_to(args.stage).as_posix())
else:
    binary = args.output / (base + '.tar.gz')
    tar(args.stage, binary, base)
source = args.output / (base + '-sources.tar.gz')
tar(args.sources, source, base + '-sources')
checksums = args.output / 'SHA256SUMS.txt'
checksums.write_text(''.join(f'{gate.digest(p)}  {p.name}\n' for p in (binary, source)))
notes = f'''# ScrubTub {args.version} — {platform}

A free video catalogue with thumbnails and silent live scrubbing. Opening a
video launches your default player. Qt, mpv, FFmpeg and ffprobe are included.

- [{binary.name}](https://github.com/MOEG-5/ScrubTub/releases/download/v{args.version}/{binary.name}) — application download
- [{source.name}](https://github.com/MOEG-5/ScrubTub/releases/download/v{args.version}/{source.name}) — matching application and dependency sources,
  including distribution patches and build recipes
- [SHA256SUMS.txt](https://github.com/MOEG-5/ScrubTub/releases/download/v{args.version}/SHA256SUMS.txt) — download checksums

Extract the entire application archive and start `bin/scrubtub{'.exe' if platform.startswith('windows') else ''}`.
Keep the folders together. On Linux this build requires x86-64 and glibc 2.41 or
newer and X11 or XWayland; it was built on Debian 13. Windows builds target 64-bit Windows 10 (1809 or later) and Windows 11.

ScrubTub is GPL-3.0-only. Dependency copyright and license notices are in
`share/scrubtub/`; they can also be opened from Settings → About and licenses.
The source download above is the corresponding-source companion to this binary.
Both downloads must remain available together. Provided without warranty.

The exact dependency inventory is in `share/scrubtub/DEPENDENCIES.json`.
'''
(args.output / 'RELEASE_NOTES.md').write_text(notes, encoding='utf-8')
for path in (binary, source, checksums):
    print(f'{path.name}: {path.stat().st_size / 1024**2:.1f} MiB')

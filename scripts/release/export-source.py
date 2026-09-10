#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Export the current checkout, including reviewed uncommitted release changes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
dest = args.destination.resolve()
dest.mkdir(parents=True, exist_ok=False)
files = subprocess.check_output(['git', '-C', str(repo), 'ls-files', '--cached', '--others', '--exclude-standard', '-z']).decode().split('\0')
hashes = {}
for name in sorted(set(files) - {''}):
    relative = Path(name)
    if relative.parts[0] in ('vids', 'out', '.git') or relative.parts[0].startswith('build'):
        raise RuntimeError(f'Refusing to export local media/build content: {name}')
    source = repo / relative
    if not source.exists():
        continue
    if source.is_symlink():
        raise RuntimeError(f'Review source symlink before export: {name}')
    target = dest / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    hashes[name] = hashlib.sha256(target.read_bytes()).hexdigest()
(dest.parent / 'application-source.json').write_text(json.dumps(hashes, indent=2) + '\n')
print(f'Exported {len(hashes)} source files; no Git history, media or build outputs.')

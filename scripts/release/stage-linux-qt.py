#!/usr/bin/env python3
"""Supplement Qt's CMake deployment with desktop plugins and their ELF closure."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('stage', type=Path)
args = parser.parse_args()
stage = args.stage.resolve()
plugins = Path(subprocess.check_output(['qtpaths6', '--query', 'QT_INSTALL_PLUGINS'], text=True).strip())
required = ['platforms/libqxcb.so', 'platforms/libqoffscreen.so',
            'imageformats/libqjpeg.so', 'imageformats/libqsvg.so']
selected = [plugins / name for name in required]
selected += sorted((plugins / 'xcbglintegrations').glob('*.so'))
selected += sorted((plugins / 'platforminputcontexts').glob('*.so'))
queue = [(source, stage / 'plugins' / source.relative_to(plugins)) for source in selected]
seen = set()
system = re.compile(r'^(ld-linux.*|lib(c|m|dl|pthread|rt|resolv)\.so\..*)$')
while queue:
    source, target = queue.pop(0)
    if target in seen:
        continue
    seen.add(target)
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        rpath = '$ORIGIN/' + os.path.relpath(stage / 'lib', target.parent)
        subprocess.run(['patchelf', '--set-rpath', rpath, str(target)], check=True)
    result = subprocess.check_output(['ldd', str(source)], text=True)
    if 'not found' in result:
        raise RuntimeError(result)
    for name, location in re.findall(r'^\s*([^/\s]+) => (/\S+) \(', result, re.M):
        if not system.match(name):
            queue.append((Path(location), stage / 'lib' / name))
print(f'Checked {len(seen)} Qt plugin/runtime files')

# Debian's Qt deployment can leave indirect libraries with no RUNPATH.
# Set every application/QML ELF's path, including files Qt already copied.
elfs = []
for path in stage.rglob('*'):
    if not path.is_file() or path.is_symlink() or path.is_relative_to(stage / 'bin/media'):
        continue
    with path.open('rb') as stream:
        if stream.read(4) != b'\x7fELF':
            continue
    rpath = '$ORIGIN/' + os.path.relpath(stage / 'lib', path.parent)
    subprocess.run(['patchelf', '--set-rpath', rpath, str(path)], check=True)
    elfs.append(path)
for path in elfs:
    result = subprocess.check_output(['ldd', str(path)], text=True)
    if 'not found' in result:
        raise RuntimeError(f'{path}: {result}')
    for name, location in re.findall(r'^\s*([^/\s]+) => (/\S+) \(', result, re.M):
        if not system.match(name) and not Path(location).resolve().is_relative_to(stage):
            raise RuntimeError(f'{path} uses an unbundled library: {name} => {location}')
print(f'Verified relative runtime resolution for {len(elfs)} application and Qt ELF files')

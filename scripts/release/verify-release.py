#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fail closed if runtime files, notices, or corresponding sources are missing."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def beneath(root, name):
    path = root / name
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f'Path escapes artifact: {name}')
    return path


def verify(stage, sources):
    manifest = json.loads((stage / 'share/scrubtub/DEPENDENCIES.json').read_text())
    if manifest.get('format') != 1 or not manifest.get('components'):
        raise ValueError('Missing dependency inventory')
    for name, expected in manifest['source_files'].items():
        path = beneath(sources, name)
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'Missing or changed dependency source: {name}')
    for name, info in manifest['components'].items():
        source = beneath(sources, info['source_path'])
        notice = beneath(stage, info['notice_path'])
        if not source.exists() or not notice.is_dir() or not any(p.is_file() for p in notice.rglob('*')):
            raise ValueError(f'Missing source/notice for {name}')
        supplied = [key for key in manifest['source_files']
                    if key == info['source_path'] or key.startswith(info['source_path'].rstrip('/') + '/')]
        if not supplied:
            raise ValueError(f'Unverified source material for {name}')
    if not manifest.get('notice_files'):
        raise ValueError('Missing notice checksums')
    for name, expected in manifest['notice_files'].items():
        path = beneath(stage, name)
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'Missing or changed notice: {name}')
    known = set()
    for record in manifest['files']:
        name = record['file']
        path = beneath(stage, name)
        if name in known:
            raise ValueError(f'Duplicate file record: {name}')
        known.add(name)
        if 'symlink' in record:
            if not path.is_symlink() or str(path.readlink()) != record['symlink']:
                raise ValueError(f'Changed symlink: {name}')
        elif not path.is_file() or digest(path) != record['sha256']:
            raise ValueError(f'Missing or changed runtime: {name}')
        elif record['component'] != 'ScrubTub' and record['component'] not in manifest['components']:
            raise ValueError(f'Unmapped runtime: {name}')
    for path in stage.rglob('*'):
        name = path.relative_to(stage).as_posix()
        if path.is_symlink() and name not in known:
            raise ValueError(f'Uninventoried symlink: {name}')
        if not path.is_file():
            continue
        if name in known:
            continue
        # Only generated human-readable notices belong outside the runtime map.
        if not name.startswith('share/scrubtub/'):
            raise ValueError(f'Uninventoried file: {name}')
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic == b'\x7fELF' or magic.startswith(b'MZ'):
            raise ValueError(f'Executable hidden in notices: {name}')
    for name in ('LICENSE', 'THIRD_PARTY.md', 'DEPENDENCIES.md'):
        if not (stage / 'share/scrubtub' / name).is_file():
            raise ValueError(f'Missing release notice: {name}')
    app_files = json.loads((sources / 'application-source.json').read_text())
    if 'CMakeLists.txt' not in app_files or 'LICENSE' not in app_files:
        raise ValueError('Incomplete application source')
    for name, expected in app_files.items():
        path = beneath(sources / 'scrubtub', name)
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'Missing or changed application source: {name}')
    print(f"Verified {len(known)} runtime entries, {len(manifest['components'])} dependencies, and {len(app_files)} application source files.")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    parser.add_argument('sources', type=Path)
    args = parser.parse_args()
    verify(args.stage.resolve(), args.sources.resolve())

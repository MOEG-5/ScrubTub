#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Match a Debian-built stage to installed packages and collect exact sources.

Run in the release builder. Never substitutes a similarly named upstream release
for a distribution package. Unmapped files or unavailable sources stop packaging.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs)


def build_id(path):
    result = subprocess.run(['readelf', '-n', str(path)], text=True, capture_output=True)
    match = re.search(r'Build ID: (\w+)', result.stdout)
    return match.group(1) if match else None


def verify_dsc(path):
    fields = path.read_text()
    match = re.search(r'^Checksums-Sha256:\n((?: .+\n)+)', fields, re.M)
    if not match:
        raise RuntimeError(f'No SHA-256 source checksums: {path}')
    for line in match.group(1).splitlines():
        expected, size, name = line.split()
        if Path(name).name != name:
            raise RuntimeError(f'Unsafe source filename: {name}')
        source = path.parent / name
        if source.stat().st_size != int(size) or digest(source) != expected:
            raise RuntimeError(f'Source checksum mismatch: {source}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    parser.add_argument('sources', type=Path)
    parser.add_argument('--media-root', type=Path, required=True)
    args = parser.parse_args()
    stage, sources, media = args.stage.resolve(), args.sources.resolve(), args.media_root.resolve()
    sources.mkdir(parents=True, exist_ok=True)
    notices = stage / 'share/scrubtub/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    metadata = {}
    fmt = '${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\t${Homepage}\n'
    for line in run('dpkg-query', '-W', '-f=' + fmt).splitlines():
        package, version, source, source_version, homepage = line.split('\t')
        metadata[package] = dict(package=package, version=version, source=source,
                                 source_version=source_version, homepage=homepage)
    candidates = defaultdict(list)
    for listing in Path('/var/lib/dpkg/info').glob('*.list'):
        package = listing.name[:-5]
        if package not in metadata:
            continue
        for name in listing.read_text().splitlines():
            path = Path(name)
            if name.startswith(('/usr/', '/lib/')) and path.is_file():
                candidates[path.name].append((path, package))
    media_origins = {item['file']: Path(item['origin']) for item in
                     json.loads((stage / 'bin/media/runtime-origins.json').read_text())}
    records, packages, ids, hashes = [], set(), {}, {}
    for path in sorted(stage.rglob('*')):
        relative = path.relative_to(stage).as_posix()
        if path.is_symlink():
            if not path.resolve().is_relative_to(stage):
                raise RuntimeError(f'Symlink escapes stage: {relative}')
            records.append(dict(file=relative, symlink=os.readlink(path)))
            continue
        if not path.is_file() or relative.startswith('share/scrubtub/'):
            continue
        component = None
        origin = None
        if relative == 'bin/scrubtub':
            component = 'ScrubTub'
        elif relative in ('bin/qt.conf', 'bin/media/runtime-origins.json'):
            component = 'ScrubTub'
        elif relative.startswith('bin/media/'):
            origin = media_origins.get(relative[len('bin/media/'):])
            if origin and origin.is_relative_to(media / 'prefix'):
                if path.name.startswith(('ffmpeg', 'ffprobe', 'libav', 'libsw')):
                    component = 'FFmpeg'
                elif path.name.startswith('libdav1d'):
                    component = 'dav1d'
                elif path.name.startswith('libplacebo'):
                    component = 'libplacebo'
                elif path.name == 'mpv':
                    component = 'mpv'
        if component is None:
            with path.open('rb') as stream:
                elf = stream.read(4) == b'\x7fELF'
            identity = build_id(path) if elf else digest(path)
            if not identity:
                raise RuntimeError(f'Missing ELF build ID: {relative}')
            for candidate, package in candidates[(origin or path).name]:
                if origin and candidate.resolve() != origin.resolve():
                    continue
                cache = ids if elf else hashes
                if candidate not in cache:
                    cache[candidate] = build_id(candidate) if elf else digest(candidate)
                if cache[candidate] == identity:
                    component = package
                    packages.add(package)
                    break
            if component is None:
                raise RuntimeError(f'No matching installed package content/build ID: {relative}')
        records.append(dict(file=relative, component=component, sha256=digest(path)))

    components = {}
    # Download-only apt verifies source records against the signed repository
    # indexes. Each .dsc is also checked against every downloaded source member.
    for package in sorted(packages):
        info = metadata[package]
        source_dir = sources / 'debian' / info['source']
        source_dir.mkdir(parents=True, exist_ok=True)
        existing = list(source_dir.glob('*.dsc'))
        if not existing:
            command = ['apt-get', 'source', '--download-only', '--only-source',
                       info['source'] + '=' + info['source_version']]
            result = subprocess.run(command, cwd=source_dir, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (source_dir / 'download.log').write_text(result.stdout)
            if result.returncode:
                raise RuntimeError(f'Source download failed for {package}:\n{result.stdout}')
        dscs = list(source_dir.glob('*.dsc'))
        if len(dscs) != 1:
            raise RuntimeError(f'Expected one source descriptor for {package}')
        dsc = dscs[0]
        if not re.search(r'^Version: ' + re.escape(info['source_version']) + r'$', dsc.read_text(), re.M):
            raise RuntimeError(f'Source version mismatch for {package}')
        verify_dsc(dsc)
        copyright_file = Path('/usr/share/doc') / package.split(':')[0] / 'copyright'
        if not copyright_file.is_file():
            raise RuntimeError(f'Missing copyright notice for {package}')
        target = notices / package.replace(':', '_')
        target.mkdir(exist_ok=True)
        shutil.copy2(copyright_file, target / 'copyright')
        components[package] = info | dict(source_path=str(source_dir.relative_to(sources)),
                                         notice_path=str(target.relative_to(stage)))
        print(f"Collected {package}: {info['source']} {info['source_version']}", flush=True)
    shutil.copytree('/usr/share/common-licenses', notices / 'common-licenses', dirs_exist_ok=True)
    private = {
        'FFmpeg': ('ffmpeg-9.0.1', 'ffmpeg-9.0.1.tar.xz', 'GPL-3.0-or-later'),
        'mpv': ('mpv-0.41.0', 'mpv-0.41.0.tar.gz', 'GPL-3.0-or-later'),
        'dav1d': ('dav1d-1.5.4', 'dav1d-1.5.4.tar.gz', 'BSD-2-Clause'),
        'libplacebo': ('libplacebo-7.360.1', 'libplacebo-7.360.1.tar.gz', 'LGPL-2.1-or-later'),
    }
    private_dir = sources / 'private-media'
    private_dir.mkdir(exist_ok=True)
    for name, (directory, archive, license_id) in private.items():
        shutil.copy2(media / 'sources' / archive, private_dir / archive)
        target = notices / name
        target.mkdir(exist_ok=True)
        for path in (media / directory).iterdir():
            if path.is_file() and path.name.startswith(('LICENSE', 'COPYING', 'Copyright', 'COPYRIGHT')):
                shutil.copy2(path, target / path.name)
        subprocess.run([sys.executable, str(Path(__file__).with_name('collect-embedded-notices.py')),
                        str(media / directory), str(target / 'SOURCE-NOTICES.txt')], check=True)
        if not list(target.iterdir()):
            raise RuntimeError(f'Missing private component notices: {name}')
        components[name] = dict(version=directory, license=license_id,
                               source_path='private-media/' + archive,
                               notice_path=str(target.relative_to(stage)))
    shutil.copytree(media / 'manifests', private_dir / 'build-manifests', dirs_exist_ok=True)
    # Header-only code is part of the executable even though ldd cannot see it.
    rapid = sources / 'rapidfuzz-cpp-3.3.4.tar.gz'
    if not rapid.exists():
        urllib.request.urlretrieve('https://codeload.github.com/rapidfuzz/rapidfuzz-cpp/tar.gz/refs/tags/v3.3.4', rapid)
    if digest(rapid) != 'a0dd2ef361cac165e12076696e7c7e8d069a2908abd9599ad4bd190de33f9881':
        raise RuntimeError('RapidFuzz archive checksum mismatch')
    target = notices / 'RapidFuzz'
    target.mkdir(exist_ok=True)
    with tarfile.open(rapid) as archive:
        member = next(m for m in archive.getmembers() if m.name.endswith('/LICENSE'))
        (target / 'LICENSE').write_bytes(archive.extractfile(member).read())
    components['RapidFuzz'] = dict(version='3.3.4', license='MIT', source_path=rapid.name,
                                  notice_path=str(target.relative_to(stage)))
    (sources / 'installed-packages.tsv').write_text(run('dpkg-query', '-W', '-f=' + fmt))
    shutil.copytree('/etc/apt/sources.list.d', sources / 'apt-sources', dirs_exist_ok=True)
    manifest = dict(format=1, platform='linux-x86_64', components=components, files=records,
                    source_files={str(p.relative_to(sources)): digest(p)
                                  for p in sorted(sources.rglob('*')) if p.is_file()})
    manifest['notice_files'] = {p.relative_to(stage).as_posix(): digest(p)
                                for p in (stage / 'share/scrubtub').rglob('*')
                                if p.is_file() and p.name not in ('DEPENDENCIES.json', 'DEPENDENCIES.md')}
    (stage / 'share/scrubtub/DEPENDENCIES.json').write_text(json.dumps(manifest, indent=2) + '\n')
    lines = ['# Bundled dependency inventory', '',
             'Exact corresponding sources and Debian patches/build rules are in the companion source archive.',
             'License and copyright texts are in `licenses/`; Debian notices reference `licenses/common-licenses/`.', '',
             '| Component | Version | Source materials |', '| --- | --- | --- |']
    for name, info in sorted(components.items()):
        lines.append(f"| {name} | {info['version']} | {info['source_path']} |")
    (stage / 'share/scrubtub/DEPENDENCIES.md').write_text('\n'.join(lines) + '\n')
    print(f'Covered {len(records)} runtime files and {len(components)} external components.')


if __name__ == '__main__':
    main()

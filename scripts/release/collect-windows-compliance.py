#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Collect signed, exact MSYS2 source packages for every staged runtime file."""
import argparse
from collections import defaultdict
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    parser.add_argument('sources', type=Path)
    parser.add_argument('--dependencies', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Run with native UCRT64 Python on Windows')
    repo = Path(__file__).resolve().parents[2]
    stage, sources = args.stage.resolve(), args.sources.resolve()
    msys = args.dependencies.resolve().parent
    sources.mkdir(parents=True, exist_ok=True)
    notices = stage / 'share/scrubtub/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    metadata, candidates = {}, defaultdict(list)
    for directory in (msys / 'var/lib/pacman/local').iterdir():
        if not (directory / 'desc').is_file():
            continue
        fields = {}
        for part in (directory / 'desc').read_text(encoding='utf-8').split('\n\n'):
            lines = part.splitlines()
            if lines:
                fields[lines[0].strip('%')] = lines[1:]
        name = fields['NAME'][0]
        metadata[name] = fields
        for relative in (directory / 'files').read_text(encoding='utf-8').splitlines():
            path = msys / relative
            if relative.startswith('ucrt64/') and path.is_file():
                candidates[path.name.lower()].append((path, name))
    records, packages, hashes = [], set(), {}
    private = repo / 'out/media/prefix/bin'
    for path in sorted(stage.rglob('*')):
        relative = path.relative_to(stage).as_posix()
        if not path.is_file() or relative.startswith(('share/scrubtub/', 'third-party/')):
            continue
        identity = sha(path)
        component = None
        if relative in ('bin/scrubtub.exe', 'bin/qt.conf', 'runtime-origins.json'):
            component = 'ScrubTub'
        elif relative.startswith('bin/media/') and (private / path.name).is_file() and sha(private / path.name) == identity:
            name = path.name.lower()
            if name.startswith(('ffmpeg', 'ffprobe', 'avcodec-', 'avfilter-', 'avformat-', 'avutil-', 'swscale-', 'swresample-')):
                component = 'FFmpeg'
            elif 'dav1d' in name:
                component = 'dav1d'
            elif 'placebo' in name:
                component = 'libplacebo'
            elif name == 'mpv.exe' or name.startswith('libmpv-'):
                component = 'mpv'
        if component is None:
            for candidate, package in candidates[path.name.lower()]:
                hashes.setdefault(candidate, sha(candidate))
                if hashes[candidate] == identity:
                    component = package
                    packages.add(package)
                    break
        if component is None and path.suffix == '.qm' and 'translations' in relative:
            component = next((p for p in metadata if p.endswith('-qt6-translations')), None)
            if component:
                packages.add(component)
        if component is None:
            raise RuntimeError(f'Unmapped runtime file: {relative}')
        records.append(dict(file=relative, component=component, sha256=identity))
    # MinGW startup code and inline headers also enter the executable, although
    # they are not visible in its DLL imports. Preserve their sources/notices.
    static_packages = {'mingw-w64-ucrt-x86_64-crt', 'mingw-w64-ucrt-x86_64-headers'}
    if not static_packages.issubset(metadata):
        raise RuntimeError('Missing MinGW CRT/header provenance')
    packages.update(static_packages)
    # Use a disposable MSYS2 trust database and its packaged revocation policy,
    # without changing the installation's keyring.
    key_home = tempfile.TemporaryDirectory(prefix='scrubtub-source-keyring-', ignore_cleanup_errors=True)
    unix_key_home = subprocess.check_output([str(msys / 'usr/bin/cygpath.exe'), '-u', key_home.name], text=True).strip()
    bash = str(msys / 'usr/bin/bash.exe')
    subprocess.run([bash, '-c', 'pacman-key --gpgdir "$1" --init && pacman-key --gpgdir "$1" --populate msys2',
                    '--', unix_key_home], check=True)
    # MSYS2's pacman-key requires this setting for read-only verification.
    # This keyring is private to this sequential collector, so no other process
    # reads or writes it concurrently.
    with (Path(key_home.name) / 'gpg.conf').open('a', encoding='utf-8') as config:
        config.write('\nlock-never\n')
    components = {}
    for package in sorted(packages):
        fields = metadata[package]
        base = fields.get('BASE', [package])[0]
        version = fields['VERSION'][0]
        archive_name = f"{base}-{version.split(':')[-1]}.src.tar.zst"
        target = sources / 'msys2' / archive_name
        target.parent.mkdir(exist_ok=True)
        url = 'https://mirror.msys2.org/mingw/sources/' + archive_name
        signature = target.with_name(target.name + '.sig')
        if not target.exists():
            urllib.request.urlretrieve(url, target)
        if not signature.exists():
            urllib.request.urlretrieve(url + '.sig', signature)
        unix_sig = subprocess.check_output(['cygpath', '-u', str(signature)], text=True).strip()
        unix_source = subprocess.check_output(['cygpath', '-u', str(target)], text=True).strip()
        subprocess.run([bash, '-c', 'pacman-key --gpgdir "$1" --verify "$2" "$3"',
                        '--', unix_key_home, unix_sig, unix_source], check=True)
        # Keep all package-provided license/copyright files. Fail if none exist.
        notice = notices / package
        notice.mkdir(exist_ok=True)
        found = 0
        for entries in candidates.values():
            for candidate, owner in entries:
                if owner == package and '/share/licenses/' in candidate.as_posix():
                    rel = candidate.relative_to(args.dependencies / 'share/licenses')
                    destination = notice / rel
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(candidate, destination)
                    found += 1
        if not found or base.startswith('mingw-w64-qt6-'):
            spec = importlib.util.spec_from_file_location('source_notices', Path(__file__).with_name('source-notices.py'))
            helper = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(helper)
            found += helper.collect(target, notice)
        if not found:
            raise RuntimeError(f'No package license notices for {package}; inspect signed source package')
        components[package] = dict(version=version, source_path='msys2/' + archive_name,
                                  notice_path=str(notice.relative_to(stage)).replace('\\', '/'),
                                  source_url=url, license=fields.get('LICENSE', []),
                                  compiled_in=package in static_packages)
        print(f'Collected and verified {package}', flush=True)
    # The private collector preserves upstream archives and full notices.
    materials = sources / 'private-materials'
    subprocess.run(['python', str(repo / 'scripts/release/collect-media-sources.py'), str(materials)], check=True)
    for name, directory, archive in [
        ('FFmpeg', 'ffmpeg-9.0.1', 'ffmpeg-9.0.1.tar.xz'),
        ('mpv', 'mpv-0.41.0', 'mpv-0.41.0.tar.gz'),
        ('dav1d', 'dav1d-1.5.4', 'dav1d-1.5.4.tar.gz'),
        ('libplacebo', 'libplacebo-7.360.1', 'libplacebo-7.360.1.tar.gz')]:
        shutil.copytree(materials / 'licenses' / directory, notices / name)
        components[name] = dict(version=directory,
                               source_path='private-materials/media-sources/' + archive,
                               notice_path='share/scrubtub/licenses/' + name)
    rapid = sources / 'rapidfuzz-cpp-3.3.4.tar.gz'
    urllib.request.urlretrieve('https://codeload.github.com/rapidfuzz/rapidfuzz-cpp/tar.gz/refs/tags/v3.3.4', rapid)
    if sha(rapid) != 'a0dd2ef361cac165e12076696e7c7e8d069a2908abd9599ad4bd190de33f9881':
        raise RuntimeError('RapidFuzz checksum mismatch')
    (notices / 'RapidFuzz').mkdir()
    with tarfile.open(rapid) as archive:
        member = next(m for m in archive.getmembers() if m.name.endswith('/LICENSE'))
        (notices / 'RapidFuzz/LICENSE').write_bytes(archive.extractfile(member).read())
    components['RapidFuzz'] = dict(version='3.3.4', source_path=rapid.name,
                                  notice_path='share/scrubtub/licenses/RapidFuzz', license='MIT')
    (materials / 'README.txt').write_text('Private media sources; the full inventory is in DEPENDENCIES.json.\n')
    manifest = dict(format=1, platform='windows-x86_64', components=components, files=records,
                    source_files={p.relative_to(sources).as_posix(): sha(p)
                                  for p in sources.rglob('*') if p.is_file()})
    manifest['notice_files'] = {p.relative_to(stage).as_posix(): sha(p)
                                for p in (stage / 'share/scrubtub').rglob('*')
                                if p.is_file() and p.name not in ('DEPENDENCIES.json', 'DEPENDENCIES.md')}
    (stage / 'share/scrubtub/DEPENDENCIES.json').write_text(json.dumps(manifest, indent=2) + '\n')
    lines = ['# Bundled dependencies', '', '| Component | Version |', '| --- | --- |']
    lines += [f"| {name} | {info['version']} |" for name, info in sorted(components.items())]
    (stage / 'share/scrubtub/DEPENDENCIES.md').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read license texts from already signature-verified MSYS2 source packages."""
import io
import json
from pathlib import Path
import posixpath
import re
import subprocess
import tarfile
import tempfile


def collect(source, destination):
    provenance = {}
    def write(name, content):
        if not content.strip():
            return
        leaf = re.sub(r'[^A-Za-z0-9._-]', '_', name.split('/')[-1])[:70]
        target = f'{len(provenance):04d}-{leaf}'
        (destination / target).write_bytes(content)
        provenance[target] = name

    def visit(archive, parent='', depth=0):
        if depth > 3:
            raise RuntimeError('Unexpected nested source archives')
        referenced_licenses = set()
        for member in archive:
            if not member.isfile():
                continue
            name = parent + member.name
            leaf = member.name.split('/')[-1].lower()
            if leaf == 'qt_attribution.json':
                content = archive.extractfile(member).read()
                write(name, content)
                entries = json.loads(content, strict=False)
                if isinstance(entries, dict):
                    entries = [entries]
                for entry in entries:
                    references = entry.get('LicenseFile', entry.get('LicenseFiles', []))
                    if isinstance(references, str):
                        references = [references]
                    for reference in references:
                        relative = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), reference))
                        referenced_licenses.add(relative)
            elif leaf.startswith(('license', 'licence', 'copying', 'copyright', 'notice')):
                write(name, archive.extractfile(member).read())
            elif leaf.endswith(('.tar.gz', '.tar.xz', '.tar.bz2', '.tgz', '.tar')):
                with tarfile.open(fileobj=io.BytesIO(archive.extractfile(member).read())) as nested:
                    visit(nested, name + '/', depth + 1)
        # Read references in archive order: repeated backwards seeks through a
        # compressed Qt tarball otherwise decompress gigabytes unnecessarily.
        available = {member.name for member in archive.getmembers() if member.isfile()}
        if missing := referenced_licenses - available:
            raise RuntimeError(f'Missing referenced Qt license texts: {sorted(missing)}')
        for member in archive.getmembers():
            if member.isfile() and member.name in referenced_licenses:
                write(parent + member.name, archive.extractfile(member).read())

    # zstd is a declared build dependency; avoid Python-version-specific support.
    data = subprocess.check_output(['zstd', '-d', '-c', str(source)])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        visit(archive)
    # LuaJIT's source package carries its upstream bare Git repository. Select
    # precisely the recipe's commit without executing any PKGBUILD shell code.
    if source.name.startswith('mingw-w64-luajit-'):
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            recipe = archive.extractfile('mingw-w64-luajit/PKGBUILD').read().decode()
            match = re.search(r'^_commit=[\"\']?([0-9a-f]{40})[\"\']?$', recipe, re.M)
            if not match:
                raise RuntimeError('Cannot identify exact LuaJIT source revision')
            with tempfile.TemporaryDirectory(prefix='scrubtub-source-notices-') as temporary:
                archive.extractall(temporary, filter='data')
                repository = Path(temporary) / 'mingw-w64-luajit/luajit'
                content = subprocess.check_output(['git', '--git-dir=' + str(repository),
                                                   'show', match[1] + ':COPYRIGHT'])
                write('luajit@' + match[1] + '/COPYRIGHT', content)
    if provenance:
        (destination / 'source-notices.json').write_text(json.dumps(provenance, indent=2) + '\n')
    return len(provenance)

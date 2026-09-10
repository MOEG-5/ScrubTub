#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Preserve per-file copyright/license comments from private media sources."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
legal = re.compile(rb'copyright|spdx-license|permission|redistribut|warrant|public domain', re.I)
blocks = re.compile(rb'/\*.*?\*/', re.S)
lua_blocks = re.compile(rb'--\[(=*)\[.*?\]\1\]', re.S)
lines = re.compile(rb'(?:^[ \t]*(?://|\#|;|--)[^\r\n]*(?:\r?\n|$))+', re.M)
code = {'.c', '.h', '.cpp', '.hpp', '.cc', '.cxx', '.asm', '.s', '.m', '.mm',
        '.py', '.lua', '.rs', '.glsl', '.inc', '.sh', '.js', '.rc'}
count = 0
with args.output.open('wb') as output:
    output.write(b'Copyright and license notices from the accompanying upstream source.\n'
                 b'Optional source components may not be enabled in this build.\n'
                 b'The original source archives and top-level licenses are also supplied.\n')
    for path in sorted(args.source.rglob('*')):
        if not path.is_file() or path.is_symlink():
            continue
        full = path.name.lower().startswith(('license', 'licence', 'copying', 'copyright', 'notice', 'authors'))
        if not full and path.suffix.lower() not in code:
            continue
        data = path.read_bytes()
        if b'\0' in data:
            continue
        notices = [data] if full else [m.group() for pattern in (blocks, lines, lua_blocks)
                                       for m in pattern.finditer(data) if legal.search(m.group())]
        if notices:
            relative = path.relative_to(args.source).as_posix().encode('utf-8')
            output.write(b'\n\n=== ' + relative + b' ===\n\n' + b'\n\n'.join(notices) + b'\n')
            count += 1
if not count:
    raise RuntimeError('No embedded source notices found')
print(f'Preserved notices from {count} source files in {args.output.name}')

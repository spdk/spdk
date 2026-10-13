#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Nutanix Inc.
#  All rights reserved.

"""Merge SPDK changelog fragments into CHANGELOG.md and remove them."""

import argparse
import re
import sys
from pathlib import Path


def text_lines(data):
    """Yield lines outside code fences with their offsets in the original text."""
    fence = None
    offset = 0
    for line in data.splitlines(keepends=True):
        start = offset
        offset += len(line)
        # Headings in code examples must not split release sections.
        marker = re.match(r' {0,3}(`{3,}|~{3,})', line)
        if fence:
            if marker and marker[1].startswith(fence) and not line[marker.end():].strip():
                fence = None
            continue
        if marker:
            fence = marker[1]
            continue
        yield start, line


def read_fragment(path):
    """Read one entry and reject headings supplied by the category directory."""
    body = path.read_text(encoding='utf-8')
    if not body.strip():
        raise ValueError(f'{path}: empty fragment')
    previous = ''
    previous_end = 0
    for start, line in text_lines(body):
        heading = re.match(r' {0,3}(?:> ?)*(?:(?:[-+*]|[0-9]+[.)]) +)?#{1,6}(?:\s|$)', line)
        # A Setext underline forms a heading only with the preceding text line.
        # Compare offsets because text_lines() skips fenced code.
        underline = previous and start == previous_end and re.fullmatch(r' {0,3}(?:=+|-+) *\n?', line)
        if heading or underline:
            raise ValueError(f'{path}: fragments must not contain headings')
        previous = line.strip()
        previous_end = start + len(line)
    return body


def load_fragments(root):
    fragments = {}
    for category in sorted((root / 'changelog.d').iterdir()):
        if category.name == 'README.md':
            continue
        if not category.is_dir() or category.is_symlink():
            raise ValueError(f'{category}: expected a category directory')
        entries = fragments[category] = []
        for path in sorted(category.iterdir()):
            if path.suffix != '.md' or not path.is_file() or path.is_symlink():
                raise ValueError(f'{path}: expected a .md file directly inside a category')
            entries.append((path, read_fragment(path)))
    return fragments


def current_release(root):
    version = (root / 'VERSION').read_text(encoding='utf-8').strip().split('-')[0]
    if version.endswith('.0'):
        version = version[:-2]
    return 'v' + version


def render(data, release, fragments):
    """Merge fragments into one release while preserving the surrounding text."""
    headings = [(start, line) for start, line in text_lines(data) if line.startswith(('## ', '### '))]
    heading = next(((start, line) for start, line in headings
                    if line.startswith('## ') and line[3:].split(':')[0].strip() == release), None)
    if not heading:
        raise ValueError(f'{release}: release heading not found in CHANGELOG.md')
    if not any(fragments.values()):
        return data
    # Offsets let us replace only the selected release body.
    start = heading[0] + len(heading[1])
    end = next((offset for offset, line in headings if offset >= start and line.startswith('## ')), len(data))
    categories = [(offset, line) for offset, line in headings if start <= offset < end and line.startswith('### ')]
    sections = {}
    for index, (offset, line) in enumerate(categories):
        stop = categories[index + 1][0] if index + 1 < len(categories) else end
        sections[line[4:].strip()] = data[offset + len(line):stop]
    for category, entries in fragments.items():
        if entries:
            bodies = [sections.get(category.name, '').strip('\n')]
            bodies.extend(body.strip('\n') for _, body in entries)
            sections[category.name] = '\n\n'.join(body for body in bodies if body)
    result = data[:categories[0][0]] if categories else data[:end]
    for name, body in sorted(sections.items()):
        result = result.rstrip('\n') + '\n\n### ' + name + '\n\n' + body.strip('\n')
    return result.rstrip('\n') + ('\n\n' if end < len(data) else '\n') + data[end:]


def consume(root, result, fragments):
    # Write the complete changelog before removing its source fragments.
    (root / 'CHANGELOG.md').write_text(result, encoding='utf-8')
    for category, entries in fragments.items():
        for path, _ in entries:
            path.unlink()
        category.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release', help='existing release section, e.g. v26.09 (default: VERSION)')
    parser.add_argument('--dry-run', action='store_true',
                        help='write the complete changelog to stdout instead of updating CHANGELOG.md')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        fragments = load_fragments(root)
        release = args.release or current_release(root)
        data = (root / 'CHANGELOG.md').read_text(encoding='utf-8')
        result = render(data, release, fragments)
        if args.dry_run:
            sys.stdout.write(result)
        else:
            consume(root, result, fragments)
    except (ValueError, OSError) as error:
        print(f'Error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

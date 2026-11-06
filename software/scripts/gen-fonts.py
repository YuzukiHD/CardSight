#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate the UI font subsets from the strings in apps/cardsight.

The font and lv_font_conv are supplied by the caller; no local path is baked in.
"""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--font', required=True)
parser.add_argument('--sizes', default='16,20', help='Comma separated subset to regenerate')
parser.add_argument('--converter', required=True, help='Path to lv_font_conv.js')
args = parser.parse_args()
sizes = {int(size) for size in args.sizes.split(',')}
app = Path(__file__).resolve().parent.parent / 'apps/cardsight'
source = '\n'.join(path.read_text() for path in sorted(app.rglob('*.c'))
                   if path.parent.name != 'fonts')
symbols = ''.join(sorted({char for literal in re.findall(r'"(?:\\.|[^"\\])*"', source)
                          for char in literal if ord(char) > 127}))
# Alpha levels lifted in the middle so thin strokes stay visible on the panel.
LEVELS = [0, 1, 2, 3, 6, 8, 10, 12, 12, 13, 13, 14, 14, 14, 15, 15]


def lift_bitmap(text):
    start = text.index('glyph_bitmap[] = {')
    end = text.index('};', start)

    def lift(match):
        value = int(match.group(0), 16)
        return '0x%02x' % (LEVELS[value >> 4] << 4 | LEVELS[value & 15])
    # Only data bytes (0x..) inside the bitmap table are remapped, never the comments.
    body = re.sub(r'/\*.*?\*/|0x[0-9a-fA-F]{1,2}',
                  lambda match: match.group(0) if match.group(0).startswith('/*') else lift(match),
                  text[start:end])
    return text[:start] + body + text[end:]


# The capacity digits are a separate Noto Sans SC face with its own alpha curve; regenerating them
# from the UI font would change every number on screen, so they are left alone here.
for size, name, filename, characters in [
        (16, 'cardsight_font_16', 'font_16.c', symbols),
        (20, 'cardsight_font_20', 'font_20.c', symbols)]:
    if size not in sizes:
        continue
    target = app / 'fonts' / filename
    command = ['node', args.converter, '--font', args.font, '--size', str(size), '--bpp', '4',
               '--format', 'lvgl', '--symbols', characters, '--no-compress', '--no-prefilter',
               '--force-fast-kern-format', '--lv-font-name', name, '-o', str(target),
               '-r', '0x20-0x7f']
    subprocess.run(command, check=True)
    text = target.read_text().replace('#include "lvgl/lvgl.h"', '#include <lvgl.h>')
    text = re.sub(r' \* Opts:.*', ' * CardSight UI glyph subset.', text)
    target.write_text(lift_bitmap(text).rstrip() + '\n')

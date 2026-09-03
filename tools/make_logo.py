#!/usr/bin/env python3
"""Writes the two SVG images used in the readme.

    python3 tools/make_logo.py

Both files are plain vector graphics, no bitmap and no external font file.
"""

import os

INK = "#141c30"
PAPER = "#eef2fb"
BLUE_DARK = "#2f4270"
BLUE = "#52689f"
TEXT = "#f4f7ff"
MUTED = "#9fb0d4"
ACCENT = "#e2952a"

FONT = "DejaVu Sans, Helvetica, Arial, sans-serif"
MONO = "DejaVu Sans Mono, Menlo, Consolas, monospace"


def page_stack(x, y):
    """Three stacked pages with a small tree drawn on the front one."""
    out = []
    for i, fill in enumerate([BLUE_DARK, BLUE, PAPER]):
        out.append(
            '<rect x="%d" y="%d" width="44" height="60" rx="4" fill="%s"/>'
            % (x + 24 - 12 * i, y + 8 * i, fill)
        )
    fx, fy = x, y + 16
    root = (fx + 22, fy + 16)
    left = (fx + 10, fy + 42)
    right = (fx + 34, fy + 42)
    for child in (left, right):
        out.append(
            '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="2"/>'
            % (root[0], root[1], child[0], child[1], BLUE_DARK)
        )
        out.append(
            '<circle cx="%d" cy="%d" r="3.5" fill="%s"/>' % (child[0], child[1], BLUE_DARK)
        )
    out.append('<circle cx="%d" cy="%d" r="4.5" fill="%s"/>' % (root[0], root[1], ACCENT))
    return "\n  ".join(out)


def logo():
    parts = []
    parts.append('<rect width="480" height="140" rx="20" fill="%s"/>' % INK)
    parts.append(page_stack(40, 24))
    parts.append(
        '<text x="142" y="78" font-family="%s" font-size="44" font-weight="700" '
        'fill="%s">Page<tspan fill="%s">DB</tspan></text>' % (FONT, TEXT, ACCENT)
    )
    parts.append(
        '<text x="145" y="104" font-family="%s" font-size="14" fill="%s" '
        'letter-spacing="2.5">EMBEDDED KEY VALUE STORE</text>' % (MONO, MUTED)
    )
    return svg(480, 140, parts)


LAYERS = [
    ("Database api", "put, get, remove, scan, checkpoint"),
    ("B+ tree", "finds the leaf page for a key, splits and merges nodes"),
    ("Buffer pool", "fixed frames, pins, lru eviction, page guards"),
    ("Disk manager", "pread and pwrite of 4096 byte pages, file lock"),
]


def arrow(x, y1, y2):
    return (
        '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="2"/>'
        '<path d="M %d %d l -5 -8 l 10 0 z" fill="%s"/>'
        % (x, y1, x, y2, BLUE, x, y2, BLUE)
    )


def architecture():
    parts = ['<rect width="620" height="400" rx="20" fill="%s"/>' % INK]
    top = 32
    for i, (title, sub) in enumerate(LAYERS):
        y = top + i * 76
        parts.append(
            '<rect x="60" y="%d" width="400" height="56" rx="10" fill="%s"/>'
            % (y, BLUE_DARK)
        )
        parts.append(
            '<text x="80" y="%d" font-family="%s" font-size="19" font-weight="700" '
            'fill="%s">%s</text>' % (y + 25, FONT, TEXT, title)
        )
        parts.append(
            '<text x="80" y="%d" font-family="%s" font-size="12" fill="%s">%s</text>'
            % (y + 43, MONO, MUTED, sub)
        )
        if i + 1 < len(LAYERS):
            parts.append(arrow(260, y + 56, y + 74))

    file_y = top + len(LAYERS) * 76 + 4
    for i, (name, note) in enumerate(
        [("pagedb.db", "pages"), ("pagedb.wal", "log")]
    ):
        x = 60 + i * 210
        parts.append(
            '<rect x="%d" y="%d" width="190" height="46" rx="10" fill="none" '
            'stroke="%s" stroke-width="2" stroke-dasharray="6 5"/>'
            % (x, file_y, BLUE)
        )
        parts.append(
            '<text x="%d" y="%d" font-family="%s" font-size="15" fill="%s">%s</text>'
            % (x + 18, file_y + 29, MONO, TEXT, name)
        )
        parts.append(
            '<text x="%d" y="%d" font-family="%s" font-size="12" fill="%s">%s</text>'
            % (x + 130, file_y + 29, MONO, MUTED, note)
        )
    parts.append(arrow(155, file_y - 18, file_y - 2))
    parts.append(arrow(365, file_y - 18, file_y - 2))
    return svg(620, 400, parts)


def svg(width, height, parts):
    head = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" role="img">' % (width, height, width, height)
    )
    return head + "\n  " + "\n  ".join(parts) + "\n</svg>\n"


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    docs = os.path.join(here, "docs")
    with open(os.path.join(docs, "logo.svg"), "w") as f:
        f.write(logo())
    with open(os.path.join(docs, "architecture.svg"), "w") as f:
        f.write(architecture())
    print("wrote docs/logo.svg and docs/architecture.svg")


if __name__ == "__main__":
    main()

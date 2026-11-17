"""Checks that the redesign mockups (docs/design/mockups/*.svg) keep to the layout grids of the proposal.

Purpose: the proposal (sections 6.3 and 6.11, rule 9) says every position on the four screens comes from a
declared grid. This script checks that on the SVG files themselves, so the claim does not rest on looking.

Usage:  python gridcheck.py [<folder with the SVGs>]     (default: the folder this script is in)
Prints one line per screen and PASS, or each edge that is off and FAIL with exit code 1.
Needs only the Python standard library. Run it after generate_mockups.py.

What it checks: every horizontal edge right of the nav (the x of each text anchor, the left and right of
each rectangle, both ends of each line, each circle centre) must be
- a line of that screen's grids, each with a 24 px gutter: page 5 x 160 (x 252-1148), card content
  12 x 46 (x 308-1124), Optimize card 8 x 85 (x 276-1124) with its detail pane 4 x 77 (x 744-1124),
  result text 2 x 420 (x 284-1148); a column start or end, or a gutter centre; or the card padding
  (276, 1124) or a status icon's centre; or
- a fixed offset of the element's own component from such a line (an item lead of 24, a button of 186 or
  89, a toggle of 40, a link's chevron, 12 px row padding, a timeline through 14 px marks, ...). An offset
  counts only for the kind of element it belongs to, so a coincidence with another component does not
  pass.

Deliberate exceptions, by content (proposal 6.11, rules 6 and 9): the captions placed after a measured
value or label ("app curve", "the run", the curve section's note, "Show all" after "Settings"), the
curve's axis ticks, and the strokes of an icon's glyph within 6 px of the icon's centre. Any new
exception has to be added here by name.
"""
import os, re, sys
sys.stdout.reconfigure(encoding='utf-8')
folder = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
PAGE_L, PAGE_R, PAD, LEAD, G = 252, 1148, 24, 32, 24
BTN, NARROW = 186, 89


def grid(name, x, w, n, expect_cw):
    cw = (w - (n - 1) * G) / n
    assert cw == expect_cw, f'{name}: {w} / {n} gives {cw}, expected {expect_cw}'
    out = {}
    for i in range(n):
        out.setdefault(x + i * (cw + G), f'{name} col {i} start')
        out.setdefault(x + i * (cw + G) + cw, f'{name} col {i} end')
        if i:
            out.setdefault(x + i * (cw + G) - G / 2, f'{name} gutter {i - 1}/{i} centre')
    return out


PAGE = grid('page', PAGE_L, PAGE_R - PAGE_L, 5, 160)                         # 5 x 160
CARD = grid('card', PAGE_L + PAD + LEAD, PAGE_R - PAGE_L - 2 * PAD - LEAD, 12, 46)   # 12 x 46 at 308
CHOOSE = grid('choose', PAGE_L + PAD, PAGE_R - PAGE_L - 2 * PAD, 8, 85)     # 8 x 85 at 276
TEXT = grid('result text', PAGE_L + LEAD, PAGE_R - PAGE_L - LEAD, 2, 420)   # 2 x 420 at 284
DETAIL = grid('detail', PAGE_L + PAD + 4 * (85 + G) + LEAD, PAGE_R - PAD - (PAGE_L + PAD + 4 * (85 + G) + LEAD), 4, 77)  # 4 x 77 at 744
FIXED = {PAGE_L + PAD: 'card padding', PAGE_R - PAD: 'card padding', PAGE_L + 11: 'page icon centre',
         PAGE_L + PAD + 10: 'title icon centre', PAGE_L + PAD + 11: 'page icon centre in card'}

SCREENS = {
    'dashboard-after.svg': [PAGE, CARD],
    'optimize-after.svg': [PAGE, CHOOSE, DETAIL],
    'run-recovering-after.svg': [PAGE, CARD],
    'result-not-kept-after.svg': [PAGE, TEXT, CARD],
}
# offsets from a grid line, per kind of element (section 6.3 / 6.6): an edge only counts when its own
# component explains it, so a coincidence with another component's offset does not pass
OFFSETS = {
    'text-start': [(24, 'item lead'), (36, 'item lead after 12 px row padding'), (40 + 12, 'label after a toggle'),
                   (10 + 4, 'phase name after its 10 px check (D15)')],
    'text-middle': [(NARROW / 2, 'narrow button centre'), (NARROW + 8 + NARROW / 2, 'second narrow button centre'),
                    (BTN / 2, 'button centre'), (BTN + 8 + NARROW / 2, 'narrow button centre after a button'),
                    (-BTN / 2, 'button centre, button at the edge'), (206, 'centre of a pane-wide button')],
    'text-end': [(-16, 'link before its chevron'), (-12, 'row padding, or a label before its control'), (-8, 'axis label gap')],
    'rect left': [(NARROW + 8, 'second narrow button'), (BTN + 8, 'button after a button'), (-40, 'toggle at an edge'),
                  (-BTN, 'button at an edge')],
    'rect right': [(NARROW, 'narrow button'), (NARROW + 8 + NARROW, 'second narrow button'), (BTN, 'button'),
                   (BTN + 8 + NARROW, 'narrow button after a button'), (40, 'toggle')],
    'circle centre': [(7, '14 px icon centre'), (10, 'title icon centre'), (11, 'page icon centre'),
                      (20, 'radio centre after row padding'), (30, 'toggle knob (on)'), (-10, 'toggle knob (on, at an edge)')],
    'line x1': [(7, 'timeline through 14 px marks')], 'line x2': [(7, 'timeline through 14 px marks')],
}
GLYPH = 6   # strokes of an icon glyph lie within 6 px of the icon's centre
# positions that follow measured text, by design (proposal 6.11): listed by content
FLOW = {'app curve', 'the run', 'On the card now: stock  ·  MHz against mV', 'Show all'}
TICKS = {'700', '800', '900', '1000', '1100', '1200'}

num = r'(-?[\d.]+)'
bad = 0
for name, grids in SCREENS.items():
    src = open(os.path.join(folder, name), encoding='utf-8').read()
    lines = dict(FIXED)
    for g in grids:
        for x, why in g.items():
            lines.setdefault(x, why)
    edges = []
    for m in re.finditer(r'<text x="' + num + r'" y="' + num + r'"[^>]*text-anchor="(\w+)">([^<]*)<', src):
        edges.append((float(m[1]), 'text-' + m[3], m[4].replace('&amp;', '&')))
    for m in re.finditer(r'<rect x="' + num + r'" y="' + num + r'" width="' + num + r'" height="' + num + r'"', src):
        x, w = float(m[1]), float(m[3])
        edges += [(x, 'rect left', f'{w:g} wide'), (x + w, 'rect right', f'{w:g} wide')]
    for m in re.finditer(r'<line x1="' + num + r'" y1="' + num + r'" x2="' + num + r'" y2="' + num + r'"', src):
        edges += [(float(m[1]), 'line x1', ''), (float(m[3]), 'line x2', '')]
    for m in re.finditer(r'<circle cx="' + num + r'" cy="' + num + r'" r="' + num + r'"', src):
        edges.append((float(m[1]), 'circle centre', 'r ' + m[3]))
    flow = ticks = 0
    off, ok = [], 0
    centres = []
    for x, kind, what in edges:
        if kind == 'circle centre':
            for c in lines:
                for d, w in OFFSETS[kind]:
                    if round(c + d, 1) == round(x, 1) and 'icon' in w:
                        centres.append(x)
    for x, kind, what in edges:
        if x <= 220:
            continue
        if what in FLOW:
            flow += 1
            continue
        if kind == 'text-middle' and what in TICKS:
            ticks += 1
            continue
        if round(x, 1) in {round(c, 1) for c in lines}:
            ok += 1
            continue
        if any(round(c + d, 1) == round(x, 1) for c in lines for d, _ in OFFSETS[kind]):
            ok += 1
            continue
        if kind in ('line x1', 'line x2', 'circle centre') and any(abs(x - c) <= GLYPH for c in centres):
            ticks += 1
            continue
        off.append((x, kind, what))
    print(f'== {name}: {len(edges)} edges; on a grid line or its component offset: {ok}; '
          f'after measured text: {flow}; chart ticks / glyph strokes: {ticks}; UNEXPLAINED: {len(off)}')
    for x, kind, what in off:
        print(f'   OFF {x:7.1f} {kind:12} {what}')
    bad += len(off)
print('PASS' if not bad else f'FAIL: {bad} edges not on a grid line')
sys.exit(1 if bad else 0)

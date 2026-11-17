"""Generates the redesign mockups (docs/design/mockups/*.svg) from the proposed tokens and layout rules.

    python generate_mockups.py <output folder>

Needs Pillow and the Windows fonts Segoe UI (segoeui.ttf) and Segoe UI Semibold (seguisb.ttf): every
text is measured, wrapped to its column and checked against the width its column allows, so a label
that does not fit stops the script instead of overlapping its neighbour.

Static pictures of proposed screens. Numbers come from logged runs where the caption says so;
live sensor values are illustrative.

Layout rules (proposal, sections 6.3 and 6.11): every position below comes from the page grid, a
card's content grid, the line heights of the five text styles and the spacing tokens. Nothing is
placed by eye.
"""
import os, sys
from html import escape

try:
    from PIL import ImageFont
except ImportError:
    sys.exit('generate_mockups.py needs Pillow (pip install pillow) to measure text')

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- tokens (proposal, section 6)
T = dict(
    bg='#111317', nav='#0D0F12', surface1='#171A1F', surface2='#1C2026', surface3='#242830',
    border='#2B313A', border_strong='#3D4552', text1='#E6E8ED', text2='#99A3B0', text3='#7A8390',
    accent='#2661BF', accent_text='#4A94FF',
    success='#5CC780', warning='#E8B53B', danger='#ED6666',
)
FONT = "Segoe UI, 'Segoe UI Variable', sans-serif"
SIZE = dict(caption=13, body=15, strong=15, title=18, page=22, display=28)
LH = dict(caption=18, body=22, strong=22, title=24, page=28, display=34)     # line heights
BASE = dict(caption=14, body=16, strong=16, title=18, page=22, display=27)   # baseline from line top

# spacing scale (px): 4 8 12 16 24 32
S1, S2, S3, S4, S6, S8 = 4, 8, 12, 16, 24, 32
GUTTER = S6        # between columns, everywhere
PAD = S6           # card padding
LEAD = S8          # status icon column: the text starts 32 px after the column's left edge
ITEM_LEAD = S6     # list item lead: a 14 px icon or step mark, the text 24 px after the item's left edge
BTN_H, START_H = 36, 48
LIST_ROW = 40      # rows of a list or table with hairlines
STEP_PITCH = 22 + S2   # rows of a step or fact list without hairlines: one body line and 8 px

W, H, NAV = 1180, 860, 220
PAGE_L, PAGE_R = NAV + S8, W - S8
PAGE_W = PAGE_R - PAGE_L     # 896

_fonts = {}


def width(s, style, weight=400):
    key = (SIZE[style], weight >= 600)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype('C:/Windows/Fonts/' + ('seguisb.ttf' if key[1] else 'segoeui.ttf'), key[0])
    return _fonts[key].getlength(s)


def wrap(s, style, maxw, weight=400):
    lines, cur = [], ''
    for word in s.split(' '):
        nxt = (cur + ' ' + word).strip()
        if cur and width(nxt, style, weight) > maxw:
            lines.append(cur)
            cur = word
        else:
            cur = nxt
    return lines + [cur]


class Grid:
    """Equal columns over [x, x + w] with the standard gutter, in whole pixels."""
    def __init__(self, x, w, cols, gutter=GUTTER):
        self.x, self.w, self.n, self.g = x, w, cols, gutter
        cw = (w - (cols - 1) * gutter) / cols
        assert cw == int(cw), f'grid {w}/{cols} does not divide into whole pixels'
        self.cw = int(cw)

    def col(self, i):
        return self.x + i * (self.cw + self.g)

    def span(self, n):
        return n * self.cw + (n - 1) * self.g


class Svg:
    def __init__(self, title):
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
                      f'role="img" aria-label="{escape(title)}">',
                      f'<rect width="{W}" height="{H}" fill="{T["bg"]}"/>']

    def raw(self, s):
        self.parts.append(s)

    def rect(self, x, y, w, h, fill, r=8, stroke=None, sw=1):
        s = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ''
        self.raw(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"{s}/>')

    def text(self, x, top, s, style='body', color='text1', weight=None, anchor='start', maxw=None):
        """Places one line by the top of its line box; returns the line's bottom."""
        weight = weight or (600 if style in ('strong', 'title', 'page', 'display') else 400)
        if maxw is not None:
            wd = width(s, style, weight)
            assert wd <= maxw, f'"{s}" is {wd:.0f} px, its column allows {maxw}'
        self.raw(f'<text x="{x:g}" y="{top + BASE[style]:g}" font-family="{FONT}" font-size="{SIZE[style]}" '
                 f'font-weight="{weight}" fill="{T.get(color, color)}" text-anchor="{anchor}">{escape(s)}</text>')
        return top + LH[style]

    def para(self, x, top, s, maxw, style='body', color='text2'):
        for line in wrap(s, style, maxw):
            top = self.text(x, top, line, style, color, maxw=maxw)
        return top

    def circle(self, x, y, r, fill, stroke=None, sw=2):
        s = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ''
        self.raw(f'<circle cx="{x:g}" cy="{y:g}" r="{r}" fill="{fill}"{s}/>')

    def line(self, x1, y1, x2, y2, color, w=1):
        self.raw(f'<line x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}" stroke="{T.get(color, color)}" '
                 f'stroke-width="{w}" stroke-linecap="round"/>')

    def path(self, d, color, w=2):
        self.raw(f'<path d="{d}" fill="none" stroke="{T.get(color, color)}" stroke-width="{w}" '
                 f'stroke-linecap="round" stroke-linejoin="round"/>')

    def save(self, name):
        self.parts.append('</svg>')
        open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n').write('\n'.join(self.parts) + '\n')


# ---------------------------------------------------------------- components
BTN_W = 186                       # standard button: three units of the 816 px card content grid
BTN_NARROW = (BTN_W - S2) // 2    # 89: two narrow buttons and their 8 px gap fill one standard width


def button(s, x, top, w, label, kind='secondary', h=BTN_H, disabled=False):
    if disabled:                           # section 6.6: 40 % opacity
        s.raw('<g opacity="0.4">')
    if kind == 'primary':
        s.rect(x, top, w, h, T['accent'], 6)
    else:
        s.rect(x, top, w, h, T['surface2'], 6, T['border_strong'])
    assert width(label, 'strong', 600) <= w - 2 * S4, f'button "{label}" is too narrow'
    s.text(x + w / 2, top + (h - LH['strong']) / 2, label, 'strong', 'text1', 600, 'middle')
    if disabled:
        s.raw('</g>')
    return top + h


def toggle(s, x, top, on=True):          # 40 x 20
    if on:
        s.rect(x, top, 40, 20, T['accent'], 10)
        s.circle(x + 30, top + 10, 6, T['text1'])
    else:
        s.rect(x, top, 40, 20, T['surface2'], 10, T['border_strong'])
        s.circle(x + 10, top + 10, 6, T['text2'])


def card(s, x, top, w, h):
    """Drawn after its content (its height follows from the content) but placed under it."""
    s.rect(x, top, w, h, T['surface1'], 8, T['border'])
    s.parts.insert(2, s.parts.pop())


def hairline(s, x1, x2, y):
    s.line(x1, y, x2, y, 'border')


def status_icon(s, cx, cy, kind, r=10):
    """A filled circle with a glyph: the glyph differs by state as well as the colour."""
    col = dict(success='success', warning='warning', notkept='warning', recovering='warning',
               danger='danger', info='text2')[kind]
    s.circle(cx, cy, r, T[col])
    k, w = r * 0.45, max(1.6, r * 0.2)
    if kind == 'success':
        s.path(f'M{cx - k:g} {cy + 0.05 * r:g} L{cx - 0.12 * r:g} {cy + 0.38 * r:g} L{cx + k:g} {cy - 0.32 * r:g}', 'bg', w)
    elif kind in ('warning', 'notkept'):
        s.line(cx, cy - 0.5 * r, cx, cy + 0.12 * r, T['bg'], w)
        s.circle(cx, cy + 0.45 * r, w * 0.6, T['bg'])
    elif kind == 'danger':
        s.line(cx - k * 0.8, cy - k * 0.8, cx + k * 0.8, cy + k * 0.8, T['bg'], w)
        s.line(cx - k * 0.8, cy + k * 0.8, cx + k * 0.8, cy - k * 0.8, T['bg'], w)
    elif kind == 'info':
        s.circle(cx, cy - 0.45 * r, w * 0.6, T['bg'])
        s.line(cx, cy - 0.08 * r, cx, cy + 0.5 * r, T['bg'], w)
    elif kind == 'recovering':
        a = 0.48 * r
        s.path(f'M{cx:g} {cy - a:g} A{a:g} {a:g} 0 1 1 {cx - a:g} {cy:g}', 'bg', w)
        s.path(f'M{cx - a - 0.26 * r:g} {cy + 0.02 * r:g} L{cx - a:g} {cy - 0.24 * r:g} L{cx - a + 0.26 * r:g} {cy + 0.02 * r:g}', 'bg', w)


ICON_R = dict(page=11, title=10, body=7, strong=7)   # the icon's size follows the line it sits on


def lead_icon(s, x, top, style, kind):
    """A status icon at the start of a lead column at x, centred on the line box at `top`."""
    r = ICON_R[style]
    status_icon(s, x + r, top + LH[style] / 2, kind, r)


def chevron(s, cx, cy, d, color='text2'):
    s.path({'down': f'M{cx - 4} {cy - 2} L{cx} {cy + 2} L{cx + 4} {cy - 2}',
            'up': f'M{cx - 4} {cy + 2} L{cx} {cy - 2} L{cx + 4} {cy + 2}',
            'right': f'M{cx - 2} {cy - 4} L{cx + 2} {cy} L{cx - 2} {cy + 4}'}[d], color, 1.5)


def disclosure(s, right, top, label, d):
    """The one disclosure control: a caption link right-aligned to `right`, its chevron in the last 12 px."""
    chevron(s, right - 5, top + LH['caption'] / 2, d)
    s.text(right - S4, top, label, 'caption', 'accent_text', 400, 'end')


def link_end(s, right, top, label):
    s.text(right, top, label, 'caption', 'accent_text', 400, 'end')


def label_value(s, x, top, label, value, maxw, vstyle='body', vweight=None):
    """A caption label over its value, 4 px apart. Returns the bottom."""
    s.text(x, top, label, 'caption', 'text2', maxw=maxw)
    return s.text(x, top + LH['caption'] + S1, value, vstyle, 'text1', vweight, maxw=maxw)


def same_baseline(top, style, other):
    """The line top that puts a line of `other` on the baseline of a line of `style` at `top`."""
    return top + BASE[style] - BASE[other]


# ---------------------------------------------------------------- timeline marks
# One family for every sequence: recent events and recovery steps. A 14 px slot; a hairline joins the
# marks of one sequence; each mark is filled with the surface it sits on, so the line stops at it.
MARK = 14
CHECK_W = 10                       # the check before a done phase's name in the phase rail (D15)


def mark(s, cx, cy, state, surface):
    if state == 'event':              # an ordinary event: a small neutral dot
        s.circle(cx, cy, 4, T[surface], T['border_strong'], 1.5)
    elif state == 'done':             # a confirmed step: a neutral filled disc with a check
        s.circle(cx, cy, 7, T['surface3'])
        s.path(f'M{cx - 3.2:g} {cy + 0.2:g} L{cx - 0.9:g} {cy + 2.6:g} L{cx + 3.4:g} {cy - 2.4:g}', 'text1', 1.5)
    elif state == 'current':          # in progress: an accent arc on a neutral ring
        s.circle(cx, cy, 6, T[surface], T['surface3'], 2)
        s.raw(f'<path d="M{cx:g} {cy - 6:g} A6 6 0 0 1 {cx + 6:g} {cy:g}" fill="none" stroke="{T["accent_text"]}" '
              f'stroke-width="2" stroke-linecap="round"/>')
    elif state == 'next':             # not reached: a hollow ring
        s.circle(cx, cy, 5.5, T[surface], T['border_strong'], 1.5)
    else:                             # a state that needs attention: the status icon itself
        status_icon(s, cx, cy, state, 7)


def timeline(s, cx, centres, states, surface):
    """The joining hairline first, then the marks on top of it."""
    s.line(cx, centres[0], cx, centres[-1], 'border')
    for cy, st in zip(centres, states):
        mark(s, cx, cy, st, surface)


PG = Grid(PAGE_L, PAGE_W, 5)      # the page grid: 5 x 160, gutter 24
CARD = Grid(PAGE_L + PAD + LEAD, PAGE_W - 2 * PAD - LEAD, 12)   # a card's content grid: 12 x 46 at 308
C4 = Grid(CARD.x, CARD.w, 4)      # 4 x 186 on the same lines


def MEASURE(x, w):
    """A paragraph is at most three of four columns of its grid wide (about 80 characters)."""
    return Grid(x, w, 4).span(3)


def readings(s, top, items):
    """The live readings: one strip of five cells on the page grid, a hairline at each gutter's centre
    between them. A caption after a value, on its baseline, names its owner or limit. The same strip on
    every screen. Returns the bottom."""
    bottom = top + LH['caption'] + S1 + LH['title']
    for i, item in enumerate(items):
        label, val = item[:2]
        x = PG.col(i)
        if i:
            s.line(x - GUTTER / 2, top, x - GUTTER / 2, bottom, 'border')
        label_value(s, x, top, label, val, PG.cw, 'title')
        if len(item) > 2:
            vx = x + round(width(val, 'title', 600)) + S2
            s.text(vx, same_baseline(top + LH['caption'] + S1, 'title', 'caption'), item[2], 'caption', 'text3',
                   maxw=x + PG.cw - vx)
    return bottom


def page_header(s, title, caption, icon=None, action=None):
    """Title (page) over one caption line at the page's left edge. A status icon sits in a 32 px lead in
    front of the title when the title is a state. One action at most, right-aligned to the page edge and
    centred on the block. `caption` items are (text, page column or None for the text column[, 'link']).
    Returns the block's bottom and its text x."""
    tx = PAGE_L + LEAD if icon else PAGE_L
    top = S8
    if icon:
        lead_icon(s, PAGE_L, top, 'page', icon)
    s.text(tx, top, title, 'page')
    ctop = top + LH['page'] + S1
    for text, col, *kind in caption:
        s.text(tx if col is None else PG.col(col), ctop, text, 'caption', 'accent_text' if kind else 'text2')
    bottom = ctop + LH['caption']
    if action:
        button(s, PAGE_R - BTN_W, top + (bottom - top - BTN_H) / 2, BTN_W, action)
    return bottom, tx


def section_row(s, x, w, top, label, note=None, open_=False):
    """A collapsible section: one 48 px row between hairlines. Returns the row's bottom."""
    hairline(s, x, x + w, top)
    ty = top + (48 - LH['strong']) / 2
    s.text(x, ty, label, 'strong')
    if note:
        s.text(x + round(width(label, 'strong', 600)) + S4, same_baseline(ty, 'strong', 'caption'), note, 'caption', 'text2')
    disclosure(s, x + w, same_baseline(ty, 'strong', 'caption'), 'Hide' if open_ else 'Show', 'up' if open_ else 'down')
    hairline(s, x, x + w, top + 48)
    return top + 48


def toggle_label(s, x, top, label, h=BTN_H, on=True):
    """A switch with its label 12 px after it, centred in a row of height h: the same everywhere."""
    toggle(s, x, top + (h - 20) / 2, on)
    s.text(x + 40 + S3, top + (h - LH['body']) / 2, label, 'body')


# ---------------------------------------------------------------- shell
NAV_C = 36         # the centre of the nav's icon column: the product mark, the item icons, the account mark
NAV_TEXT = 56      # every text in the nav starts here


def nav_icon(s, name, cx, cy, color, behind):
    c = T[color]
    if name == 'Dashboard':
        for dx in (-4, 4):
            for dy in (-4, 4):
                s.rect(cx + dx - 3, cy + dy - 3, 6, 6, 'none', 1.5, c, 1.5)
    elif name == 'Optimize':
        s.line(cx - 7, cy - 3.5, cx + 7, cy - 3.5, c, 1.5)
        s.line(cx - 7, cy + 3.5, cx + 7, cy + 3.5, c, 1.5)
        s.circle(cx - 2.5, cy - 3.5, 2.5, T[behind], c, 1.5)
        s.circle(cx + 3, cy + 3.5, 2.5, T[behind], c, 1.5)
    elif name == 'Fan':
        s.circle(cx, cy, 7, 'none', c, 1.5)
        for dx, dy in ((0, -4.5), (3.9, 2.25), (-3.9, 2.25)):
            s.line(cx, cy, cx + dx, cy + dy, c, 1.5)
    else:
        s.circle(cx, cy, 7, 'none', c, 1.5)
        s.circle(cx, cy - 3.2, 1.1, c)
        s.line(cx, cy - 0.5, cx, cy + 3.5, c, 1.5)


def shell(s, active, running=False):
    s.rect(0, 0, NAV, H, T['nav'], 0)
    # identity: a 24 px mark (the flat-topped curve an undervolt draws) and the name, on the page title's line
    mt = S8 + (LH['page'] - 24) / 2
    s.rect(NAV_C - 12, mt, 24, 24, T['accent'], 6)
    s.path(f'M{NAV_C - 7} {mt + 18} L{NAV_C - 2} {mt + 12} L{NAV_C + 1} {mt + 8} L{NAV_C + 7} {mt + 8}', 'text1', 2)
    s.text(NAV_TEXT, same_baseline(S8, 'page', 'strong'), 'GPU Auto Optimizer', 'strong', maxw=NAV - S3 - NAV_TEXT)
    for i, name in enumerate(['Dashboard', 'Optimize', 'Fan', 'About']):
        top = 96 + i * (BTN_H + S2)
        on = name == active
        if on:
            s.rect(S3, top, NAV - 2 * S3, BTN_H, T['surface3'], 6)
        nav_icon(s, name, NAV_C, top + BTN_H / 2, 'text1' if on else 'text2', 'surface3' if on else 'nav')
        s.text(NAV_TEXT, top + (BTN_H - LH['body']) / 2, name, 'body', 'text1' if on else 'text2', 600 if on else 400)
        if name == 'Optimize' and running:
            s.circle(NAV - S3 - S4, top + BTN_H / 2, 3.5, T['accent_text'])
    # the account area: what the app may do on this PC, the one thing it knows about the user
    bt = H - S8 - LH['body'] - LH['caption']
    hairline(s, S3, NAV - S3, bt - S4)
    cy = bt + (LH['body'] + LH['caption']) / 2
    s.circle(NAV_C, cy, 12, T['surface3'])
    s.path(f'M{NAV_C} {cy - 6} L{NAV_C + 5} {cy - 4} L{NAV_C + 5} {cy + 0.5} Q{NAV_C + 5} {cy + 5} {NAV_C} {cy + 7} '
           f'Q{NAV_C - 5} {cy + 5} {NAV_C - 5} {cy + 0.5} L{NAV_C - 5} {cy - 4} Z', 'text2', 1.5)
    s.text(NAV_TEXT, bt, 'Administrator', 'body', maxw=NAV - S3 - NAV_TEXT)
    s.text(NAV_TEXT, bt + LH['body'], 'Can tune the card', 'caption', 'text3', maxw=NAV - S3 - NAV_TEXT)


# ================================================================ dashboard
s = Svg('Proposed Dashboard: the card now, your tune, recent activity')
shell(s, 'Dashboard')
y, _ = page_header(s, 'NVIDIA GeForce RTX 4070', [('Driver 617.42', 0), ('Detect again', 1, 'link')])
# the page is full to its bottom, so the mockup's note sits on the header's caption line, at the page edge
s.text(PAGE_R, y - LH['caption'], 'Mockup. The average temperatures are illustrative.', 'caption', 'text3', anchor='end')
y = readings(s, y + S6, [('Core clock', '2850 MHz'), ('Memory clock', '12001 MHz'), ('Temperature', '61 °C'),
                         ('Power', '172 / 220 W'), ('Fans', '52 %', 'app curve')])

# your tune: one card after the status icon's lead, in three parts. The outcomes, measured against stock,
# on the four 186 px columns; under a hairline the active settings as a list in the left half and the
# step, which lowers all of them, as one group in the right half (2 x 396): outcomes and settings do not
# share columns, because an outcome does not belong to the setting under it
CT = y + S8
C2 = Grid(CARD.x, CARD.w, 2)       # 2 x 396 on the same 816 px content width
top = CT + PAD
lead_icon(s, PAGE_L + PAD, top, 'title', 'success')
s.text(C4.x, top, 'Your tune is applied', 'title', maxw=C4.span(3))
sb = s.text(C4.x, top + LH['title'] + S1, 'All in one, saved 2026-10-08. Against stock, 5 minutes each:',
            'body', 'text2', maxw=C4.span(3))
button(s, C4.col(3), top + (sb - top - BTN_H) / 2, BTN_W, 'Optimize again...')
top = sb + S6
# a gain in text-1 semibold; a trade-off at the same size in text-2 regular, with the word that says what
# it is and a caption that says why (README: the fan tune aims for 70 C, so the card runs warmer). The
# temperature is the average (decision D14); the README records only peaks (67 -> 71 C), so the averages
# here are illustrative, and the note on the header line says so.
for i, (label, big, small, gain) in enumerate([('Speed', '+1.6 %', '5728 → 5820 it/s', True),
                                               ('Power', '−8 %', '196 → 180 W', True),
                                               ('Fans', '−11 points', '73 → 62 %', True),
                                               ('Average temperature', '+4 °C warmer', '64 → 68 °C; fans aim for 70 °C', False)]):
    s.text(C4.col(i), top, label, 'caption', 'text2', maxw=C4.cw)
    b = s.text(C4.col(i), top + LH['caption'] + S1, big, 'display', 'text1' if gain else 'text2', 600 if gain else 400,
               maxw=C4.cw)
    b = s.text(C4.col(i), b + S1, small, 'caption', 'text3', maxw=C4.cw)
div = b + S6
hairline(s, PAGE_L + PAD, PAGE_R - PAD, div)
top = div + S6
sh = s.text(C2.col(0), top, 'Settings', 'strong')
lx = C2.col(0) + round(width('Settings', 'strong', 600)) + S4
s.text(lx, same_baseline(top, 'strong', 'caption'), 'Show all', 'caption', 'accent_text')
chevron(s, lx + round(width('Show all', 'caption')) + S2, same_baseline(top, 'strong', 'caption') + LH['caption'] / 2, 'down')
lb = sh + S3
for k, v in [('Undervolt', '2850 MHz at 1035 mV'), ('Core offset', '+135 MHz'), ('Memory offset', '+1500 MHz'),
             ('Power limit', '105 %')]:
    s.text(C4.col(0), same_baseline(lb, 'body', 'caption'), k, 'caption', 'text2', maxw=C4.cw)
    s.text(C4.col(1), lb, v, 'body', maxw=C4.cw)
    lb += STEP_PITCH
left_bottom = lb - S2
# the step: one group, its heading with the current value, what it does, the two buttons
s.text(C2.col(1), top, 'Tune step', 'strong')
s.text(C2.col(1) + C2.cw, same_baseline(top, 'strong', 'caption'), 'Current step: 0 %', 'caption', 'text2',
       anchor='end')
rb = s.para(C2.col(1), sh + S3, 'Each −5 % takes 5 % of the gain over stock off the undervolt and the core and '
            'memory offsets; the power limit stays. +5 % gives a step back.', C2.cw) + S3
button(s, C2.col(1), rb, BTN_NARROW, '−5 %')
rb = button(s, C2.col(1) + BTN_NARROW + S2, rb, BTN_NARROW, '+5 %', disabled=True)   # at step 0 (can_step_up)
ft = max(left_bottom, rb) + S6      # the card's last row: keep it, or take it off
toggle_label(s, C4.x, ft, 'Apply at logon')
cb = button(s, C4.col(3), ft, BTN_W, 'Revert to stock')
card(s, PAGE_L, CT, PAGE_W, cb + PAD - CT)

# recent activity: a timeline on the page grid; time in column 1, the mark on the gutter's centre, the
# event from column 2 on
top = cb + PAD + S8
s.text(PAGE_L, top, 'Recent activity', 'strong')
link_end(s, PAGE_R, same_baseline(top, 'strong', 'caption'), 'Open log folder')
top += LH['strong'] + S2
events = [('16 Nov, 09:12', 'No crash in the 2 minutes after logon: the tune is confirmed.', 'event'),
          ('16 Nov, 09:10', 'Applied at logon: All in one.', 'event'),
          ('15 Nov, 22:41', 'Graphics driver reset. Your tune was put back.', 'warning'),
          ('15 Nov, 18:03', 'Applied at logon: All in one.', 'event')]
centres = [top + i * LIST_ROW + LIST_ROW / 2 for i in range(len(events))]
timeline(s, PG.col(1) - GUTTER / 2, centres, [e[2] for e in events], 'bg')
for cy, (t, msg, _) in zip(centres, events):
    lt = cy - LH['body'] / 2
    s.text(PG.col(0), same_baseline(lt, 'body', 'caption'), t, 'caption', 'text3', maxw=PG.cw)
    s.text(PG.col(1), lt, msg, 'body', maxw=PAGE_R - PG.col(1))
s.save('dashboard-after.svg')

# ================================================================ choose
s = Svg('Proposed Optimize page: pick a goal, see what it means, start')
shell(s, 'Optimize')
y, _ = page_header(s, 'Optimize', [('Every result is measured against your card at stock. If it does not hold or does not '
                                    'gain, nothing is kept.', 0)])
CT = y + S8
inner = Grid(PAGE_L + PAD, PAGE_W - 2 * PAD, 8)       # 8 x 85, gutter 24
L = Grid(inner.col(0), inner.span(4), 4)              # list pane, 412
DX = inner.col(4) + LEAD                              # the detail starts a status-icon lead after the divider
D = Grid(DX, PAGE_R - PAD - DX, 4)                    # detail pane, 380: 4 x 77
DIV = inner.col(4)                                    # the divider: 24 px after the list, 32 before the detail
top = CT + PAD
lt = s.text(L.x, top, 'What do you want from your card?', 'strong', maxw=L.w) + S2
ROW_H, ROW_GAP = 68, S1          # 12 + name 22 + 4 + goal 18 + 12
for name, goal, mins, sel in [('All in one', 'Faster, on less power, with quieter fans', '40 min', True),
                              ('Best of my GPU', 'As fast as it stays stable, with a margin', '15 min', False),
                              ('Max performance', 'The most speed; warmer (83 °C) and louder', '15 min', False),
                              ('Undervolt', 'Stock speed on less power and heat', '13 min', False),
                              ('Quiet', 'Lower fan noise; some speed given up', '15 min', False),
                              ('Cool & efficient', 'Coolest (65 °C); speed given up', '15 min', False)]:
    if sel:
        s.rect(L.x, lt, L.w, ROW_H, T['surface3'], 6)
    ct = lt + S3
    rx = L.x + S3                                     # the radio in a 24 px item lead after 12 px padding
    s.circle(rx + 8, ct + LH['strong'] / 2, 8, 'none', T['accent_text'] if sel else T['border_strong'], 1.5)
    if sel:
        s.circle(rx + 8, ct + LH['strong'] / 2, 4, T['accent_text'])
    tx = rx + ITEM_LEAD
    s.text(tx, ct, name, 'strong', maxw=L.x + L.w - S3 - 48 - tx)
    s.text(L.x + L.w - S3, same_baseline(ct, 'strong', 'caption'), mins, 'caption', 'text3', anchor='end')
    s.text(tx, ct + LH['strong'] + S1, goal, 'caption', 'text2', maxw=L.x + L.w - S3 - tx)
    lt += ROW_H + ROW_GAP
list_bottom = lt - ROW_GAP
# the detail answers five questions in order: what it does, what it changes, how long, when it is kept,
# what to know before starting
top = CT + PAD
s.text(D.x, same_baseline(top, 'strong', 'title'), 'All in one', 'title')
s.text(D.x + D.w, same_baseline(top, 'strong', 'caption'), 'Recommended', 'caption', 'text3', anchor='end')
top = s.para(D.x, top + LH['strong'] + S2, 'An overclock with an undervolt on top, then the quietest fan curve that '
             'holds the temperature.', D.w) + S4
for k, v in [('Changes', 'Power limit, core, memory, curve, fans'), ('Takes', 'About 40 minutes of full load'),
             ('Limit', '75 °C'), ('Kept if', 'It holds a 5-minute final test')]:
    s.text(D.col(0), same_baseline(top, 'body', 'caption'), k, 'caption', 'text2', maxw=D.cw)
    s.text(D.col(1), top, v, 'body', maxw=D.span(3))
    top += STEP_PITCH
top = s.text(D.x, top - S2 + S6, 'Before you start', 'strong') + S2
ix = D.x + ITEM_LEAD
lead_icon(s, D.x, top, 'body', 'success')
top = s.text(ix, top, 'No other program is using the card.', 'body', maxw=D.x + D.w - ix)
top = s.text(ix, top, 'Checked again when you press Start.', 'caption', 'text3') + S2
lead_icon(s, D.x, top, 'body', 'info')
top = s.para(ix, top, 'The screen can go black for a few seconds. Save your work first.', D.x + D.w - ix, color='text1') + S2
lead_icon(s, D.x, top, 'body', 'info')
top = s.text(ix, top, 'Abort puts the card back at any moment.', 'body', 'text1', maxw=D.x + D.w - ix) + S4
s.text(D.x, top, 'Advanced options', 'caption', 'accent_text')
chevron(s, D.x + round(width('Advanced options', 'caption')) + S2, top + LH['caption'] / 2, 'right')
detail_bottom = top + LH['caption']
# the decision row, across the card: the switch at the left (as on the Dashboard), Start under the detail
div = max(list_bottom, detail_bottom) + S6
hairline(s, PAGE_L + PAD, PAGE_R - PAD, div)
s.line(DIV, CT + PAD, DIV, div - S6, 'border')
ft = div + S6
toggle_label(s, L.x, ft, 'Keep the result applied at every logon', START_H)
fb = button(s, D.x, ft, D.w, 'Start All in one', 'primary', START_H)
card(s, PAGE_L, CT, PAGE_W, fb + PAD - CT)
s.save('optimize-after.svg')

# ================================================================ run, recovering
s = Svg('Proposed run screen during a driver reset: recovering, with what is confirmed')
shell(s, 'Optimize', running=True)
y, _ = page_header(s, 'Max performance', [('GeForce GTX 1060 6GB  ·  9 of about 15 min (estimate)  ·  1 driver reset', 0)],
                   action='Abort run')
top = y + S6                       # the phase rail on the page's five columns
for i, (name, st) in enumerate([('Stock, 5 min', 'done'), ('Power limit', 'done'), ('Memory', 'done'),
                                ('Core', 'current'), ('Final test, 5 min', 'next')]):
    s.rect(PG.col(i), top, PG.cw, 3, {'done': T['text3'], 'current': T['accent_text'], 'next': T['surface3']}[st], 1.5)
    nx = PG.col(i)
    if st == 'done':               # decision D15: a check before a done phase's name, in its colour; none on the current
        cy = top + 3 + S2 + LH['caption'] / 2
        s.path(f'M{nx + 0.75:g} {cy:g} L{nx + 3.75:g} {cy + 3:g} L{nx + 9.75:g} {cy - 3:g}', 'text2', 1.5)
        nx += CHECK_W + S1
    s.text(nx, top + 3 + S2, name, 'caption', {'done': 'text2', 'current': 'text1', 'next': 'text3'}[st],
           600 if st == 'current' else 400, maxw=PG.col(i) + PG.cw - nx)
y = top + 3 + S2 + LH['caption']
CT = y + S8                        # the activity card: the state, what is confirmed, what comes next
C2 = Grid(CARD.x, CARD.w, 2)       # 2 x 396 on the same 816 px content width
top = CT + PAD
lead_icon(s, PAGE_L + PAD, top, 'page', 'recovering')
s.text(C2.x, top, 'Recovering after a driver reset', 'page')
top = s.para(C2.x, top + LH['page'] + S1, 'The graphics driver was reset while the run tested core +330 MHz. A reset '
             'can happen while a run looks for the card\'s limit. The run reconnects to the driver and checks that '
             'the card works before continuing.', MEASURE(C2.x, C2.w)) + S6
lb = s.text(C2.col(0), top, 'Recovery', 'caption', 'text2') + S2
steps = [('done', 'Reconnected to the driver'), ('done', 'Card set to stock, confirmed by read-back'),
         ('current', 'Resting before the card is loaded again  ·  12 s left'),
         ('next', 'Checking that the card works, 5 s at stock')]
centres = [lb + i * STEP_PITCH + LH['body'] / 2 for i in range(len(steps))]
timeline(s, C2.col(0) + MARK / 2, centres, [st for st, _ in steps], 'surface1')
for cy, (st, t) in zip(centres, steps):
    s.text(C2.col(0) + ITEM_LEAD, cy - LH['body'] / 2, t, 'body', 'text3' if st == 'next' else 'text1',
           600 if st == 'current' else 400, maxw=C2.cw - ITEM_LEAD)
left_bottom = centres[-1] + LH['body'] / 2
rb = label_value(s, C2.col(1), top, 'Your part', 'Nothing. Keep the PC switched on.', C2.cw, 'strong') + S4
rb = s.text(C2.col(1), rb, 'Next', 'caption', 'text2') + S1
rb = s.para(C2.col(1), rb, 'If the check passes, the run goes on below core +330 MHz. If not, it stops and the result '
            'shows the card\'s state.', C2.cw)
cb = max(left_bottom, rb) + PAD
card(s, PAGE_L, CT, PAGE_W, cb - CT)
y = readings(s, cb + S8, [('Core clock', '139 MHz'), ('Memory clock', '405 MHz'), ('Temperature', '58 °C'),
                          ('Power', '11 / 120 W'), ('Fans', '41 %', 'the run')])
top = section_row(s, PAGE_L, PAGE_W, y + S8, 'Voltage/frequency curve', 'On the card now: stock  ·  MHz against mV', True)
GX, GH = CARD.x, 84                # plot: starts on the card's text column; the axis labels end 8 px before it
GW = PAGE_R - GX
GY = top + S4
for f in (1400, 1700, 2000):
    gy = GY + GH - (f - 1300) / 800 * GH
    s.line(GX, gy, GX + GW, gy, 'surface3')
    s.text(GX - S2, gy - LH['caption'] / 2, str(f), 'caption', 'text3', anchor='end')
pts = [(700, 1320), (775, 1500), (850, 1650), (925, 1790), (1000, 1900), (1050, 1960), (1093, 1987), (1200, 1987)]
s.path('M' + ' L'.join(f'{GX + (v - 700) / 500 * GW:.1f} {GY + GH - (f - 1300) / 800 * GH:.1f}' for v, f in pts), 'text2', 1.5)
for v in (700, 800, 900, 1000, 1100, 1200):
    s.text(GX + (v - 700) / 500 * GW, GY + GH + S1, str(v), 'caption', 'text3',
           anchor='start' if v == 700 else 'end' if v == 1200 else 'middle')
top = section_row(s, PAGE_L, PAGE_W, GY + GH + S1 + LH['caption'] + S4, 'Technical log  ·  148 lines')
s.text(PAGE_L, top + S4, 'Mockup. Reset at core +330 MHz and recovery steps from a logged GTX 1060 run (hardware check 113); '
       'curve shape illustrative.', 'caption', 'text3', maxw=PAGE_W)
s.save('run-recovering-after.svg')

# ================================================================ result, not kept
s = Svg('Proposed result screen: a result not kept, with the rule that decided it')
shell(s, 'Optimize')
y, TX = page_header(s, 'Not kept: slower than stock', [('Best of my GPU  ·  GeForce GTX 1060 6GB', None)], icon='notkept')
TW = PAGE_R - TX                                      # 864: everything aligns to the title's text column
top = s.para(TX, y + S6, 'This profile keeps a result only when it is at least as fast as your card at stock. This one '
             'was 1.6 % slower, so the run left it out.', MEASURE(TX, TW))
F = Grid(TX, TW, 2)                                   # 2 x 420
for i, (k, v) in enumerate([('Your card', 'At stock, confirmed by read-back'),
                            ('Saved tune', 'Unchanged: nothing from this run was saved')]):
    b = label_value(s, F.col(i), top + S6, k, v, F.cw)
CT = b + S8
TG = Grid(TX + PAD, TW - 2 * PAD, 12)                 # 12 x 46: the same content grid as the Dashboard's card
# columns: what (units 1-2), the two measurements right-aligned (3-4, 5-6), one empty unit between what was
# measured and how it was judged, the rule (8-9), the outcome (10-12)
NAME_X, STOCK_R, RESULT_R, RULE_X, OUT_X = TG.col(0), TG.col(3) + TG.cw, TG.col(5) + TG.cw, TG.col(7), TG.col(9)
top = CT + PAD
s.text(TG.x, top, 'Compared with stock', 'strong')
s.text(TG.x + TG.w, same_baseline(top, 'strong', 'caption'), 'Five minutes each', 'caption', 'text3', anchor='end')
top += LH['strong'] + S4
s.text(STOCK_R, top, 'Stock', 'caption', 'text2', anchor='end')
s.text(RESULT_R, top, 'This run', 'caption', 'text2', anchor='end')
s.text(RULE_X, top, 'Rule', 'caption', 'text2')
s.text(OUT_X, top, 'Outcome', 'caption', 'text2')
top += LH['caption'] + S2
for i, (k, a, r, rule, out) in enumerate([('Speed', '1086 it/s', '1068 it/s', '≥ 99 % of stock', 'Not met: −1.6 %'),
                                          ('Power limit', '100 %', '70 %', 'Under 75 °C', 'Lowered to stay under it')]):
    hairline(s, TG.x, TG.x + TG.w, top)
    lt = top + (LIST_ROW - LH['body']) / 2
    s.text(NAME_X, lt, k, 'body', 'text2', maxw=TG.span(2))
    s.text(STOCK_R, lt, a, 'body', 'text1', anchor='end', maxw=TG.span(2))
    s.text(RESULT_R, lt, r, 'body', 'text1', 600 if i == 0 else 400, anchor='end', maxw=TG.span(2))
    s.text(RULE_X, lt, rule, 'body', 'text2', maxw=TG.span(2))
    if i == 0:
        lead_icon(s, OUT_X, lt, 'body', 'notkept')
        s.text(OUT_X + ITEM_LEAD, lt, out, 'strong', maxw=TG.span(3) - ITEM_LEAD)
    else:
        s.text(OUT_X, lt, out, 'body', 'text2', maxw=TG.span(3))
    top += LIST_ROW
hairline(s, TG.x, TG.x + TG.w, top)
cb = s.text(TG.x, top + S4, 'Measured: the stress test\'s score and the power limit. Calculated: the percentage.',
            'caption', 'text3', maxw=TG.w) + PAD
card(s, TX, CT, TW, cb - CT)
# the next step follows from the comparison, and promises nothing the other profile has not measured
top = s.text(TX, cb + S8, 'What you can do', 'strong') + S1
top = s.para(TX, top, 'This card ran above 75 °C at stock. Undervolt aims for the stock speed on less power; only its '
             'own run (about 13 min) shows whether it gains on this card.', MEASURE(TX, TW)) + S4
button(s, TX, top, BTN_W, 'Try Undervolt...', 'primary')
bb = button(s, TX + BTN_W + S2, top, BTN_NARROW, 'Back')
top = section_row(s, TX, TW, bb + S8, 'Why: the run\'s steps and the technical log')
s.text(TX, top + S4, 'Mockup with the numbers of a logged GTX 1060 run (hardware check 114 (a)).', 'caption', 'text3')
s.save('result-not-kept-after.svg')
print('ok')

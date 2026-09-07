#!/usr/bin/env python3
"""
Build the NocSif menu ICON FONT (UI-shell P3.2). See README.md.

The done-state mockup (docs/design/full-app-mockup.html) draws its module icons as thin
line icons: `<symbol viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width=W>`
with round caps/joins. An icon FONT (not per-image assets) is the target: it recolors with
a plain text colour (steel at rest -> accent on press), scales with the font size, and drops
straight into a row label — one file, consistent metrics.

Fonts hold FILLED outlines, not strokes, so each icon is stroke-EXPANDED into filled
contours here (the same idea as img/gen_star.py, but emitting vector contours instead of a
raster): every stroked segment becomes a quad, every vertex/endpoint a round disc (round
join/cap), and each `fill="currentColor"` sub-element a filled disc/rect. All contours are
emitted with one winding so TrueType's nonzero fill unions them (rings keep their hole
because nothing covers the centre). fontTools compiles the glyphs into a TTF mapped to a
Private-Use range; `lv_font_conv` then rasterizes it to a 4bpp LVGL font exactly like the
text faces. gen_icons_preview.py renders the same TTF to a PNG contact sheet for eyeballing.

    python gen_icons.py            # writes nocsif_icons.c + nocsif_icons.h (+ .iconwork/nocsif_icons.ttf)

Codepoints start at U+E000 (PUA) in ICON order; the header emits UTF-8 string macros
(NOCSIF_ICON_WIFI ...) so ui.c never hard-codes a codepoint.
"""
import math
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, ".iconwork")
UPM = 1024                 # units per em
VB = 24.0                  # icon viewBox is 24x24
S = UPM / VB               # svg unit -> font unit
NDISC = 24                 # sides of a round join/cap disc
NCURVE = 26                # samples per bezier / arc
FONT_PX = 22               # lv_font_conv raster size (mockup row icon is 22px)
HUB_PX = 48                # P8 v2.2: large cut for the Home-hub celestial motifs (128px circles)
CC_PX = 38                 # P8 v2.4: Control-Center toggle glyphs (USER: 48->42->38, smaller still)
CC_SW_SCALE = 0.8          # P8 v2.4: ...at a LIGHTER stroke than the row/hub cuts (USER: less bold)
XL_PX = 40                 # P8 v2.8: full-set LARGE cut for floating planet glyphs + the icon picker
XL_SW_SCALE = 0.55         # P8 v2.8: ...drawn THINNER than the row/hub cuts (USER: thinner line)

# ---- icon geometry: name -> (root stroke-width, inner SVG) verbatim from the mockup ----
# fill defaults to none (stroked); a child with fill="currentColor" is a filled shape.
ICONS = [
 ("wifi", 1.5, '<path d="M4 9a13 13 0 0 1 16 0M7 12.5a8 8 0 0 1 10 0M9.5 16a4 4 0 0 1 5 0"/><circle cx="12" cy="19" r="1" fill="currentColor"/>'),
 ("nfc", 1.5, '<rect x="3" y="4" width="18" height="16" rx="3"/><path d="M8 15v-6l8 6v-6"/>'),
 ("usb", 1.5, '<path d="M12 21V4M12 4l-2.5 3M12 4l2.5 3"/><path d="M12 14l4-2.5V9"/><path d="M12 11l-3.5-2V7"/><circle cx="15.8" cy="9" r="1.1" fill="currentColor"/>'),
 ("ble", 1.5, '<path d="M5.5 5l13 8-6.5 5V2l6.5 5-13 8"/>'),   # P8 v2.4: enlarged ~1.6x so its ink matches the other icons
 ("sys", 1.3, '<circle cx="12" cy="12" r="3"/><path d="M12 3v3M12 18v3M3 12h3M18 12h3M6 6l2 2M16 16l2 2M18 6l-2 2M8 16l-2 2"/>'),
 ("bell", 1.5, '<path d="M6 9a6 6 0 0 1 12 0c0 4.5 2 5.5 2 5.5H4S6 13.5 6 9"/><path d="M10 20a2 2 0 0 0 4 0"/>'),
 ("flash", 1.5, '<path d="M13 2 5 13h6l-1 9 9-12h-6z"/>'),
 ("hunt", 1.5, '<circle cx="12" cy="12" r="1.6" fill="currentColor"/><path d="M12 6.5a5.5 5.5 0 0 1 5.5 5.5"/><path d="M12 2.5a9.5 9.5 0 0 1 9.5 9.5"/>'),
 ("mon", 1.5, '<path d="M3 12h4l2-5 3 10 2-7 2 4h5"/>'),
 ("ap", 1.5, '<circle cx="12" cy="8" r="2.5"/><path d="M12 10.5V19M8 19h8M7 8a5 5 0 0 1 10 0"/>'),
 ("lock", 1.5, '<rect x="5" y="10" width="14" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>'),
 ("drive", 1.4, '<rect x="3" y="12" width="18" height="7" rx="2"/><path d="M6 12l2-6h8l2 6M8 15.5h.01M11 15.5h5"/>'),
 ("chev", 2.0, '<path d="M9 6l6 6-6 6"/>'),
 ("back", 2.0, '<path d="M15 6l-6 6 6 6"/>'),
 ("up", 2.0, '<path d="M6 15l6-6 6 6"/>'),
 ("down", 2.0, '<path d="M6 9l6 6 6-6"/>'),
 ("star", 0.9, '<path d="M12 2.4C12.5 7.5 12.7 9.3 15.2 10.8 16.7 11.7 18.5 11.9 21.6 12 18.5 12.1 16.7 12.3 15.2 13.2 12.7 14.7 12.5 16.5 12 21.6 11.5 16.5 11.3 14.7 8.8 13.2 7.3 12.3 5.5 12.1 2.4 12 5.5 11.9 7.3 11.7 8.8 10.8 11.3 9.3 11.5 7.5 12 2.4Z"/>'),
 ("radio", 1.5, '<circle cx="12" cy="12" r="2"/><path d="M6.3 6.3a8 8 0 0 0 0 11.4M17.7 6.3a8 8 0 0 1 0 11.4M8.9 8.9a4 4 0 0 0 0 6.2M15.1 8.9a4 4 0 0 1 0 6.2"/>'),
 ("loc", 1.5, '<path d="M12 21s7-5.6 7-11a7 7 0 0 0-14 0c0 5.4 7 11 7 11Z"/><circle cx="12" cy="10" r="2.5"/>'),
 ("clock", 1.5, '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3.5 2"/>'),
 ("mic", 1.5, '<rect x="9" y="3" width="6" height="11" rx="3"/><path d="M6 11a6 6 0 0 0 12 0M12 17v4"/>'),
 ("act", 1.5, '<path d="M3 12h3l2 6 4-14 2 8h5"/>'),
 ("nav", 1.5, '<path d="M12 3l7 16-7-4-7 4z"/>'),
 ("wx", 1.5, '<path d="M7 18a4 4 0 0 1 0-8 5 5 0 0 1 9.6-1.3A3.5 3.5 0 0 1 17 18Z"/>'),
 ("sun", 1.5, '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5 19 19M19 5l-1.5 1.5M6.5 17.5 5 19"/>'),
 ("rain", 1.5, '<path d="M7 15a4 4 0 0 1 0-8 5 5 0 0 1 9.6-1.3A3.5 3.5 0 0 1 17 15Z"/><path d="M8 19l-1 2M12 19l-1 2M16 19l-1 2"/>'),
 ("note", 1.5, '<path d="M6 3h9l3 3v15H6z"/><path d="M9 9h6M9 13h6M9 17h4"/>'),
 ("folder", 1.5, '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>'),
 ("auto", 1.5, '<path d="M12 2v6l3-2M12 22a7 7 0 0 0 7-7 7 7 0 0 0-3-5.7M12 22a7 7 0 0 1-7-7 7 7 0 0 1 3-5.7"/>'),
 ("theme", 1.5, '<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 0 0 18 3 3 0 0 0 0-6 2 2 0 0 1 0-4 3 3 0 0 0 0-8Z"/>'),
 ("batt", 1.4, '<rect x="2" y="8" width="17" height="9" rx="2"/><path d="M22 11v3"/><rect x="4" y="10.5" width="9" height="4" rx="1" fill="currentColor"/>'),
 ("refresh", 1.5, '<path d="M20 11a8 8 0 0 0-14-4L4 9M4 13a8 8 0 0 0 14 4l2-2M4 5v4h4M20 19v-4h-4"/>'),
 ("globe", 1.4, '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c2.5 2.5 3.5 6 3.5 9S14.5 18.5 12 21M12 3C9.5 5.5 8.5 9 8.5 12s1 6.5 3.5 9"/>'),
 ("play", 1.5, '<path d="M7 5l12 7-12 7z"/>'),
 ("phone", 1.5, '<path d="M5 4h4l2 5-2.5 1.5a11 11 0 0 0 5 5L16 13l5 2v4a2 2 0 0 1-2 2A16 16 0 0 1 3 6a2 2 0 0 1 2-2Z"/>'),
 ("key", 1.5, '<circle cx="8" cy="8" r="4"/><path d="M11 11l9 9M17 17l2-2M14 14l2-2"/>'),
 ("msg", 1.5, '<path d="M4 5h16v11H9l-4 3z"/>'),
 ("cast", 1.5, '<path d="M3 6h18v12h-6M3 12a6 6 0 0 1 6 6M3 16a2 2 0 0 1 2 2"/>'),
 # P8 v2.2 — celestial Home-hub motifs (Life=sun[exists], Cyber=moon, System=planet). moon is a
 # stroked crescent path; planet is a stroked circle + a rotated stroked ellipse (Saturn ring).
 # Appended at the END so existing codepoints (U+E000..) never shift.
 ("moon", 1.5, '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5Z"/>'),
 ("planet", 1.5, '<circle cx="12" cy="11" r="6"/><ellipse cx="12" cy="11" rx="11" ry="3.4" transform="rotate(-24 12 11)"/>'),
 # P8 v2.3 — Control Center glyphs: airplane-mode + media prev/next. Appended at the END so
 # existing codepoints (U+E000..E027) never shift (plane=E028, prev=E029, next=E02A).
 ("plane", 1.5, '<path d="M12 3.5c.8 0 1.4 1 1.4 2.3v3.9l7.1 4.1v1.9l-7.1-2v3.7l1.9 1.4v1.5L12 19l-3.2 1.2v-1.5l1.9-1.4v-3.7l-7.1 2v-1.9l7.1-4.1V5.8c0-1.3.6-2.3 1.4-2.3z"/>'),
 ("prev", 1.5, '<path d="M18 6l-8 6 8 6z"/><path d="M7 6v12"/>'),
 ("next", 1.5, '<path d="M6 6l8 6-8 6z"/><path d="M17 6v12"/>'),
 # M7 AMS — a proper speaker (volume) glyph for the Control-Center volume slider (was reusing the
 # document-looking "note"). Appended at the END so existing codepoints never shift (speaker=E02B).
 ("speaker", 1.5, '<path d="M4 9h4l5-4v14l-5-4H4z"/><path d="M16 9a4 4 0 0 1 0 6"/>'),
 # M7 AMS — a PAUSE glyph (two bars) for the Control-Center play/pause toggle. Appended at the END so
 # existing codepoints never shift (pause=E02C); added to the _lg motif cut so the 48px media button has it.
 ("pause", 1.5, '<rect x="7" y="5" width="4" height="14" rx="1.4"/><rect x="13" y="5" width="4" height="14" rx="1.4"/>'),
 # §4.14 — per-condition WEATHER glyphs (the peek chip + Weather screen used to show the one cloud). Same
 # cloud outline as "rain" (lifted so the ground element fits): snow = a six-spoke flake with a centre dot,
 # storm = cloud + a lightning bolt, fog = cloud + two horizon bars. Appended at the END so existing
 # codepoints never shift (snow=E02D, storm=E02E, fog=E02F).
 ("snow", 1.5, '<path d="M12 3v18M4.2 7.5l15.6 9M4.2 16.5l15.6-9"/><path d="M12 3l-2.2 1.3M12 3l2.2 1.3M12 21l-2.2-1.3M12 21l2.2-1.3"/><circle cx="12" cy="12" r="1.2" fill="currentColor"/>'),
 ("storm", 1.5, '<path d="M7 14a4 4 0 0 1 0-8 5 5 0 0 1 9.6-1.3A3.5 3.5 0 0 1 17 14Z"/><path d="M13 13.5l-3 4.5h4l-3 4.5"/>'),
 ("fog", 1.5, '<path d="M7 13a4 4 0 0 1 0-8 5 5 0 0 1 9.6-1.3A3.5 3.5 0 0 1 17 13Z"/><path d="M5 17h14M7 20.5h10"/>'),
]

# ------------------------- SVG path -> polylines ---------------------------------
NUM = re.compile(r'[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?')


def cubic(p0, c1, c2, p3, n=NCURVE):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u*u*u*p0[0] + 3*u*u*t*c1[0] + 3*u*t*t*c2[0] + t*t*t*p3[0],
                    u*u*u*p0[1] + 3*u*u*t*c1[1] + 3*u*t*t*c2[1] + t*t*t*p3[1]))
    return out


def quad(p0, c, p1, n=NCURVE):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u*u*p0[0] + 2*u*t*c[0] + t*t*p1[0],
                    u*u*p0[1] + 2*u*t*c[1] + t*t*p1[1]))
    return out


def arc(p0, rx, ry, phi, large, sweep, p1, n=NCURVE):
    """SVG elliptical arc (endpoint form) -> sampled points (F.6.5)."""
    if p0 == p1:
        return []
    rx, ry = abs(rx), abs(ry)
    if rx == 0 or ry == 0:
        return [p1]
    phi = math.radians(phi)
    cosp, sinp = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0]-p1[0])/2.0, (p0[1]-p1[1])/2.0
    x1p = cosp*dx + sinp*dy
    y1p = -sinp*dx + cosp*dy
    lam = x1p*x1p/(rx*rx) + y1p*y1p/(ry*ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx *= s
        ry *= s
    num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
    den = rx*rx*y1p*y1p + ry*ry*x1p*x1p
    co = math.sqrt(max(0.0, num/den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp = co * rx*y1p/ry
    cyp = co * -ry*x1p/rx
    cx = cosp*cxp - sinp*cyp + (p0[0]+p1[0])/2.0
    cy = sinp*cxp + cosp*cyp + (p0[1]+p1[1])/2.0

    def ang(ux, uy, vx, vy):
        d = math.hypot(ux, uy)*math.hypot(vx, vy)
        c = max(-1.0, min(1.0, (ux*vx+uy*vy)/d))
        a = math.acos(c)
        return -a if (ux*vy-uy*vx) < 0 else a
    th1 = ang(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry)
    dth = ang((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry)
    if not sweep and dth > 0:
        dth -= 2*math.pi
    if sweep and dth < 0:
        dth += 2*math.pi
    out = []
    for i in range(1, n + 1):
        th = th1 + dth * i / n
        x = cosp*rx*math.cos(th) - sinp*ry*math.sin(th) + cx
        y = sinp*rx*math.cos(th) + cosp*ry*math.sin(th) + cy
        out.append((x, y))
    return out


def parse_path(d):
    """Return list of subpaths; each subpath = (list-of-points, closed?).
    Handles M/L/H/V/C/S/Q/T/A/Z (absolute + relative). S/T reflect the previous
    control point (smooth curves — used by i-bell/i-loc/i-globe)."""
    toks = re.findall(r'[MmLlHhVvCcSsQqTtAaZz]|' + NUM.pattern, d)
    i = 0
    subs = []
    pts = []
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    cmd = None
    last_ctrl = None       # previous cubic/quad control point (for S/T reflection)
    last_kind = None       # 'C' or 'Q' — which family the last curve was

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def reflect():
        if last_ctrl is None:
            return cur
        return (2*cur[0] - last_ctrl[0], 2*cur[1] - last_ctrl[1])

    while i < len(toks):
        t = toks[i]
        if re.match(r'[A-Za-z]', t):
            cmd = t
            i += 1
            if cmd in 'Zz':
                if pts:
                    subs.append((pts, True))
                pts = []
                cur = start
                last_ctrl = None
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == 'M':
            x, y = num(), num()
            cur = (cur[0]+x, cur[1]+y) if rel else (x, y)
            if pts:
                subs.append((pts, False))
            pts = [cur]
            start = cur
            last_ctrl = None
            cmd = 'l' if rel else 'L'      # subsequent pairs are implicit lineto
        elif c == 'L':
            x, y = num(), num()
            cur = (cur[0]+x, cur[1]+y) if rel else (x, y)
            pts.append(cur)
            last_ctrl = None
        elif c == 'H':
            x = num()
            cur = (cur[0]+x, cur[1]) if rel else (x, cur[1])
            pts.append(cur)
            last_ctrl = None
        elif c == 'V':
            y = num()
            cur = (cur[0], cur[1]+y) if rel else (cur[0], y)
            pts.append(cur)
            last_ctrl = None
        elif c in ('C', 'S'):
            if c == 'C':
                c1 = (num(), num())
            else:
                c1 = reflect() if last_kind == 'C' else cur
            c2 = (num(), num())
            e = (num(), num())
            if rel:
                if c == 'C':
                    c1 = (cur[0]+c1[0], cur[1]+c1[1])
                c2 = (cur[0]+c2[0], cur[1]+c2[1])
                e = (cur[0]+e[0], cur[1]+e[1])
            pts.extend(cubic(cur, c1, c2, e))
            cur = e
            last_ctrl = c2
            last_kind = 'C'
        elif c in ('Q', 'T'):
            if c == 'Q':
                cc = (num(), num())
                if rel:
                    cc = (cur[0]+cc[0], cur[1]+cc[1])
            else:
                cc = reflect() if last_kind == 'Q' else cur
            e = (num(), num())
            if rel:
                e = (cur[0]+e[0], cur[1]+e[1])
            pts.extend(quad(cur, cc, e))
            cur = e
            last_ctrl = cc
            last_kind = 'Q'
        elif c == 'A':
            rx, ry, phi, la, sw = num(), num(), num(), num(), num()
            e = (num(), num())
            if rel:
                e = (cur[0]+e[0], cur[1]+e[1])
            pts.extend(arc(cur, rx, ry, phi, int(la), int(sw), e))
            cur = e
            last_ctrl = None
        else:
            raise ValueError(f"unhandled path cmd {cmd} in {d!r}")
    if pts:
        subs.append((pts, False))
    return subs


# ------------------------- stroke expansion -> contours --------------------------
def disc(cx, cy, r, n=NDISC):
    return [(cx + r*math.cos(2*math.pi*k/n), cy + r*math.sin(2*math.pi*k/n)) for k in range(n)]


def ellipse_pts(cx, cy, rx, ry, n=72):
    """Sample an axis-aligned ellipse outline (rx != ry) — like disc() but two radii."""
    return [(cx + rx*math.cos(2*math.pi*k/n), cy + ry*math.sin(2*math.pi*k/n)) for k in range(n)]


def parse_rotate(transform):
    """Parse SVG transform='rotate(deg [cx cy])' -> (deg, cx, cy) or None (only rotate handled)."""
    m = re.search(r'rotate\(\s*([-\d.]+)(?:[\s,]+([-\d.]+)[\s,]+([-\d.]+))?\s*\)', transform or "")
    if not m:
        return None
    return (float(m.group(1)),
            float(m.group(2)) if m.group(2) else 0.0,
            float(m.group(3)) if m.group(3) else 0.0)


def apply_rotate(pts, rot):
    """Rotate points by rot=(deg, ox, oy) about (ox, oy)."""
    deg, ox, oy = rot
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    return [((x-ox)*ca - (y-oy)*sa + ox, (x-ox)*sa + (y-oy)*ca + oy) for (x, y) in pts]


def stroke(pts, hw, closed):
    """A stroked polyline -> filled contours: a quad per segment + a disc per vertex
    (round join/cap). Uniform winding so nonzero unions them."""
    contours = []
    n = len(pts)
    segs = range(n) if closed else range(n - 1)
    for i in segs:
        a = pts[i]
        b = pts[(i + 1) % n]
        dx, dy = b[0]-a[0], b[1]-a[1]
        L = math.hypot(dx, dy)
        if L < 1e-9:
            continue
        nx, ny = -dy/L*hw, dx/L*hw
        contours.append([(a[0]+nx, a[1]+ny), (b[0]+nx, b[1]+ny),
                         (b[0]-nx, b[1]-ny), (a[0]-nx, a[1]-ny)])
    for p in pts:                                  # round joins + round caps
        contours.append(disc(p[0], p[1], hw))
    return contours


def rrect(x, y, w, h, rx):
    rx = min(rx, w/2, h/2)
    ry = rx
    pts = []

    def q(cx, cy, a0, a1):
        for k in range(NCURVE + 1):
            a = a0 + (a1-a0)*k/NCURVE
            pts.append((cx + rx*math.cos(a), cy + ry*math.sin(a)))
    # corners CW starting top-left, math angles (y-down svg)
    q(x+rx,   y+ry,   math.pi,       math.pi*1.5)   # TL
    q(x+w-rx, y+ry,   math.pi*1.5,   math.pi*2.0)   # TR
    q(x+w-rx, y+h-ry, 0.0,           math.pi*0.5)   # BR
    q(x+rx,   y+h-ry, math.pi*0.5,   math.pi)       # BL
    return pts


def area(poly):
    s = 0.0
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i+1) % n]
        s += x1*y2 - x2*y1
    return s/2.0


def to_font(poly):
    """svg (y-down, 24) -> font (y-up, UPM); force one winding (positive area)."""
    if area(poly) < 0:
        poly = poly[::-1]
    return [(round(x*S), round((VB - y)*S)) for (x, y) in poly]


def icon_contours(root_sw, inner, sw_scale=1.0):
    """Parse one icon's inner SVG -> list of filled contours in font units. sw_scale thins/thickens the
    stroke (P8 v2.4: the Control-Center cut uses <1 for a lighter weight)."""
    contours = []
    # elements: <path .../>, <circle .../>, <rect .../>
    for m in re.finditer(r'<(path|circle|ellipse|rect)\b([^>]*)/?>', inner):
        tag, attrs = m.group(1), m.group(2)
        A = dict(re.findall(r'([\w-]+)="([^"]*)"', attrs))
        filled = A.get('fill', '') == 'currentColor'
        sw = float(A.get('stroke-width', root_sw)) * sw_scale
        hw = sw / 2.0
        if tag == 'path':
            for pts, closed in parse_path(A['d']):
                if filled:
                    contours.append(pts)               # fill the closed outline directly
                else:
                    contours += stroke(pts, hw, closed)
        elif tag == 'circle':
            cx, cy, r = float(A['cx']), float(A['cy']), float(A['r'])
            if filled:
                contours.append(disc(cx, cy, r, max(NDISC, 28)))
            else:
                ring = disc(cx, cy, r, 64)
                contours += stroke(ring, hw, True)
        elif tag == 'ellipse':
            cx, cy = float(A['cx']), float(A['cy'])
            rx, ry = float(A['rx']), float(A['ry'])
            ring = ellipse_pts(cx, cy, rx, ry, 72)
            rot = parse_rotate(A.get('transform', ''))   # e.g. Saturn ring: rotate(-24 12 11)
            if rot:
                ring = apply_rotate(ring, rot)
            if filled:
                contours.append(ring)
            else:
                contours += stroke(ring, hw, True)
        elif tag == 'rect':
            x, y, w, h = float(A['x']), float(A['y']), float(A['width']), float(A['height'])
            rx = float(A.get('rx', 0))
            poly = rrect(x, y, w, h, rx)
            if filled:
                contours.append(poly)
            else:
                contours += stroke(poly, hw, True)
    return [to_font(c) for c in contours]


# ------------------------- TTF + LVGL emit ---------------------------------------
def build_ttf(path, sw_scale=1.0):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    names = [".notdef"] + ["i_" + n for (n, _, _) in ICONS]
    cmap = {0xE000 + i: "i_" + n for i, (n, _, _) in enumerate(ICONS)}
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    metrics = {".notdef": (UPM, 0)}
    for (n, sw, inner) in ICONS:
        pen = TTGlyphPen(None)
        contours = icon_contours(sw, inner, sw_scale)
        for contour in contours:
            pen.moveTo(contour[0])
            for p in contour[1:]:
                pen.lineTo(p)
            pen.closePath()
        g = pen.glyph()
        gn = "i_" + n
        glyphs[gn] = g
        # lsb = the glyph's real xMin (not 0): lv_font_conv derives each icon's ofs_x from
        # the left side bearing, so lsb=0 would left-justify every icon's ink inside its
        # full-em advance (narrow icons drift left of wide ones, the chevron floats off the
        # right edge). Setting lsb=xMin places the ink at its 24-box design x, so symmetric
        # icons sit centered in the uniform advance and share a common center in the row.
        xs = [p[0] for c in contours for p in c]
        xmin = min(xs) if xs else 0
        metrics[gn] = (UPM, xmin)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=UPM, descent=0)
    fb.setupNameTable({"familyName": "NocSif Icons", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=UPM, sTypoDescender=0, usWinAscent=UPM, usWinDescent=0)
    fb.setupPost()
    fb.save(path)


def normalize_include(path):
    with open(path, "r", encoding="utf-8") as fh:
        txt = fh.read()
    txt = re.sub(
        r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE\s*\n#include "lvgl.h"\s*\n'
        r'#else\s*\n#include "lvgl/lvgl.h"\s*\n#endif',
        '#include "lvgl.h"', txt, count=1)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(txt)


def emit_header():
    lines = [
        "/*",
        " * NocSif menu icon font — codepoint macros (UI-shell P3.2). GENERATED by gen_icons.py.",
        " *",
        " * One 4bpp LVGL bitmap font (nocsif_icons) whose glyphs are the mockup's line icons,",
        " * stroke-expanded to filled outlines. Use a glyph as the text of a label styled with",
        " * &nocsif_icons and recolour it with the normal text colour (steel at rest -> accent on",
        " * press). Codepoints are Private-Use U+E000+; these UTF-8 macros hide the raw value.",
        " */",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "extern const lv_font_t nocsif_icons;",
        "extern const lv_font_t nocsif_icons_lg;   /* P8 v2.2: large celestial motifs (Home hub) */",
        "extern const lv_font_t nocsif_icons_cc;   /* P8 v2.4: 48px Control-Center toggles, lighter stroke */",
        "extern const lv_font_t nocsif_icons_xl;   /* P8 v2.8: full set, large + thin (floating planets, icon picker) */",
        "",
    ]
    for i, (n, _, _) in enumerate(ICONS):
        cp = 0xE000 + i
        b = chr(cp).encode("utf-8")
        esc = "".join("\\x%02X" % x for x in b)
        lines.append('#define NOCSIF_ICON_%-8s "%s"   /* U+%04X */'
                     % (n.upper(), esc, cp))
    lines += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    with open(os.path.join(HERE, "nocsif_icons.h"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def cp_of(name):
    """Codepoint of an icon by name (U+E000 + its index in ICONS)."""
    for i, (n, _, _) in enumerate(ICONS):
        if n == name:
            return 0xE000 + i
    raise KeyError(name)


def run_conv(ttf, size, name, out, ranges):
    """Rasterize `ranges` of the TTF to a 4bpp LVGL font `.c` (one lv_font_t named `name`)."""
    cmd = ["npx", "-y", "lv_font_conv@1.5.3", "--font", ttf, "--size", str(size),
           "--bpp", "4", "--format", "lvgl", "--no-compress"]
    for r in ranges:
        cmd += ["-r", r]
    cmd += ["--lv-font-name", name, "-o", out]
    subprocess.run(cmd, check=True, shell=(os.name == "nt"))
    normalize_include(out)


def main():
    os.makedirs(WORK, exist_ok=True)
    ttf = os.path.join(WORK, "nocsif_icons.ttf")
    build_ttf(ttf)
    print("built TTF:", ttf, "(%d glyphs)" % len(ICONS))
    lo, hi = 0xE000, 0xE000 + len(ICONS) - 1
    # Row icon font — every glyph at the 22px row size.
    run_conv(ttf, FONT_PX, "nocsif_icons", os.path.join(HERE, "nocsif_icons.c"),
             ["0x%X-0x%X" % (lo, hi)])
    # A LARGE cut at HUB_PX: the celestial hub motifs (Life=sun, Cyber=moon, System=planet) for the
    # 128px Home circles + P8 v2.3 the Control-Center media transport (prev/play/next) so those float
    # bigger than the 22px row font. Shares the NOCSIF_ICON_* macros.
    motifs = ["sun", "moon", "planet", "prev", "play", "next", "pause"]   # hub celestial + CC media transport
    run_conv(ttf, HUB_PX, "nocsif_icons_lg", os.path.join(HERE, "nocsif_icons_lg.c"),
             ["0x%X" % cp_of(n) for n in motifs])
    # P8 v2.4 — Control-Center toggle glyphs at CC_PX with a LIGHTER stroke (USER: match the BLE button
    # size + less bold). A SEPARATE, thinner TTF so the 22px row cut and the 48px hub cut keep their
    # normal weight; only these CC buttons get the lighter stroke.
    ttf_cc = os.path.join(WORK, "nocsif_icons_cc.ttf")
    build_ttf(ttf_cc, sw_scale=CC_SW_SCALE)
    cc_glyphs = ["moon", "flash", "plane", "wifi", "ble"]
    run_conv(ttf_cc, CC_PX, "nocsif_icons_cc", os.path.join(HERE, "nocsif_icons_cc.c"),
             ["0x%X" % cp_of(n) for n in cc_glyphs])
    # P8 v2.8 — the WHOLE icon set at a LARGE size + a THIN stroke, for the floating planet glyphs and the
    # planet-icon picker (USER: bigger + thinner + not blank — the _lg cut only carried 6 hub motifs, so
    # every other planet glyph rendered empty).
    ttf_xl = os.path.join(WORK, "nocsif_icons_xl.ttf")
    build_ttf(ttf_xl, sw_scale=XL_SW_SCALE)
    run_conv(ttf_xl, XL_PX, "nocsif_icons_xl", os.path.join(HERE, "nocsif_icons_xl.c"),
             ["0x%X-0x%X" % (lo, hi)])
    emit_header()
    print("wrote nocsif_icons.c (%d @%dpx) + nocsif_icons_lg.c (%d @%dpx) + nocsif_icons_cc.c (%d @%dpx x%.2f) + nocsif_icons.h"
          % (len(ICONS), FONT_PX, len(motifs), HUB_PX, len(cc_glyphs), CC_PX, CC_SW_SCALE))


if __name__ == "__main__":
    main()

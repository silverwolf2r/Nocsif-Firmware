r"""
NocSif Desktop Bridge — the NocSif look for Tk (PLAN §4.15 UI overhaul).

The firmware's own UI is menu-first, near-black, serif titles over mono detail, muted greys, with ONE
accent color the owner chooses on the watch (System › Theme), and an engraved star / orrery-ring motif
sitting behind the Home ring. This module reproduces that in the desktop app:
  - the palette mirrors firmware/src/ui_theme.h's tokens (VOID / PIT / EDGE / ASH / STEEL / BONE / WHITE / GOLD);
  - the fonts are the firmware's own (Fraunces + JetBrains Mono, OFL, bundled under fonts/), loaded
    privately on Windows, falling back to the platform's system serif/mono elsewhere;
  - the accent color is set at RUNTIME: apply_accent() re-colors every accent-bearing style and canvas
    item, and the app calls it with whatever value the watch reports (`version` / `state` carry "accent");
  - Menu: draws the left-hand menu as a canvas widget (rounded selection pill + accent bar, hover wash,
    glyph + serif label) with the orrery motif engraved at the bottom;
  - WatchCard: the header band mirroring the watch's own header (name · version · battery · link dot).
"""
import ctypes
import math
import os
import sys
import tkinter as tk
from tkinter import ttk, font as tkfont

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = getattr(sys, "_MEIPASS", HERE)          # PyInstaller's bundled-data path when running frozen
FONT_DIR = os.path.join(BASE, "fonts")

# ---- palette (mirrors firmware ui_theme.h) -------------------------------------------------------------
VOID, PIT, PIT_ON = "#070708", "#0D0D0F", "#121215"
EDGE, EDGE2 = "#1C1C20", "#28282D"
ASH, STEEL, BONE, WHITE = "#42424A", "#78787F", "#8C8C92", "#A6A6AC"
GOLD = "#C9AD82"
ACCENT_DEFAULT = "#655578"           # the firmware's default violet accent (nocsif_accent index 0)
OK, WARN, BAD = "#4F8A80", "#B8824A", "#9A4F4F"   # teal / amber (firmware presets) / a muted red

_accent = ACCENT_DEFAULT
_listeners = []


def accent():
    return _accent


def on_accent(fn):
    """Registers fn(hex) to be called whenever the accent color changes, so canvases can re-color their tagged items."""
    _listeners.append(fn)


def mix(hex_a, hex_b, t):
    a = tuple(int(hex_a[i:i + 2], 16) for i in (1, 3, 5))
    b = tuple(int(hex_b[i:i + 2], 16) for i in (1, 3, 5))
    return "#%02x%02x%02x" % tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def accent_dk():
    """Matches the firmware's NOCSIF_VIOLET_DK: the current accent darkened to ~30% luminance, used as a fill under an accent border."""
    return mix(VOID, _accent, 0.30)


def normalize_hex(h):
    h = (h or "").strip().lstrip("#")
    if len(h) != 6 or any(c not in "0123456789abcdefABCDEF" for c in h):
        return None
    return "#" + h.lower()


# ---- fonts ---------------------------------------------------------------------------------------
_loaded = {"serif": None, "mono": None}


def load_fonts():
    """Loads the bundled Fraunces + JetBrains Mono fonts privately: on Windows via
    AddFontResourceExW(FR_PRIVATE), which makes them visible only to this process without installing
    them system-wide. On other platforms it falls back to whatever serif/mono fonts the system offers.
    Returns (serif_family, mono_family)."""
    if _loaded["serif"]:
        return _loaded["serif"], _loaded["mono"]
    serif, mono = None, None
    if sys.platform.startswith("win"):
        try:
            FR_PRIVATE = 0x10
            for name in ("Fraunces.ttf", "JetBrainsMono.ttf"):
                path = os.path.join(FONT_DIR, name)
                if os.path.isfile(path):
                    ctypes.windll.gdi32.AddFontResourceExW(path, FR_PRIVATE, 0)
        except Exception:
            pass
    fams = set(tkfont.families()) if tk._default_root else set()
    # the variable-weight Fraunces font registers separate named instances ("Fraunces 9pt",
    # "Fraunces 9pt Light", …) — prefer the plain text-optical regular weight, then any Fraunces
    # instance, then a platform serif as a last resort
    fraunces = sorted(f for f in fams if f.startswith("Fraunces"))
    for cand in ["Fraunces 9pt", "Fraunces"] + fraunces + ["Georgia", "Palatino Linotype", "Palatino", "DejaVu Serif", "Times New Roman"]:
        if cand in fams and not any(cand.endswith(s) for s in (" Thin", " Light", " SemiBold", " Bold", " Black")):
            serif = cand
            break
    for cand in ("JetBrains Mono", "Cascadia Mono", "Consolas", "Menlo", "DejaVu Sans Mono", "Courier New"):
        if cand in fams:
            mono = cand
            break
    _loaded["serif"], _loaded["mono"] = serif or "TkDefaultFont", mono or "TkFixedFont"
    return _loaded["serif"], _loaded["mono"]


class Fonts:
    def __init__(self):
        serif, mono = load_fonts()
        self.serif, self.mono = serif, mono
        self.title = tkfont.Font(family=serif, size=20, weight="normal")
        self.h2 = tkfont.Font(family=serif, size=14)
        self.menu = tkfont.Font(family=serif, size=12)
        self.wordmark = tkfont.Font(family=serif, size=26)
        self.body = tkfont.Font(family=mono, size=10)
        self.small = tkfont.Font(family=mono, size=9)
        self.value = tkfont.Font(family=mono, size=11)
        self.italic = tkfont.Font(family=serif, size=11, slant="italic")


# ---- ttk styling ---------------------------------------------------------------------------------
def apply_styles(style, fonts):
    """Configures every ttk style the app uses, derived from the palette and the current accent color.
    Cheap enough to re-run whenever the accent changes."""
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass
    a = _accent
    style.configure(".", background=VOID, foreground=BONE, fieldbackground=PIT, bordercolor=EDGE,
                    lightcolor=EDGE, darkcolor=EDGE, troughcolor=PIT, font=fonts.body)
    style.configure("TFrame", background=VOID)
    style.configure("Card.TFrame", background=PIT)
    style.configure("TLabel", background=VOID, foreground=BONE, font=fonts.body)
    style.configure("Card.TLabel", background=PIT, foreground=BONE, font=fonts.body)
    style.configure("Dim.TLabel", background=VOID, foreground=STEEL, font=fonts.small)
    style.configure("CardDim.TLabel", background=PIT, foreground=STEEL, font=fonts.small)
    style.configure("Head.TLabel", background=VOID, foreground=WHITE, font=fonts.h2)
    style.configure("CardHead.TLabel", background=PIT, foreground=WHITE, font=fonts.h2)
    style.configure("Value.TLabel", background=PIT, foreground=WHITE, font=fonts.value)
    style.configure("Accent.TLabel", background=VOID, foreground=a, font=fonts.body)
    style.configure("TButton", background=PIT, foreground=BONE, bordercolor=EDGE2, lightcolor=PIT, darkcolor=PIT,
                    focuscolor=PIT, borderwidth=1, padding=(12, 6), font=fonts.body, relief="flat")
    style.map("TButton", background=[("active", PIT_ON), ("pressed", PIT_ON), ("disabled", VOID)],
              foreground=[("active", WHITE), ("disabled", ASH)], bordercolor=[("active", a)])
    style.configure("Accent.TButton", background=accent_dk(), foreground=WHITE, bordercolor=a)
    style.map("Accent.TButton", background=[("active", mix(accent_dk(), a, 0.25))], bordercolor=[("active", a)])
    style.configure("Warn.TButton", foreground=WARN)
    style.map("Warn.TButton", bordercolor=[("active", WARN)])
    style.configure("Bad.TButton", foreground=BAD)
    style.map("Bad.TButton", bordercolor=[("active", BAD)])
    style.configure("Treeview", background=PIT, fieldbackground=PIT, foreground=BONE, bordercolor=EDGE,
                    lightcolor=PIT, darkcolor=PIT, rowheight=24, font=fonts.body)
    style.map("Treeview", background=[("selected", accent_dk())], foreground=[("selected", WHITE)])
    style.configure("Treeview.Heading", background=VOID, foreground=STEEL, font=fonts.small, relief="flat", bordercolor=EDGE)
    style.map("Treeview.Heading", background=[("active", VOID)])
    style.configure("TEntry", fieldbackground=PIT, foreground=WHITE, insertcolor=WHITE, bordercolor=EDGE2,
                    lightcolor=PIT, darkcolor=PIT, padding=4)
    style.map("TEntry", bordercolor=[("focus", a)])
    style.configure("TCombobox", fieldbackground=PIT, foreground=WHITE, background=PIT, bordercolor=EDGE2,
                    arrowcolor=STEEL, lightcolor=PIT, darkcolor=PIT, padding=3)
    style.map("TCombobox", fieldbackground=[("readonly", PIT)], foreground=[("readonly", WHITE)],
              bordercolor=[("focus", a)])
    style.configure("Horizontal.TProgressbar", background=a, troughcolor=PIT, bordercolor=EDGE, lightcolor=a, darkcolor=a)
    style.configure("Horizontal.TScale", background=VOID, troughcolor=PIT, bordercolor=EDGE, lightcolor=a, darkcolor=a)
    style.configure("TCheckbutton", background=VOID, foreground=BONE, font=fonts.body, indicatorbackground=PIT,
                    indicatorforeground=WHITE, indicatormargin=(2, 2, 6, 2))
    style.map("TCheckbutton", indicatorbackground=[("selected", a), ("!selected", PIT)],
              indicatorforeground=[("selected", WHITE)], background=[("active", VOID)])
    style.configure("Card.TCheckbutton", background=PIT, foreground=BONE, font=fonts.body, indicatorbackground=VOID)
    style.map("Card.TCheckbutton", indicatorbackground=[("selected", a), ("!selected", VOID)], background=[("active", PIT)])
    style.configure("TSeparator", background=EDGE)
    style.configure("Vertical.TScrollbar", background=PIT, troughcolor=VOID, bordercolor=VOID, arrowcolor=STEEL)
    style.configure("Horizontal.TScrollbar", background=PIT, troughcolor=VOID, bordercolor=VOID, arrowcolor=STEEL)


def apply_accent(hex_color, style=None, fonts=None):
    """Sets the accent color (typically reported by the watch) and re-colors everything that depends on
    it: the ttk styles plus every registered canvas listener."""
    global _accent
    h = normalize_hex(hex_color)
    if not h or h == _accent:
        return False
    _accent = h
    if style is not None and fonts is not None:
        apply_styles(style, fonts)
    for fn in list(_listeners):
        try:
            fn(h)
        except Exception:
            pass
    return True


# ---- drawing helpers -------------------------------------------------------------------------------
def rounded_rect(canvas, x1, y1, x2, y2, r=8, **kw):
    """Draws a rounded rectangle as a smoothed polygon, since Tk's canvas has no native primitive for one. fill="" draws just the outline."""
    pts = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
           x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    kw.setdefault("outline", "")
    return canvas.create_polygon(pts, smooth=True, splinesteps=16, **kw)


def draw_star(canvas, cx, cy, r, color, width=1, tags=()):
    """Draws the engraved four-point star: two crossed tapered spike lines plus a fainter diagonal cross."""
    ids = [canvas.create_line(cx - r, cy, cx + r, cy, fill=color, width=width, tags=tags),
           canvas.create_line(cx, cy - r, cx, cy + r, fill=color, width=width, tags=tags),
           canvas.create_line(cx - r * 0.35, cy - r * 0.35, cx + r * 0.35, cy + r * 0.35, fill=mix(VOID, color, 0.5), width=1, tags=tags),
           canvas.create_line(cx - r * 0.35, cy + r * 0.35, cx + r * 0.35, cy - r * 0.35, fill=mix(VOID, color, 0.5), width=1, tags=tags)]
    return ids


def draw_orrery(canvas, cx, cy, radius, tags=("motif",)):
    """Draws the Home-ring backdrop: faint concentric rings, one accent-tinted ring, a handful of
    orbiting dots ("planets") and a few engraved stars."""
    for k, f in enumerate((1.0, 0.72, 0.46)):
        rr = radius * f
        col = EDGE2 if k else EDGE
        canvas.create_oval(cx - rr, cy - rr, cx + rr, cy + rr, outline=col, width=1, tags=tags)
    rr = radius * 0.72
    canvas.create_oval(cx - rr, cy - rr, cx + rr, cy + rr, outline=mix(EDGE2, _accent, 0.45), width=1, tags=tags + ("accent-ring",))
    # small dots ("planets") placed along the rings
    for ang, f, size in ((-70, 0.72, 3), (150, 1.0, 2), (30, 0.46, 2)):
        a = math.radians(ang)
        px, py = cx + radius * f * math.cos(a), cy + radius * f * math.sin(a)
        canvas.create_oval(px - size, py - size, px + size, py + size, fill=STEEL, outline="", tags=tags)
    draw_star(canvas, cx, cy, radius * 0.16, STEEL, tags=tags)
    for dx, dy, r in ((-0.85, -0.6, 0.09), (0.7, -0.75, 0.06), (0.95, 0.4, 0.07), (-0.6, 0.85, 0.05)):
        draw_star(canvas, cx + dx * radius, cy + dy * radius, radius * r, ASH, tags=tags)


# ---- the menu widget ------------------------------------------------------------------------------
class Menu(tk.Canvas):
    """The left-hand navigation menu: glyph + serif label rows, a rounded selection pill with an accent
    bar, a hover wash, the orrery motif engraved below the rows, and a footer line.
    items = [(key, glyph, label)]."""
    ROW_H, PAD_X, TOP = 44, 14, 96

    def __init__(self, parent, items, fonts, on_select, width=200, height=620, footer=""):
        super().__init__(parent, width=width, height=height, bg=VOID, highlightthickness=0, bd=0)
        self.items, self.fonts, self.on_select = items, fonts, on_select
        self.w, self.h = width, height
        self.selected = items[0][0]
        self.hover = None
        self.footer = footer
        self.enabled = {k: True for k, _, _ in items}
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda e: self._set_hover(None))
        self.bind("<Button-1>", self._click)
        on_accent(lambda h: self.redraw())
        self.redraw()

    def _row_at(self, y):
        i = int((y - self.TOP) // self.ROW_H)
        return self.items[i][0] if 0 <= i < len(self.items) and y >= self.TOP else None

    def _motion(self, e):
        self._set_hover(self._row_at(e.y))

    def _set_hover(self, key):
        if key != self.hover:
            self.hover = key
            self.redraw()

    def _click(self, e):
        key = self._row_at(e.y)
        if key and self.enabled.get(key, True):
            self.select(key)
            self.on_select(key)

    def select(self, key):
        self.selected = key
        self.redraw()

    def resize(self, height):
        """Follows the parent body's height, so the motif and footer stay pinned to the bottom of whatever vertical space is available."""
        if height > 100 and height != self.h:
            self.h = height
            self.configure(height=height)
            self.redraw()

    def set_enabled(self, key, on):
        self.enabled[key] = on
        self.redraw()

    def redraw(self):
        self.delete("all")
        a = accent()
        # the app wordmark at the top of the menu
        self.create_text(self.PAD_X + 2, 34, text="NocSif", anchor="w", fill=WHITE, font=self.fonts.wordmark)
        self.create_text(self.PAD_X + 4, 62, text="desktop bridge", anchor="w", fill=STEEL, font=self.fonts.small)
        self.create_line(self.PAD_X, 80, self.w - self.PAD_X, 80, fill=EDGE, dash=(2, 4))
        for i, (key, glyph, label) in enumerate(self.items):
            y = self.TOP + i * self.ROW_H
            on = key == self.selected
            en = self.enabled.get(key, True)
            if on:
                rounded_rect(self, self.PAD_X - 4, y + 4, self.w - self.PAD_X + 4, y + self.ROW_H - 4, r=9, fill=PIT_ON, outline="")
                self.create_rectangle(self.PAD_X - 4, y + 12, self.PAD_X - 1, y + self.ROW_H - 12, fill=a, outline="")
            elif key == self.hover and en:
                rounded_rect(self, self.PAD_X - 4, y + 4, self.w - self.PAD_X + 4, y + self.ROW_H - 4, r=9, fill=PIT, outline="")
            col = WHITE if on else (BONE if en else ASH)
            self.create_text(self.PAD_X + 12, y + self.ROW_H / 2, text=glyph, anchor="w", fill=a if on else (STEEL if en else ASH), font=self.fonts.value)
            self.create_text(self.PAD_X + 40, y + self.ROW_H / 2, text=label, anchor="w", fill=col, font=self.fonts.menu)
            if on:
                self.create_text(self.w - self.PAD_X - 6, y + self.ROW_H / 2, text="›", anchor="e", fill=STEEL, font=self.fonts.value)
        # the orrery motif, engraved in the space below the last row
        oy = self.TOP + len(self.items) * self.ROW_H
        space = self.h - oy - 60
        if space > 140:
            draw_orrery(self, self.w / 2, oy + space / 2 + 6, min(self.w * 0.34, space * 0.36))
        if self.footer:
            self.create_text(self.PAD_X + 2, self.h - 22, text=self.footer, anchor="w", fill=ASH, font=self.fonts.small)


# ---- the watch header card ------------------------------------------------------------------------
class WatchCard(tk.Canvas):
    """The header band at the top of the app: shows the same things the watch's own header shows — name,
    version · slot · boot, battery, the link dot — plus a one-line status on the right. set(...)
    redraws it; unset fields default to the "no watch attached" state."""
    H = 88

    def __init__(self, parent, fonts, width=760):
        super().__init__(parent, width=width, height=self.H, bg=VOID, highlightthickness=0, bd=0)
        self.fonts = fonts
        self.w = width
        self.state = {"name": "no watch", "line": "plug a T-Watch Ultra in over USB-C", "batt": None,
                      "linked": False, "kind": "none", "status": ""}
        self.bind("<Configure>", lambda e: (setattr(self, "w", e.width), self.redraw()))
        on_accent(lambda h: self.redraw())
        self.redraw()

    def set(self, **kw):
        self.state.update(kw)
        self.redraw()

    def redraw(self):
        self.delete("all")
        a = accent()
        s = self.state
        rounded_rect(self, 0, 6, self.w, self.H - 6, r=12, fill=PIT, outline=EDGE)
        # the link dot: accent color when the bridge is answering, gold for a stock board, ash otherwise
        dot = a if s["linked"] else (GOLD if s["kind"] == "stock" else ASH)
        self.create_oval(22, self.H / 2 - 5, 32, self.H / 2 + 5, fill=dot, outline="")
        if s["linked"]:
            self.create_oval(18, self.H / 2 - 9, 36, self.H / 2 + 9, outline=mix(PIT, a, 0.35), width=1)
        self.create_text(48, self.H / 2 - 12, text=s["name"], anchor="w", fill=WHITE, font=self.fonts.title)
        self.create_text(50, self.H / 2 + 16, text=s["line"], anchor="w", fill=STEEL, font=self.fonts.small)
        # the battery glyph, drawn on the right side of the card
        if s["batt"] is not None:
            bx, by = self.w - 150, self.H / 2 - 7
            self.create_rectangle(bx, by, bx + 30, by + 14, outline=STEEL, width=1)
            self.create_rectangle(bx + 30, by + 4, bx + 33, by + 10, fill=STEEL, outline="")
            fill_w = max(1, int(26 * min(100, max(0, s["batt"])) / 100))
            self.create_rectangle(bx + 2, by + 2, bx + 2 + fill_w, by + 12, fill=a if s["batt"] > 20 else WARN, outline="")
            self.create_text(bx + 42, by + 7, text="%d%%" % s["batt"], anchor="w", fill=BONE, font=self.fonts.value)
        if s["status"]:
            self.create_text(self.w - 24, self.H / 2 + 16, text=s["status"], anchor="e", fill=STEEL, font=self.fonts.small)


def card(parent, **kw):
    """Returns a dark card-style frame with the hairline border color (a plain tk.Frame, so the border color can be set directly)."""
    f = tk.Frame(parent, bg=PIT, highlightbackground=EDGE, highlightthickness=1, bd=0, **kw)
    return f


def section(parent, text, fonts):
    """Returns a small uppercase mono caption label, matching the web companion page's `.ct` style."""
    return tk.Label(parent, text=text.upper(), bg=VOID, fg=STEEL, font=fonts.small, anchor="w")

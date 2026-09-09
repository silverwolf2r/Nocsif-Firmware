r"""
NocSif Desktop Bridge — window chrome: rounded corners plus an accent highlight that tracks the watch's theme.

Windows 11 (build ≥ 22000): keeps the native window frame and asks DWM for rounded corners, an
accent-colored border, and a near-black caption bar with our own text color — snapping, the taskbar and
resizing all still behave natively.

Windows 10: DWM offers none of that, so the window is made frameless instead — a rounded window REGION
clips the corners, a canvas draws the accent ring along the rounded edge, and a slim custom title strip
carries the icon, title text, drag-to-move, double-click-to-maximize, minimize and close buttons; a
corner grip handles resizing. The taskbar entry is preserved by giving the frameless window the
APPWINDOW extended style.

macOS / Linux: the native window frame is kept as-is (macOS already rounds its corners); only a thin
inner accent hairline is added.

apply(win, fonts, title, …) returns the frame the caller should build its content INTO; set_accent(win,
hex) re-colors the highlight live (wired up via ntheme.on_accent).
"""
import ctypes
import sys
import tkinter as tk

if sys.platform.startswith("win"):
    import ctypes.wintypes   # noqa: F401 — provides RECT, used by the work-area query

import ntheme
from ntheme import VOID, PIT, EDGE, EDGE2, STEEL, BONE, WHITE

RADIUS = 14
RING = 2
INSET = 6
TITLE_H = 36

_win = sys.platform.startswith("win")
_build = sys.getwindowsversion().build if _win else 0
NATIVE_DWM = _win and _build >= 22000


def _hex_to_colorref(h):
    r, g, b = int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)
    return (b << 16) | (g << 8) | r


def _hwnd(win):
    win.update_idletasks()
    return ctypes.windll.user32.GetAncestor(win.winfo_id(), 2)      # GA_ROOT — walk up to the top-level window handle


# ---- Windows 11: DWM attributes -------------------------------------------------------------------
def _dwm_set(hwnd, attr, value):
    v = ctypes.c_int(value)
    ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(v), ctypes.sizeof(v))


def _dwm_apply(win, accent):
    hwnd = _hwnd(win)
    try:
        _dwm_set(hwnd, 20, 1)                                    # DWMWA_USE_IMMERSIVE_DARK_MODE
        _dwm_set(hwnd, 33, 2)                                    # DWMWA_WINDOW_CORNER_PREFERENCE = ROUND
        _dwm_set(hwnd, 34, _hex_to_colorref(accent))             # DWMWA_BORDER_COLOR
        _dwm_set(hwnd, 35, _hex_to_colorref(VOID))               # DWMWA_CAPTION_COLOR
        _dwm_set(hwnd, 36, _hex_to_colorref(WHITE))              # DWMWA_TEXT_COLOR
    except Exception:
        pass


# ---- Windows 10: frameless with our own chrome -----------------------------------------------------
class _Frameless:
    def __init__(self, win, fonts, title, closable=True, resizable=True, minimizable=True, on_close=None):
        self.win, self.fonts, self.title = win, fonts, title
        self.on_close = on_close or win.destroy
        self.maximized = False
        self._restore_geom = None
        win.overrideredirect(True)
        win.configure(bg=VOID)
        self.canvas = tk.Canvas(win, bg=VOID, highlightthickness=0, bd=0)
        self.canvas.pack(fill="both", expand=True)
        self.content = tk.Frame(self.canvas, bg=VOID)
        self.bar = tk.Canvas(self.content, height=TITLE_H, bg=VOID, highlightthickness=0, bd=0)
        self.bar.pack(fill="x")
        self.inner = tk.Frame(self.content, bg=VOID)
        self.inner.pack(fill="both", expand=True)
        self.closable, self.resizable, self.minimizable = closable, resizable, minimizable
        self._cw = self.canvas.create_window(INSET, INSET, anchor="nw", window=self.content)
        self.canvas.bind("<Configure>", self._layout)
        self.bar.bind("<ButtonPress-1>", self._drag_start)
        self.bar.bind("<B1-Motion>", self._drag)
        self.bar.bind("<Double-Button-1>", lambda e: self.toggle_max())
        self.bar.bind("<Motion>", self._bar_hover)
        self.bar.bind("<Leave>", lambda e: self._set_hover(None))
        self.bar.bind("<ButtonRelease-1>", self._bar_click)
        self.hover = None
        self._buttons = []
        if resizable:
            self.grip = tk.Canvas(self.canvas, width=18, height=18, bg=VOID, highlightthickness=0, cursor="size_nw_se")
            self.grip.bind("<ButtonPress-1>", self._resize_start)
            self.grip.bind("<B1-Motion>", self._resize)
        win.after(50, self._taskbar)
        win.bind("<Map>", self._on_map)
        self.draw_bar()

    # recomputes the clip region, accent ring and content placement after a resize
    def _layout(self, e=None):
        w, h = self.canvas.winfo_width(), self.canvas.winfo_height()
        if w < 10 or h < 10:
            return
        self.canvas.delete("ring")
        a = ntheme.accent()
        ntheme.rounded_rect(self.canvas, RING / 2, RING / 2, w - RING / 2, h - RING / 2, r=RADIUS, outline=a, width=RING, fill="", tags=("ring",))
        self.canvas.coords(self._cw, INSET, INSET)
        self.canvas.itemconfigure(self._cw, width=w - 2 * INSET, height=h - 2 * INSET)
        if self.resizable:
            self.grip.place(x=w - 22, y=h - 22)
            self.grip.delete("all")
            for i in range(3):
                self.grip.create_line(16 - i * 5, 16, 16, 16 - i * 5, fill=EDGE2)
        try:
            hwnd = _hwnd(self.win)
            rgn = ctypes.windll.gdi32.CreateRoundRectRgn(0, 0, w + 1, h + 1, RADIUS * 2, RADIUS * 2)
            ctypes.windll.user32.SetWindowRgn(hwnd, rgn, True)
        except Exception:
            pass
        self.draw_bar()

    def set_accent(self, hex_color):
        self._layout()

    # (re)draws the custom title strip: icon dot, title text, and the min/max/close buttons
    def draw_bar(self):
        b = self.bar
        b.delete("all")
        w = max(b.winfo_width(), 200)
        a = ntheme.accent()
        b.create_oval(14, TITLE_H / 2 - 4, 22, TITLE_H / 2 + 4, fill=a, outline="")
        b.create_text(32, TITLE_H / 2, text=self.title, anchor="w", fill=BONE, font=self.fonts.small)
        self._buttons = []
        x = w - 12
        glyphs = [("close", "✕")] if self.closable else []
        if self.minimizable:
            glyphs.insert(0, ("min", "–"))
        if self.resizable:
            glyphs.insert(len(glyphs) - 1 if self.closable else len(glyphs), ("max", "▢"))
        for key, g in reversed(glyphs):
            x0 = x - 30
            fill = PIT if self.hover == key else VOID
            b.create_rectangle(x0, 4, x, TITLE_H - 4, fill=fill, outline="")
            b.create_text((x0 + x) / 2, TITLE_H / 2, text=g, fill=WHITE if self.hover == key else STEEL, font=self.fonts.small)
            self._buttons.append((key, x0, x))
            x = x0 - 2
        b.create_line(12, TITLE_H - 1, w - 12, TITLE_H - 1, fill=EDGE, dash=(2, 4))

    def _button_at(self, x):
        for key, x0, x1 in self._buttons:
            if x0 <= x <= x1:
                return key
        return None

    def _bar_hover(self, e):
        self._set_hover(self._button_at(e.x))

    def _set_hover(self, key):
        if key != self.hover:
            self.hover = key
            self.draw_bar()

    def _bar_click(self, e):
        key = self._button_at(e.x)
        if key == "close":
            self.on_close()
        elif key == "min":
            self.minimize()
        elif key == "max":
            self.toggle_max()

    # move / resize / minimise
    def _drag_start(self, e):
        if self._button_at(e.x):
            self._drag_from = None
            return
        self._drag_from = (e.x_root - self.win.winfo_x(), e.y_root - self.win.winfo_y())

    def _drag(self, e):
        if getattr(self, "_drag_from", None) and not self.maximized:
            self.win.geometry("+%d+%d" % (e.x_root - self._drag_from[0], e.y_root - self._drag_from[1]))

    def _resize_start(self, e):
        self._rs = (e.x_root, e.y_root, self.win.winfo_width(), self.win.winfo_height())

    def _resize(self, e):
        x0, y0, w0, h0 = self._rs
        w = max(self.win.minsize()[0], w0 + e.x_root - x0)
        h = max(self.win.minsize()[1], h0 + e.y_root - y0)
        self.win.geometry("%dx%d" % (w, h))

    def toggle_max(self):
        if not self.resizable:
            return
        if self.maximized:
            self.win.geometry(self._restore_geom)
            self.maximized = False
        else:
            self._restore_geom = self.win.geometry()
            sw, sh = self.win.winfo_screenwidth(), self.win.winfo_screenheight()
            try:                                                   # prefer the work area, excluding the taskbar
                rect = ctypes.wintypes.RECT()
                ctypes.windll.user32.SystemParametersInfoW(0x30, 0, ctypes.byref(rect), 0)
                sw, sh = rect.right - rect.left, rect.bottom - rect.top
                self.win.geometry("%dx%d+%d+%d" % (sw, sh, rect.left, rect.top))
            except Exception:
                self.win.geometry("%dx%d+0+0" % (sw, sh))
            self.maximized = True

    def minimize(self):
        # an overrideredirect window can't be iconified directly: the flag is dropped first, then
        # restored once the window is mapped again (see _on_map)
        self.win.overrideredirect(False)
        self.win.iconify()

    def _on_map(self, e):
        if e.widget is self.win and not self.win.overrideredirect():
            self.win.after(10, lambda: (self.win.overrideredirect(True), self._layout(), self._taskbar()))

    def _taskbar(self):
        """Keeps a taskbar button showing for the frameless window (sets WS_EX_APPWINDOW, clears WS_EX_TOOLWINDOW)."""
        try:
            hwnd = _hwnd(self.win)
            GWL_EXSTYLE, WS_EX_APPWINDOW, WS_EX_TOOLWINDOW = -20, 0x40000, 0x80
            style = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            style = (style | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW
            ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style)
        except Exception:
            pass


# ---- public ----------------------------------------------------------------------------------------
def apply(win, fonts, title, closable=True, resizable=True, minimizable=True, on_close=None):
    """Dresses up `win` with NocSif chrome; returns the frame to build the window's content into, and
    registers the accent-color listener that keeps it in sync."""
    if _win and not NATIVE_DWM:
        fl = _Frameless(win, fonts, title, closable, resizable, minimizable, on_close)
        win._chrome = fl
        ntheme.on_accent(lambda h, fl=fl: fl.set_accent(h))
        return fl.inner
    # native window frame: DWM styling on Win11, nothing extra on macOS/Linux — plus an inner accent hairline either way
    outer = tk.Frame(win, bg=ntheme.accent(), bd=0)
    outer.pack(fill="both", expand=True)
    inner = tk.Frame(outer, bg=VOID)
    inner.pack(fill="both", expand=True, padx=1, pady=1)
    ntheme.on_accent(lambda h, o=outer: o.configure(bg=h))
    if NATIVE_DWM:
        win.after(80, lambda: _dwm_apply(win, ntheme.accent()))
        ntheme.on_accent(lambda h, w=win: _dwm_apply(w, h))
    win._chrome = None
    return inner


def fit(win):
    """Resizes a frameless window to match its content's requested size (a canvas-embedded frame
    doesn't propagate size requests up to the toplevel on its own). Call once after building the
    content; a no-op when the window uses the native frame."""
    fl = getattr(win, "_chrome", None)
    if not fl:
        return
    win.update_idletasks()
    w = fl.content.winfo_reqwidth() + 2 * INSET
    h = fl.content.winfo_reqheight() + 2 * INSET
    win.geometry("%dx%d" % (w, h))
    win.minsize(w, h)


def set_title(win, title):
    fl = getattr(win, "_chrome", None)
    win.title(title)
    if fl:
        fl.title = title
        fl.draw_bar()

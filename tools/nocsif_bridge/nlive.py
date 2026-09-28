r"""
NocSif Desktop Bridge — the live mirror over USB, inside the app (PLAN §4.15 / §4.8a).

The phone Companion is a full-screen live mirror the phone drives by swipe / tap, with a hamburger
(Blank · Wake · Reset), the two side buttons (FN · PWR, hold for long-press) and a keyboard bar when a
watch text field is focused. LiveFrame is that surface, natively in Tk, over the USB console — no
sockets, no browser, nothing outside this process. It IS the Control page: scaled to the room it has
(the panel's own 410×502 pixels once the window is tall enough), polling whenever the page is up.

    pump thread  ── `mirror` polls, as fast as the round trip allows ──►  the watch
                 ◄── the changed RECTANGLE since the last poll (PackBits RLE over RGB565; the dark UI
                     packs 10×+, far smaller than a JPEG of the whole frame would be over the ~110 KB/s
                     console), or "none" / "busy", plus the flags asleep · blank · focused
    mouse        ── press / drag (25 ms) / release, mapped to watch pixels ──► queued; the whole batch
                     rides the NEXT poll ("t": [[x, y, s], …]) so a swipe arrives as a swipe
    FN · PWR · hamburger · keys ──► `ctl` commands, run on the pump thread between polls (in order)

The pump composites each rectangle into a 410×502 Pillow framebuffer and hands the UI thread a frame
to show (only the newest one — the UI never falls behind). "busy" keeps a pending full-frame request
alive (the old native view cleared it and went black); "asleep" shows black, and the watch answers the
first poll after a wake with a full frame. A polling viewer keeps the watch awake (firmware side), like
a connected phone.
"""
import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox

from PIL import Image, ImageDraw, ImageTk

import nbridge
import ntheme
from ntheme import VOID, PIT, PIT_ON, EDGE, EDGE2, STEEL, BONE, WHITE

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = getattr(sys, "_MEIPASS", HERE)
W, H = 410, 502
RADIUS = 26                 # the panel's own rounded corners, as the phone page draws them
IDLE_SLEEP = 0.012          # s between polls when nothing changed and no input is pending
# Always the panel's own pixels — never a softer frame. The bytes are cut instead: the pump keeps a
# 565 shadow of every rectangle it has applied and asks the watch for XOR deltas against it, so a
# redraw that covers the whole screen but changes few pixels (an animated backdrop) packs to a few KB
# where a plain frame is ~25 KB (~300 ms on the ~110 KB/s console). A firmware without deltas simply
# answers with plain rectangles.
MOVE_MS = 25                # drag sample cadence (the phone page's pointermove throttle)
LONG_MS = 500               # side-button long-press (the phone page's hold time)
CHROME_H = 150              # header + FN/PWR row + hint + paddings around the canvas (for scale-to-fit)
MIN_SCALE = 0.4


def _hex(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


class LiveFrame(tk.Frame):
    """The mirror surface. `fit=True` scales the canvas to the frame's height (re-laid out on resize);
    `fit=False` is the panel's 1:1 pixels. Polls only while `active` (the Control page is up)."""

    def __init__(self, parent, app, fit=True):
        super().__init__(parent, bg=VOID)
        self.app = app
        self.fit = fit
        self.scale = 1.0
        f = app.fonts

        # ---- header: ☰  NocSif  fps  ● -----------------------------------------------------------
        hdr = tk.Frame(self, bg=VOID); hdr.pack(fill="x", padx=2, pady=(4, 2))
        ham = tk.Label(hdr, text="☰", bg=VOID, fg=BONE, font=f.h2, cursor="hand2")
        ham.pack(side="left")
        ham.bind("<Button-1>", lambda e: self.drawer(True))
        ham.bind("<Enter>", lambda e: ham.configure(fg=WHITE))
        ham.bind("<Leave>", lambda e: ham.configure(fg=BONE))
        tk.Label(hdr, text="NocSif", bg=VOID, fg=WHITE, font=f.h2).pack(side="left", padx=(10, 0))
        self.dot = tk.Canvas(hdr, width=14, height=14, bg=VOID, highlightthickness=0)
        self.dot.pack(side="right", pady=4)
        self._dot = self.dot.create_oval(2, 2, 12, 12, fill=EDGE2, outline="")
        self.fps_var = tk.StringVar(value="")
        tk.Label(hdr, textvariable=self.fps_var, bg=VOID, fg=STEEL, font=f.small).pack(side="right", padx=(0, 10))

        # ---- the mirror ---------------------------------------------------------------------------
        self.canvas = tk.Canvas(self, width=W, height=H, bg=VOID, highlightthickness=0, cursor="hand2")
        self.canvas.pack(pady=2)
        self._item = self.canvas.create_image(0, 0, anchor="nw")
        self._photo = None
        self._mask = self._bg = None
        self._size = (0, 0)
        self._last_img = None
        self.canvas.bind("<ButtonPress-1>", lambda e: self._touch(e, 1, press=True))
        self.canvas.bind("<B1-Motion>", lambda e: self._touch(e, 1))
        self.canvas.bind("<ButtonRelease-1>", lambda e: self._touch(e, 0))
        self.canvas.bind("<MouseWheel>", self._wheel)                              # Windows / macOS
        self.canvas.bind("<Button-4>", lambda e: self._wheel(e, 1))               # X11 wheel up
        self.canvas.bind("<Button-5>", lambda e: self._wheel(e, -1))              # X11 wheel down
        self._last_move = 0.0

        # ---- FN · PWR (tap = short, hold = long) --------------------------------------------------
        row = tk.Frame(self, bg=VOID); row.pack(fill="x", padx=2, pady=(4, 2))
        self._side_btn(row, "FN", "fn").pack(side="left", fill="x", expand=True, padx=(0, 5))
        self._side_btn(row, "PWR", "pwr").pack(side="left", fill="x", expand=True, padx=(5, 0))
        self.hint = tk.Label(self, text="tap · swipe · drag the dials · wheel scrolls · type into a focused field  ·  hold FN / PWR for a long press", bg=VOID,
                             fg=STEEL, font=f.small, justify="center")
        self.hint.pack(pady=(0, 4))

        # ---- keyboard: the computer's keys go to the watch --------------------------------------------
        # No mode to enter: while this page is up, every plain keystroke is typed into whatever watch
        # text field is focused (the watch ignores keys otherwise), Enter / Backspace edit it. The bar
        # is only a reminder that the watch is listening (the firmware reports a focused field).
        self.kbar = tk.Frame(self, bg=PIT_ON, highlightthickness=1, highlightbackground=ntheme.accent())
        self.kb_lbl = tk.Label(self.kbar, text="watch text field focused — just type  ·  Enter · Backspace",
                               bg=PIT_ON, fg=WHITE, font=f.small, anchor="w")
        self.kb_lbl.pack(side="left", fill="x", expand=True, padx=12, pady=8)
        self._focused = False
        self.bind("<Key>", self._key)
        self.canvas.bind("<Enter>", lambda e: self.focus_set(), add="+")      # keys follow the pointer onto the mirror
        self.canvas.bind("<ButtonPress-1>", lambda e: self.focus_set(), add="+")

        # ---- drawer (hamburger): Blank · Wake · Reset ----------------------------------------------
        self.scrim = tk.Frame(self, bg=VOID, cursor="hand2")
        self.scrim.bind("<Button-1>", lambda e: self.drawer(False))
        self.draw = tk.Frame(self, bg=PIT, highlightthickness=1, highlightbackground=EDGE)
        tk.Label(self.draw, text="Watch", bg=PIT, fg=WHITE, font=f.h2, anchor="w").pack(fill="x", padx=16, pady=(16, 8))
        self.d_blank = self._draw_btn("Blank watch screen", self._blank, is_on=lambda: self.blank)
        self._draw_btn("Wake watch", self._wake)
        self._draw_btn("Reset watch", self._reset)
        self._draw_btn("Save screenshot…", lambda: (self.drawer(False), app.screenshot()))
        tk.Label(self.draw, text="Blank turns the watch panel off so this screen is the display  ·  Wake lights it "
                                 "back up  ·  Reset reboots the watch.\n\nowner use  ·  authorized testing only",
                 bg=PIT, fg=STEEL, font=f.small, justify="left", wraplength=230, anchor="sw").pack(side="bottom", fill="x", padx=16, pady=16)
        ntheme.on_accent(self._recolor)

        # ---- the pump -------------------------------------------------------------------------------
        self.events = []                 # queued touch points (x, y, pressed) in watch pixels
        self.cmds = queue.Queue()        # ctl commands, run in order on the pump thread
        self.lock = threading.Lock()
        self.fb = Image.new("RGB", (W, H), (0, 0, 0))
        self._latest = None
        self._paint_due = False
        self.blank = False
        self.active = False
        self.legacy = False
        self.running = True
        self._set_scale(1.0)
        if fit:
            self.bind("<Configure>", self._on_resize)
        threading.Thread(target=self._pump, name="live-pump", daemon=True).start()

    # ---- size -----------------------------------------------------------------------------------------
    def _on_resize(self, e):
        if e.widget is not self:
            return
        avail = max(100, e.height - CHROME_H - (self.kbar.winfo_reqheight() if self._focused else 0))
        s = max(MIN_SCALE, min(1.0, avail / float(H)))
        if abs(s - self.scale) > 0.01:
            self._set_scale(s)

    def _set_scale(self, s):
        self.scale = s
        w, h = int(round(W * s)), int(round(H * s))
        self._size = (w, h)
        self.canvas.configure(width=w, height=h)
        self.hint.configure(wraplength=w)                     # the column is as wide as the mirror, no wider
        self._mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(self._mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=max(6, int(RADIUS * s)), fill=255)
        self._bg = Image.new("RGB", (w, h), _hex(VOID))
        self._show(self._last_img if self._last_img is not None else self.fb)

    # ---- widgets ------------------------------------------------------------------------------------
    @staticmethod
    def _hover(b, normal_bg=PIT_ON, normal_fg=BONE, hover_bg=EDGE2, hover_fg=WHITE, is_on=None):
        """Mouse-over highlight: a lighter fill + white text while the pointer is on the widget. `is_on`
        (a callable) keeps a toggled widget in its own colours instead."""
        def enter(_e):
            if is_on and is_on():
                return
            b.configure(bg=hover_bg, fg=hover_fg, highlightbackground=ntheme.accent())
        def leave(_e):
            if is_on and is_on():
                return
            b.configure(bg=normal_bg, fg=normal_fg, highlightbackground=EDGE2)
        b.bind("<Enter>", enter, add="+")
        b.bind("<Leave>", leave, add="+")

    def _side_btn(self, parent, text, k):
        b = tk.Label(parent, text=text, bg=PIT_ON, fg=BONE, font=self.app.fonts.value, pady=10, cursor="hand2",
                     highlightthickness=1, highlightbackground=EDGE2)
        state = {"t": None}
        def press(_e):
            b.configure(bg=ntheme.accent_dk(), fg=WHITE, highlightbackground=ntheme.accent())
            state["t"] = b.after(LONG_MS, lambda: (state.update(t=None), self._cmd("button", k=k, l=1)))
        def release(_e):
            b.configure(bg=EDGE2, fg=WHITE)                   # still hovered after the click
            if state["t"] is not None:                       # released before the hold time → short press
                b.after_cancel(state["t"]); state["t"] = None
                self._cmd("button", k=k, l=0)
        b.bind("<ButtonPress-1>", press)
        b.bind("<ButtonRelease-1>", release)
        self._hover(b)
        return b

    def _draw_btn(self, text, cmd, is_on=None):
        b = tk.Label(self.draw, text=text, bg=PIT_ON, fg=BONE, font=self.app.fonts.body, anchor="w", padx=14, pady=10,
                     cursor="hand2", highlightthickness=1, highlightbackground=EDGE2)
        b.pack(fill="x", padx=16, pady=4)
        b.bind("<Button-1>", lambda e: cmd())
        self._hover(b, is_on=is_on)
        return b

    def _wheel(self, e, direction=None):
        """The scroll wheel scrolls the watch: one notch = a short finger drag under the pointer (LVGL
        has no wheel, so the drag IS the scroll). Wheel up drags the content down, like a touchscreen."""
        if direction is None:                                 # Windows / macOS: e.delta; X11: Button-4/5
            direction = 1 if e.delta > 0 else -1
        s = self.scale or 1.0
        x = max(0, min(W - 1, int(e.x / s)))
        y = max(0, min(H - 1, int(e.y / s)))
        dy = 56 * direction
        y0 = max(20, min(H - 21, y))                          # keep the whole drag inside the panel
        pts = [(x, y0, 1)]
        for k in (1, 2, 3, 4):
            pts.append((x, max(0, min(H - 1, y0 + dy * k // 4)), 1))
        pts.append((x, max(0, min(H - 1, y0 + dy)), 0))
        with self.lock:
            self.events.extend(pts)

    def _recolor(self, hex_color):
        try:
            self.kbar.configure(highlightbackground=hex_color)
            self._set_blank_ui(self.blank)
        except tk.TclError:
            pass

    def drawer(self, show):
        if show:
            self.scrim.place(relx=0, rely=0, relwidth=0.3, relheight=1)
            self.draw.place(relx=0.3, rely=0, relwidth=0.7, relheight=1)
            self.scrim.lift(); self.draw.lift()
        else:
            self.scrim.place_forget(); self.draw.place_forget()

    def _set_blank_ui(self, on):
        self.blank = on
        self.d_blank.configure(bg=ntheme.accent_dk() if on else PIT_ON, fg=WHITE if on else BONE,
                               highlightbackground=ntheme.accent() if on else EDGE2)

    # ---- input → the watch ------------------------------------------------------------------------
    def _touch(self, e, pressed, press=False):
        now = time.time()
        if pressed and not press and now - self._last_move < MOVE_MS / 1000.0:
            return                                            # the page's pointermove throttle
        self._last_move = now
        s = self.scale or 1.0
        x = max(0, min(W - 1, int(e.x / s)))
        y = max(0, min(H - 1, int(e.y / s)))
        with self.lock:
            self.events.append((x, y, pressed))

    def _cmd(self, action, **args):
        self.cmds.put((action, args))

    def _blank(self):
        self._set_blank_ui(not self.blank)
        self._cmd("cast", on=1 if self.blank else 0)

    def _wake(self):
        self._cmd("wake")
        self.drawer(False)

    def _reset(self):
        self.drawer(False)
        if messagebox.askyesno("NocSif — live view", "Reboot the watch now?", parent=self.winfo_toplevel()):
            self._cmd("reset")

    def _set_focused(self, on):
        if on == self._focused:
            return
        self._focused = on
        if on:
            self.kbar.pack(fill="x", padx=2, pady=(0, 4))
            self.focus_set()
        else:
            self.kbar.pack_forget()

    def _key(self, e):
        if not self.active or (e.state & 0x4):               # not shown, or a Ctrl shortcut
            return
        if e.keysym == "Return":
            self._cmd("key", key="enter")
        elif e.keysym == "BackSpace":
            self._cmd("key", key="backspace")
        elif e.char and e.char.isprintable():
            self._cmd("type", text=e.char)

    # ---- frames → the canvas ----------------------------------------------------------------------
    def _offer(self, img):
        """Pump thread: hand the newest frame to the UI thread; drop intermediate ones if it is behind."""
        with self.lock:
            self._latest = img
            if self._paint_due:
                return
            self._paint_due = True
        self.app.ui_q.put(self._paint)

    def _paint(self):
        with self.lock:
            img, self._latest = self._latest, None
            self._paint_due = False
        if img is not None and self.running:
            self._show(img)

    def _show(self, img):
        self._last_img = img
        w, h = self._size
        src = img if (w, h) == (W, H) else img.resize((w, h), Image.BILINEAR)
        out = self._bg.copy()
        out.paste(src, (0, 0), self._mask)                    # the panel's rounded corners
        try:
            self._photo = ImageTk.PhotoImage(out)
            self.canvas.itemconfigure(self._item, image=self._photo)
        except tk.TclError:
            pass

    def _ui(self, fn):
        self.app.ui_q.put(fn)

    def _dot_set(self, on):
        self._ui(lambda: self.dot.itemconfigure(self._dot, fill=ntheme.accent() if on else EDGE2))

    # ---- lifecycle --------------------------------------------------------------------------------
    def set_active(self, on):
        """Poll only while shown (the Control page is up and a watch is connected)."""
        self.active = on

    def stop(self):
        self.running = False
        if self.blank:
            try:
                self.app.ctl("cast", on=0, quiet=True)        # never leave the watch dark behind us
            except Exception:
                pass

    # ---- the pump ---------------------------------------------------------------------------------
    def _run_cmds(self, b):
        while True:
            try:
                action, args = self.cmds.get_nowait()
            except queue.Empty:
                return
            try:
                b.ctl(action, **args)
            except nbridge.BridgeError as e:
                s = str(e)
                if "unknown action" in s and action == "wake":      # an older firmware: PWR short wakes the panel
                    try: b.ctl("button", k="pwr", l=0)
                    except nbridge.BridgeError: pass
                elif "unknown action" in s and action == "reset":
                    try: b.reboot()
                    except nbridge.BridgeError: pass
                elif "port closed" in s:
                    return
            except Exception:
                return

    def _pump(self):
        seq, full = 0, True
        black_shown = False
        frames, nbytes, t0 = 0, 0, time.time()
        online = None
        shadow = bytearray(W * H * 2)   # RGB565-LE copy of every rectangle applied (the delta base)
        while self.running:
            b = self.app.bridge
            if not self.active or b is None or not b.alive:
                if online is not False:
                    online = False; self._dot_set(False)
                    self._ui(lambda: self.fps_var.set("" if self.active else "paused"))
                    if b is None or not b.alive:
                        self._ui(lambda: self.fps_var.set("waiting for the watch"))
                time.sleep(0.25)
                full = True
                continue
            self._run_cmds(b)
            with self.lock:
                if self.legacy:                 # a firmware before the point list: ONE point per poll
                    pts, self.events = self.events[:1], self.events[1:]
                else:
                    pts, self.events = self.events, []
            try:
                final, raw = b.mirror_poll(seq, full=full, touches=pts or None, delta=True)
            except nbridge.BridgeError as e:
                if "timeout" in str(e):
                    full = True
                time.sleep(0.2)
                continue
            except Exception:
                time.sleep(0.5)
                full = True
                continue
            if online is not True:
                online = True; self._dot_set(True)
            # A reply without the viewer flags = a firmware before this protocol: no busy / asleep /
            # focused, one touch point per poll, wake and reset via PWR / reboot (see _run_cmds).
            self.legacy = "asleep" not in final
            self._ui(lambda f=bool(final.get("focused")): self._set_focused(f))
            self._ui(lambda v=bool(final.get("blank")): self._set_blank_ui(v))
            if final.get("asleep") and not final.get("blank"):
                if not black_shown:                            # the panel is dark: show it dark, once
                    black_shown = True
                    self.fb.paste((0, 0, 0), (0, 0, W, H))
                    self._offer(self.fb.copy())
                    self._ui(lambda: self.fps_var.set("watch asleep"))
                full = True                                    # whatever comes next is a fresh start
                time.sleep(0.1)
                continue
            if black_shown:
                black_shown = False
                full = True                                    # the watch forces a full frame on wake too
            if final.get("busy"):
                time.sleep(IDLE_SLEEP)                         # keep `full` as it was — nothing was delivered
                continue
            if final.get("none"):
                if not pts:
                    time.sleep(IDLE_SLEEP)
                continue
            if len(raw) != int(final.get("raw", -1)):
                full = True                                    # a short reply: resync with a full frame
                continue
            full = False
            seq = int(final["seq"])
            x, y, w, h = int(final["x"]), int(final["y"]), int(final["w"]), int(final["h"])
            sc = int(final.get("scale", 1))
            if sc == 1:
                # apply into the shadow (XOR for a delta, copy otherwise); the rectangle's new pixels
                # then come straight out of it. Full-width rectangles are one contiguous slice.
                is_delta = bool(final.get("delta"))
                rw = w * 2
                if x == 0 and w == W:
                    off = y * W * 2
                    if is_delta:
                        cur = int.from_bytes(shadow[off:off + len(raw)], "little") ^ int.from_bytes(raw, "little")
                        shadow[off:off + len(raw)] = cur.to_bytes(len(raw), "little")
                    else:
                        shadow[off:off + len(raw)] = raw
                    px = bytes(shadow[off:off + len(raw)])
                else:
                    rows = []
                    for yy in range(h):
                        off = ((y + yy) * W + x) * 2
                        seg = raw[yy * rw:(yy + 1) * rw]
                        if is_delta:
                            seg = (int.from_bytes(shadow[off:off + rw], "little") ^ int.from_bytes(seg, "little")).to_bytes(rw, "little")
                        shadow[off:off + rw] = seg
                        rows.append(seg)
                    px = b"".join(rows)
                patch = Image.frombytes("RGB", (w, h), px, "raw", "BGR;16")
                self.fb.paste(patch, (x, y))
            else:                                              # a half-res rectangle (never requested here)
                patch = Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16").resize((w * sc, h * sc), Image.BILINEAR)
                self.fb.paste(patch, (x * sc, y * sc))
            self._offer(self.fb.copy())
            frames += 1
            nbytes += len(raw)
            now = time.time()
            if now - t0 >= 1.0:
                fps, kb = frames / (now - t0), nbytes / 1024.0 / (now - t0)
                self._ui(lambda s="%.0f fps · %.0f KB/s" % (fps, kb): self.fps_var.set(s))
                frames, nbytes, t0 = 0, 0, now

r"""
NocSif Desktop Bridge — the desktop companion GUI for the T-Watch Ultra (PLAN §4.15).

    python nocsif_bridge_app.py            (Windows / macOS / Linux; Python 3.9+, Tk 8.6)

A single build that works with whatever T-Watch Ultra is plugged in over USB-C:
  - a watch already running NocSif gets the full feature set — health board, files, control + live
    view, update, backups;
  - a stock watch (LilyGo firmware) or a blank board is instead identified from the ROM side, and can
    be backed up, flashed with NocSif, flashed with LilyGo's factory firmware, restored from a backup,
    or run through the ROM-level checks. Tests that need firmware cooperation are marked as such (they
    become available once the RAM diagnostic, P2, is on the board).

Visually this follows the firmware's own theme (see ntheme.py): near-black background, serif titles
over mono detail, muted greys, the accent color the owner picked on the watch (picked up automatically
from the watch on connect), the engraved star/orrery motif, and a left-hand menu.
"""
import collections
import datetime as dt
import json
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = getattr(sys, "_MEIPASS", HERE)           # PyInstaller's extracted-data path (fonts, icon) when frozen
sys.path.insert(0, HERE)
import nbridge      # noqa: E402
import flasher      # noqa: E402
import updater      # noqa: E402
import ntheme       # noqa: E402
import nchrome      # noqa: E402
from ntheme import VOID, PIT, PIT_ON, EDGE, EDGE2, ASH, STEEL, BONE, WHITE, GOLD, OK, WARN, BAD   # noqa: E402

APP_NAME = "NocSif Desktop Bridge"
APP_VERSION = "0.3.5"       # parsed by release_app.ps1; GitHub releases are tagged app-v<APP_VERSION>
GITHUB_URL = "https://github.com/silverwolf2r/Nocsif-Firmware"
RELEASES_URL = GITHUB_URL + "/releases"
WEBSITE_URL = ""            # left blank; fill in eigencat.org once the operator wants the link shown
CREDITS = "NocSif firmware + bridge by silverwolf2r"
HOME_DIR = os.path.join(os.path.expanduser("~"), ".nocsif_bridge")
CACHE_DIR = os.path.join(HOME_DIR, "releases")
BACKUP_DIR = os.path.join(HOME_DIR, "backups")
LILYGO_DIR = os.path.join(HOME_DIR, "lilygo")
SETTINGS_PATH = os.path.join(HOME_DIR, "settings.json")
COMPANION_URL = "http://nocsif.local/"

MENU = [("watch", "◐", "Watch"), ("health", "✦", "Health"), ("flash", "↯", "Flash"),
        ("files", "▤", "Files"), ("control", "⌖", "Control"), ("log", "≡", "Log")]
NEEDS_BRIDGE = ("files", "control", "log")


def load_settings():
    try:
        with open(SETTINGS_PATH, encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return {}


def save_settings(d):
    try:
        os.makedirs(HOME_DIR, exist_ok=True)
        with open(SETTINGS_PATH, "w", encoding="utf-8") as fh:
            json.dump(d, fh, indent=2)
    except Exception:
        pass


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_NAME)
        self.geometry("1040x720")
        self.minsize(900, 600)
        self.configure(bg=VOID)
        self.settings = load_settings()
        self.fonts = ntheme.Fonts()
        self.style = ttk.Style(self)
        ntheme.apply_styles(self.style, self.fonts)      # begins in the default NocSif purple until a watch reports its own accent
        self._icon()
        self.root = nchrome.apply(self, self.fonts, APP_NAME, on_close=self._on_close)
        self.protocol("WM_DELETE_WINDOW", self._on_close)   # native-frame X / Alt-F4 (the frameless ✕ uses on_close above)
        self.bridge = None
        self.kind = "none"           # one of: none | nocsif | stock | blank | silent
        self.ident = None            # holds flasher.identify()'s result when the bridge doesn't answer
        self.port = None
        self.busy = False
        self.log_q = queue.Queue()
        self.ui_q = queue.Queue()
        self.manifest = None
        self.version = None
        self.shot_img = None
        self._slider_t = {}
        self._shot_png = None
        self._failed_ports = {}
        self._probed_ports = {}
        self.live = None
        self._app_release = None
        self._build()
        self.after(100, self._tick)
        self.refresh_ports()
        self.after(1200, self._autoconnect_tick)
        threading.Thread(target=self._check_app_update, daemon=True).start()

    def _icon(self):
        try:
            if sys.platform.startswith("win"):
                self.iconbitmap(os.path.join(BASE, "nocsif.ico"))
            else:
                self._icon_img = tk.PhotoImage(file=os.path.join(BASE, "nocsif_icon.png"))
                self.iconphoto(True, self._icon_img)
        except Exception:
            pass

    # ---- window layout ---------------------------------------------------------------------------
    def _build(self):
        self.card = ntheme.WatchCard(self.root, self.fonts)
        self.card.pack(side="top", fill="x", padx=14, pady=(8, 6))
        foot = tk.Frame(self.root, bg=VOID)              # packed ahead of the body frame so it can never get clipped
        foot.pack(side="bottom", fill="x", padx=16, pady=(0, 6))
        body = tk.Frame(self.root, bg=VOID)
        body.pack(side="top", fill="both", expand=True, padx=14, pady=(0, 6))
        self.menu = ntheme.Menu(body, MENU, self.fonts, self.show_page, width=196, height=520,
                                footer="v%s · %s" % (APP_VERSION, "silverwolf2r"))
        self.menu.pack(side="left", fill="y")
        body.bind("<Configure>", lambda e: self.menu.resize(e.height))
        self.content = tk.Frame(body, bg=VOID)
        self.content.pack(side="left", fill="both", expand=True, padx=(14, 0))
        self.pages = {}
        for key, _, _ in MENU:
            f = tk.Frame(self.content, bg=VOID)
            f.place(relx=0, rely=0, relwidth=1, relheight=1)
            self.pages[key] = f
        self._build_watch(); self._build_health(); self._build_flash()
        self._build_files(); self._build_control(); self._build_log()
        foot = tk.Frame(self.root, bg=VOID)
        foot.pack(fill="x", padx=16, pady=(0, 8))
        self.status_var = tk.StringVar(value="waiting for a watch on USB")
        tk.Label(foot, textvariable=self.status_var, bg=VOID, fg=STEEL, font=self.fonts.small, anchor="w").pack(side="left")
        self._link(foot, "GitHub", GITHUB_URL).pack(side="right")
        self.upd_link = self._link(foot, "", RELEASES_URL)
        self.upd_link.pack(side="right", padx=14)
        if WEBSITE_URL:
            self._link(foot, "website", WEBSITE_URL).pack(side="right", padx=(0, 12))
        self.port_var = tk.StringVar()
        self.show_page("watch")
        self.update_menu_state()

    def _link(self, parent, text, url):
        lbl = tk.Label(parent, text=text, fg=ntheme.accent(), bg=VOID, cursor="hand2", font=self.fonts.small)
        lbl.bind("<Button-1>", lambda e: webbrowser.open(url))
        ntheme.on_accent(lambda h, l=lbl: l.configure(fg=h))
        return lbl

    def show_page(self, key):
        self.pages[key].lift()
        self.menu.select(key)

    def update_menu_state(self):
        on = self.bridge is not None
        for key in NEEDS_BRIDGE:
            self.menu.set_enabled(key, on)
        self._refresh_flash_sd_enabled()

    def set_status(self, text):
        self.status_var.set(text)

    # ---- busy / interrupt guards ---------------------------------------------------------------
    def _busy_block(self, what="that"):
        """True (and warns) when a flash or backup is already running; the caller should then return.
        Guards every action that would grab the USB port out from under an operation in progress."""
        if self.busy:
            messagebox.showwarning(APP_NAME, "A flash or backup is still running.\n\n"
                                   "Wait for it to finish — watch the Flash tab — before starting %s." % what)
            return True
        return False

    def _on_close(self):
        """Window close (the frameless ✕, the native title-bar X, or Alt-F4). Warn before quitting in the
        middle of an operation — interrupting a flash can leave the watch half-written."""
        if self.busy and not messagebox.askyesno(
                APP_NAME, "A flash or backup is still running.\n\nQuitting now interrupts it — the watch "
                "can be left half-written and may need another download-mode entry and a re-flash.\n\n"
                "Quit anyway?", icon="warning"):
            return
        self.destroy()

    def _post_flash_notice(self):
        """After a flash the watch is sitting in the manually-entered download mode; auto-reset over the
        native USB-Serial/JTAG port is unreliable, so tell the user to tap RST to boot the new firmware."""
        messagebox.showinfo(APP_NAME, "Flashing complete.\n\nIf the watch screen is BLANK it is still in "
                            "download mode — press the RESET (RST) button on the watch once to start the "
                            "firmware. The app reconnects on its own once it boots.")

    # ---- Watch page (the NocSif overview, or a landing page for anything else) ---------------------
    def _build_watch(self):
        f = self.pages["watch"]
        # the overview frame shown when a NocSif watch is connected
        self.ov_frame = ntheme.card(f)
        grid = tk.Frame(self.ov_frame, bg=PIT); grid.pack(anchor="w", padx=18, pady=14, fill="x")
        self.ov = {}
        rows = [("device", "name"), ("running", "version"), ("slot", "slot"), ("last boot", "boot"), ("last crash", "crash"),
                ("battery", "batt"), ("uptime", "uptime"), ("MAC", "mac"), ("accent", "accent"),
                ("published", "published"), ("update", "update")]
        for i, (label, key) in enumerate(rows):
            tk.Label(grid, text=label, bg=PIT, fg=STEEL, font=self.fonts.small, anchor="w").grid(row=i, column=0, sticky="w", padx=(0, 22), pady=3)
            v = tk.StringVar(value="—")
            self.ov[key] = v
            tk.Label(grid, textvariable=v, bg=PIT, fg=BONE if key not in ("name", "version") else WHITE,
                     font=self.fonts.value if key in ("name", "version") else self.fonts.body, anchor="w",
                     justify="left", wraplength=640).grid(row=i, column=1, sticky="w", pady=3)
        btns = tk.Frame(self.ov_frame, bg=PIT); btns.pack(anchor="w", padx=18, pady=(0, 14))
        ttk.Button(btns, text="Refresh", command=self.load_overview).pack(side="left")
        ttk.Button(btns, text="Check published version", command=self.check_published).pack(side="left", padx=8)
        self.update_btn = ttk.Button(btns, text="Update watch", style="Accent.TButton", command=self.update_watch, state="disabled")
        self.update_btn.pack(side="left")
        # the alternate frame shown for a stock/blank/silent board, or when nothing is plugged in
        self.land_frame = ntheme.card(f)
        inner = tk.Frame(self.land_frame, bg=PIT); inner.pack(fill="both", expand=True, padx=18, pady=16)
        self.land_title = tk.Label(inner, text="No watch attached", bg=PIT, fg=WHITE, font=self.fonts.h2, anchor="w")
        self.land_title.pack(anchor="w")
        self.land_text = tk.Label(inner, text="Plug a T-Watch Ultra in over USB-C. The app finds it on its own.", bg=PIT, fg=BONE,
                                  font=self.fonts.body, justify="left", anchor="w", wraplength=680)
        self.land_text.pack(anchor="w", pady=(6, 12))
        self.land_btns = tk.Frame(inner, bg=PIT); self.land_btns.pack(anchor="w")
        self.land_frame.pack(fill="x", padx=2, pady=6)

    def _show_watch_page(self):
        self.ov_frame.pack_forget()
        self.land_frame.pack_forget()
        if self.kind == "nocsif":
            self.ov_frame.pack(fill="x", padx=2, pady=6)
        else:
            self.land_frame.pack(fill="x", padx=2, pady=6)
            self._fill_landing()

    def _fill_landing(self):
        for w in self.land_btns.winfo_children():
            w.destroy()
        if self.kind == "none":
            self.land_title.configure(text="No watch attached")
            self.land_text.configure(text="Plug a T-Watch Ultra in over USB-C. The app finds it on its own — a NocSif watch connects "
                                          "straight away; a stock LilyGo watch or a blank board is identified through the chip's "
                                          "ROM loader.")
            return
        idn = self.ident or {}
        rom = idn.get("rom") or {}
        ard = idn.get("arduino")
        noc = idn.get("nocsif")
        if self.kind == "stock":
            fw = "%s %s" % (ard.get("project") or "LilyGo firmware", ard.get("version") or "")
            self.land_title.configure(text="Stock T-Watch Ultra — %s" % fw.strip())
            self.land_text.configure(text=(
                "This watch runs its factory (Arduino) firmware, built %s %s. Chip %s · flash %s · MAC %s.\n\n"
                "Without NocSif on it the app can: back the whole flash up, put NocSif on it, put LilyGo's factory image "
                "on it, restore a backup, and run the ROM-level checks in Health. Files, Control, the live view and the "
                "full hardware board need NocSif (or the RAM diagnostic, coming next).\n\n"
                "Recommended order: Back up → Flash NocSif. The backup restores the watch exactly as it is now.")
                % (ard.get("date", ""), ard.get("time", ""), rom.get("chip") or "ESP32-S3", rom.get("flash_size") or "?", rom.get("mac") or "?"))
        elif self.kind == "silent":
            self.land_title.configure(text="NocSif %s — not answering" % (noc.get("version") if noc else ""))
            self.land_text.configure(text=("A NocSif image is on the board but its bridge did not answer — an older build without the "
                                           "desktop bridge, or the watch is busy in a USB mode (File Share / keyboard). Set USB back to "
                                           "Detached on the watch, or update it from here: Update flashes the published NocSif app "
                                           "over USB and keeps the settings."))
        else:
            self.land_title.configure(text="Blank or unknown board")
            self.land_text.configure(text=("The chip answers (%s, flash %s, MAC %s) but no recognisable app image was found at the NocSif "
                                           "or Arduino offsets. Flash new watch puts the complete NocSif firmware on it; LilyGo's factory "
                                           "image is the other option.")
                                     % (rom.get("chip") or "ESP32-S3", rom.get("flash_size") or "?", rom.get("mac") or "?"))
        if self.kind in ("stock", "blank"):
            ttk.Button(self.land_btns, text="Back up this watch", command=self.backup_watch).pack(side="left")
            ttk.Button(self.land_btns, text="Flash NocSif", style="Accent.TButton", command=self.flash_new_watch).pack(side="left", padx=8)
            ttk.Button(self.land_btns, text="LilyGo factory firmware…", command=lambda: self.show_page("flash")).pack(side="left")
        elif self.kind == "silent":
            ttk.Button(self.land_btns, text="Update NocSif (USB)", style="Accent.TButton", command=self.update_watch).pack(side="left")
            ttk.Button(self.land_btns, text="Back up this watch", command=self.backup_watch).pack(side="left", padx=8)
        ttk.Button(self.land_btns, text="Re-check", command=lambda: self.identify_port(self.port, force=True)).pack(side="left", padx=8)

    def load_overview(self):
        def work(b):
            return b.version(), b.status()
        def done(res):
            v, s = res
            self.version = v
            self.ov["name"].set(v.get("name") or "NocSif")
            self.ov["version"].set("%s  ·  built %s  ·  IDF %s  ·  elf %s" % (v.get("version"), v.get("build"), v.get("idf"), v.get("elf")))
            self.ov["slot"].set("%s · %s%s" % (v.get("slot"), v.get("ota_state"), "  · SAFE MODE" if v.get("safe") else ""))
            self.ov["boot"].set(v.get("boot", "—"))
            self.ov["crash"].set(v.get("crash") or "none recorded")
            self.ov["batt"].set("%s%%  ·  %s" % (s.get("batt"), "on USB power" if s.get("vbus") else "on battery"))
            self.ov["uptime"].set("%d s" % s.get("uptime_s", 0))
            self.ov["mac"].set(v.get("mac", "—"))
            acc = ntheme.normalize_hex(v.get("accent", ""))
            if acc:
                self.ov["accent"].set(acc + "  ·  System › Theme on the watch — the app wears it")
                ntheme.apply_accent(acc, self.style, self.fonts)      # recolor the whole app to match the watch's theme
            else:
                self.ov["accent"].set("not reported (older firmware) — NocSif purple")
            self.card.set(name=v.get("name") or "NocSif", line="NocSif %s  ·  %s  ·  %s" % (v.get("version"), v.get("slot"), v.get("boot")),
                          batt=s.get("batt"), linked=True, kind="nocsif", status="connected on %s" % self.port)
            self._compare_versions()
        self.run_bridge(work, done, "reading the watch")

    def check_published(self):
        def work():
            return updater.fetch_manifest(updater.DEFAULT_REPO)
        def done(m):
            self.manifest = m
            parts = ", ".join(p["file"] for p in m.get("parts", [])) or "firmware.bin"
            self.ov["published"].set("%s  ·  built %s  ·  %s  ·  %s" % (m.get("version"), m.get("build"), nbridge.human_size(m.get("size")), parts))
            self._compare_versions()
        self.run_bg(work, done, "fetching manifest.json from GitHub")

    def _compare_versions(self):
        if not (self.version and self.manifest):
            return
        rel = updater.compare_versions(self.version.get("version"), self.manifest.get("version"))
        self.ov["update"].set({"same": "the watch runs the published version", "differs": "the watch runs a different build than the published one",
                               "unknown": "—"}[rel])
        self.update_btn.configure(state="normal" if self.bridge else "disabled")

    # ---- Health page -------------------------------------------------------------------------------
    def _build_health(self):
        f = self.pages["health"]
        bar = tk.Frame(f, bg=VOID); bar.pack(fill="x", pady=(6, 4))
        ttk.Button(bar, text="Run hardware check", style="Accent.TButton", command=self.run_health).pack(side="left")
        tk.Label(bar, text="active tests", bg=VOID, fg=STEEL, font=self.fonts.small).pack(side="left", padx=(18, 4))
        self.test_btns = []
        for t, lbl in (("tone", "Speaker tone"), ("nfc", "NFC front-end"), ("lora", "LoRa RSSI probe"), ("gnss", "GNSS")):
            b = ttk.Button(bar, text=lbl, command=lambda t=t: self.run_test(t))
            b.pack(side="left", padx=2)
            self.test_btns.append(b)
        self.health_sum = tk.StringVar(value="")
        tk.Label(f, textvariable=self.health_sum, bg=VOID, fg=BONE, font=self.fonts.body, anchor="w").pack(anchor="w", pady=(0, 4))
        cols = ("result", "subsystem", "detail")
        self.health_tree = ttk.Treeview(f, columns=cols, show="headings", height=18)
        for c, w in zip(cols, (80, 130, 620)):
            self.health_tree.heading(c, text=c)
            self.health_tree.column(c, width=w, anchor="w")
        self.health_tree.tag_configure("pass", foreground=OK)
        self.health_tree.tag_configure("fail", foreground=BAD)
        self.health_tree.tag_configure("skip", foreground=STEEL)
        self.health_tree.tag_configure("need", foreground=GOLD)
        self.health_tree.pack(fill="both", expand=True, pady=4)
        tk.Label(f, text="Verdicts of the active tests print in Log. 'skip' = not probed from here (a lazy worker that isn't running, "
                         "or hardware with no standalone test). On a stock watch the ROM-level rows run now; the rest needs NocSif.",
                 bg=VOID, fg=STEEL, font=self.fonts.small, justify="left", wraplength=760).pack(anchor="w", pady=(0, 6))

    def run_health(self):
        if self.bridge:
            def work(b):
                return b.health()
            def done(res):
                final, checks = res
                self.health_tree.delete(*self.health_tree.get_children())
                for k in checks:
                    ok = k.get("ok")
                    tag = "pass" if ok is True else ("fail" if ok is False else "skip")
                    self.health_tree.insert("", "end", values=(tag.upper() if tag != "skip" else "skip", k["n"], k.get("d", "")), tags=(tag,))
                self.health_sum.set("pass %d  ·  fail %d  ·  not probed %d" % (final.get("pass", 0), final.get("fail", 0), final.get("skip", 0)))
            self.run_bridge(work, done, "running the hardware check")
        elif self.port and self.kind in ("stock", "blank", "silent"):
            self.health_tree.delete(*self.health_tree.get_children())
            self.health_sum.set("ROM-level checks (no NocSif on this watch)…")
            def work():
                return flasher.probe_rom(self.port, self._fl)
            def done(rom):
                self.health_tree.delete(*self.health_tree.get_children())
                if not rom:
                    self.health_sum.set("the chip did not answer the ROM loader — check the cable / try another port")
                    return
                self.health_tree.insert("", "end", values=("PASS", "soc", "%s · revision %s" % (rom.get("chip") or "ESP32-S3", rom.get("revision") or "?")), tags=("pass",))
                fs = rom.get("flash_size") or "?"
                self.health_tree.insert("", "end", values=("PASS" if "16" in fs else "FAIL", "flash", "%s · %s" % (fs, rom.get("flash_id") or "")), tags=("pass" if "16" in fs else "fail",))
                self.health_tree.insert("", "end", values=("PASS", "usb", "ROM loader answers on %s" % self.port), tags=("pass",))
                self.health_tree.insert("", "end", values=("PASS", "mac", rom.get("mac") or "?"), tags=("pass",))
                for n in ("i2c", "pmu", "display", "imu", "rtc", "sd", "audio", "mic", "gnss", "lora", "nfc", "wifi", "ble", "memory"):
                    self.health_tree.insert("", "end", values=("needs NocSif", n, "runs once NocSif (or the RAM diagnostic) is on the watch"), tags=("need",))
                self.health_sum.set("ROM-level checks done — the peripheral rows need NocSif on the watch")
            self.run_bg(work, done, "probing the chip")
        else:
            self.set_status("plug a watch in first")

    def run_test(self, t):
        self.run_bridge(lambda b: b.test(t), lambda r: self.set_status(r.get("msg", "started")), "test " + t)

    # ---- Flash page --------------------------------------------------------------------------------
    def _fbtn(self, parent, text, cmd, sd=False, style=None):
        """One flash-tab button; sd=True registers it as needing the NocSif bridge (greyed without it)."""
        b = ttk.Button(parent, text=text, command=cmd, **({"style": style} if style else {}))
        b.pack(fill="x", pady=2)
        if sd:
            self._sd_btns.append(b)
        return b

    def _refresh_flash_sd_enabled(self):
        """Grey the SD-dependent rows when there's no NocSif bridge (their SD half needs the watch running)."""
        on = self.bridge is not None
        for b in getattr(self, "_sd_btns", []):
            try:
                b.state(["!disabled"] if on else ["disabled"])
            except tk.TclError:
                pass
        if hasattr(self, "sd_note"):
            self.sd_note.configure(text="" if on else "SD backup / restore / format need NocSif running on the watch. "
                                                       "On other firmware, remove the microSD and copy it on your computer.")

    def _build_flash(self):
        f = self.pages["flash"]
        self._sd_btns = []
        top = tk.Frame(f, bg=VOID); top.pack(fill="x", pady=(6, 4))
        c1 = tk.Frame(top, bg=VOID); c1.pack(side="left", fill="y", padx=(0, 20))
        c2 = tk.Frame(top, bg=VOID); c2.pack(side="left", fill="y", padx=(0, 20))
        c3 = tk.Frame(top, bg=VOID); c3.pack(side="left", fill="both", expand=True)

        # --- Backup (📁 opens the backup folder) ---
        hb = tk.Frame(c1, bg=VOID); hb.pack(fill="x")
        ntheme.section(hb, "Backup", self.fonts).pack(side="left", anchor="w")
        fld = tk.Label(hb, text="📁", bg=VOID, fg=STEEL, font=self.fonts.body, cursor="hand2")
        fld.pack(side="right")
        fld.bind("<Button-1>", lambda e: self._open_folder(BACKUP_DIR))
        self._tooltip(fld, "Open backup folder")
        self._fbtn(c1, "Backup Flash + SD + NVS", self.backup_complete, sd=True, style="Accent.TButton")
        self._fbtn(c1, "Backup Flash", self.backup_watch)
        self._fbtn(c1, "Backup SD", self.backup_sd_only, sd=True)
        self._fbtn(c1, "Backup NVS", self.backup_nvs_only)

        # --- Restore ---
        ntheme.section(c1, "Restore", self.fonts).pack(anchor="w", pady=(12, 0))
        self._fbtn(c1, "Restore Flash + SD + NVS", self.restore_flash_sd, sd=True)
        self._fbtn(c1, "Restore Flash only", self.restore_flash_only)
        self._fbtn(c1, "Restore SD only", self.restore_sd_only, sd=True)
        self._fbtn(c1, "Restore NVS only", self.restore_nvs_only)

        # --- Erase ---
        ntheme.section(c2, "Erase", self.fonts).pack(anchor="w")
        self._fbtn(c2, "Erase Flash + SD + NVS", self.erase_flash_sd, sd=True, style="Bad.TButton")
        self._fbtn(c2, "Erase Flash only", self.erase_flash_only, style="Bad.TButton")
        self._fbtn(c2, "Erase SD only", self.erase_sd_only, sd=True, style="Warn.TButton")
        nv = tk.Frame(c2, bg=VOID); nv.pack(fill="x", pady=2)
        ttk.Button(nv, text="Erase NVS settings", command=self.erase_nvs, style="Warn.TButton").pack(side="left", fill="x", expand=True)
        info = tk.Label(nv, text="ⓘ", bg=VOID, fg=STEEL, font=self.fonts.body, cursor="question_arrow")
        info.pack(side="left", padx=(6, 0))
        self._tooltip(info, "NVS holds the watch's saved settings — device name, Wi-Fi credentials, paired-phone (BLE) "
                            "bonds, passcode/PIN, brightness/theme and other preferences. Erasing it resets the watch to "
                            "first-boot defaults but leaves the firmware installed and bootable.")

        # --- Flash (nothing here erases) ---
        ntheme.section(c2, "Flash", self.fonts).pack(anchor="w", pady=(12, 0))
        self._fbtn(c2, "Flash NocSif to Watch", self.flash_nocsif, style="Accent.TButton")
        self.lg_variant = tk.StringVar(value=self.settings.get("lilygo_variant", "sx1262"))
        vf = tk.Frame(c2, bg=VOID); vf.pack(fill="x", pady=(2, 0))
        tk.Label(vf, text="LilyGo radio", bg=VOID, fg=STEEL, font=self.fonts.small).pack(side="left")
        ttk.Combobox(vf, textvariable=self.lg_variant, values=list(updater.LILYGO_VARIANTS), width=8, state="readonly").pack(side="left", padx=6)
        self._fbtn(c2, "Flash LilyGo Firmware", self.flash_lilygo)
        self._fbtn(c2, "Set up folders", self.flash_provision, sd=True)

        # --- help + SD note ---
        tk.Label(c3, text="When asked, put the watch in DOWNLOAD mode: hold BOOT, briefly press RST, release BOOT (the "
                          "screen goes blank). Backup Flash / Restore Flash / Erase Flash work on ANY firmware (they read/write "
                          "the raw flash). Flash NocSif updates NocSif if it's already installed, otherwise installs it — "
                          "neither erases. A full-flash backup is one continuous read (~30-35 min).",
                 bg=VOID, fg=STEEL, font=self.fonts.small, justify="left", anchor="nw", wraplength=240).pack(anchor="nw", fill="x")
        self.sd_note = tk.Label(c3, text="", bg=VOID, fg=WARN, font=self.fonts.small, justify="left", anchor="nw", wraplength=240)
        self.sd_note.pack(anchor="nw", fill="x", pady=(10, 0))

        self.flash_prog = ttk.Progressbar(f, mode="determinate")
        self.flash_prog.pack(fill="x", pady=(8, 0))
        self.flash_out = scrolledtext.ScrolledText(f, height=12, bg=PIT, fg=BONE, insertbackground=BONE, font=self.fonts.small,
                                                   relief="flat", highlightthickness=1, highlightbackground=EDGE)
        self.flash_out.pack(fill="both", expand=True, pady=(6, 8))
        self._refresh_flash_sd_enabled()

    def _open_folder(self, path):
        os.makedirs(path, exist_ok=True)
        try:
            if sys.platform.startswith("win"):
                os.startfile(path)
            elif sys.platform == "darwin":
                os.system('open "%s"' % path)
            else:
                os.system('xdg-open "%s"' % path)
        except Exception:
            pass

    def _tooltip(self, widget, text):
        """Attach a simple hover tooltip to any widget (used by the 📁 and (i) affordances)."""
        state = {"win": None}
        def show(_e=None):
            if state["win"] or not text:
                return
            x = widget.winfo_rootx() + 18
            y = widget.winfo_rooty() + widget.winfo_height() + 4
            w = tk.Toplevel(widget); w.wm_overrideredirect(True); w.wm_geometry("+%d+%d" % (x, y))
            w.configure(bg=EDGE2)
            tk.Label(w, text=text, bg=PIT, fg=BONE, font=self.fonts.small, justify="left",
                     wraplength=340, padx=9, pady=7).pack(padx=1, pady=1)
            state["win"] = w
        def hide(_e=None):
            if state["win"]:
                state["win"].destroy(); state["win"] = None
        widget.bind("<Enter>", show); widget.bind("<Leave>", hide)

    # ---- SD-only backup (bridge; NocSif only) --------------------------------------------------
    def backup_sd_only(self):
        if self._busy_block("a backup"):
            return
        if not self.bridge:
            messagebox.showwarning(APP_NAME, "SD backup needs NocSif running on the watch.\n\nOn other firmware, remove the microSD and copy it on your computer.")
            return
        folder = self._backup_folder()
        self.show_page("flash"); self.flash_out.delete("1.0", "end"); self._prog_busy()
        b = self.bridge
        def scan():
            return b.walk("/sd", progress=lambda nd, nf: self.ui_q.put(lambda: self.set_status("scanning the card… %d folders, %d files" % (nd, nf))))
        def scanned(res):
            dirs, files = res
            total = sum(s for _, s in files)
            if not messagebox.askyesno(APP_NAME, "Back up the microSD (%d files, %s) into\n%s ?\n\nNothing is written to the watch."
                                       % (len(files), nbridge.human_size(total), folder)):
                self._prog_idle(); return
            self._backup_sd_run(folder, dirs, files, total)
        self._fl("== scanning the microSD (counting files — a full card can take a few minutes)… ==")
        self.run_bridge(lambda bb: scan(), scanned, "scanning the microSD")

    def _backup_sd_run(self, folder, dirs, files, total):
        self.busy = True
        b = self.bridge
        def work():
            os.makedirs(os.path.join(folder, "sd"), exist_ok=True)
            for d in dirs:
                os.makedirs(os.path.join(folder, "sd", d[len("/sd/"):].replace("/", os.sep)), exist_ok=True)
            nbytes = 0
            self._fl("== microSD backup: %d files, %s ==" % (len(files), nbridge.human_size(total)))
            for remote, size in files:
                local = os.path.join(folder, "sd", remote[len("/sd/"):].replace("/", os.sep))
                os.makedirs(os.path.dirname(local), exist_ok=True)
                b.get(remote, local)
                nbytes += size
                self._fl("  %s  (%s)" % (remote, nbridge.human_size(size)))
                self._prog_set(100.0 * nbytes / total if total else 100)
            meta = {"created": dt.datetime.now().isoformat(timespec="seconds"), "app": APP_VERSION, "kind": self.kind,
                    "nocsif": self.version, "ident": self.ident,
                    "sd": {"files": [{"path": r, "size": s} for r, s in files], "dirs": dirs, "bytes": total}}
            with open(os.path.join(folder, "backup.json"), "w", encoding="utf-8") as fh:
                json.dump(meta, fh, indent=2)
            return folder
        def done(f):
            self.busy = False
            self._fl("== microSD backup saved: %s ==" % f); self.set_status("SD backup saved to " + f)
        def fail(e):
            self.busy = False
        self.run_bg(work, done, "backing up the microSD", on_error=fail)

    # ---- explicit restore (Flash+SD / Flash / SD) ----------------------------------------------
    def _restore_pick(self):
        os.makedirs(BACKUP_DIR, exist_ok=True)
        return filedialog.askdirectory(title="Pick a backup folder", initialdir=BACKUP_DIR, mustexist=True) or None

    def restore_flash_sd(self):
        if self._busy_block("a restore"):
            return
        folder = self._restore_pick()
        if not folder:
            return
        flash = os.path.join(folder, "flash.bin")
        if not flasher.image_is_full_flash(flash):
            messagebox.showerror(APP_NAME, "No 16 MB flash.bin in that folder — pick a complete backup, or use Restore Flash only for a lone image."); return
        if not os.path.isdir(os.path.join(folder, "sd")):
            messagebox.showerror(APP_NAME, "No microSD backup (an sd/ folder) here — use Restore Flash only."); return
        if not self._confirm_full_image("flash.bin from " + os.path.basename(folder)):
            return
        self._flash_flow("full_image", image=flash, then=lambda: self._restore_sd(folder))

    def restore_flash_only(self):
        if self._busy_block("a restore"):
            return
        folder = self._restore_pick()
        if not folder:
            return
        cand = os.path.join(folder, "flash.bin")
        if not flasher.image_is_full_flash(cand):
            bins = [os.path.join(folder, n) for n in os.listdir(folder)
                    if n.lower().endswith(".bin") and flasher.image_is_full_flash(os.path.join(folder, n))]
            if len(bins) == 1:
                cand = bins[0]
            elif len(bins) > 1:
                messagebox.showerror(APP_NAME, "Several 16 MB images in that folder — open the specific backup's folder."); return
            else:
                messagebox.showerror(APP_NAME, "No 16 MB flash image (flash.bin) in that folder."); return
        if not self._confirm_full_image(os.path.basename(cand)):
            return
        self._flash_flow("full_image", image=cand)

    def restore_sd_only(self):
        if self._busy_block("a restore"):
            return
        if not self.bridge:
            messagebox.showwarning(APP_NAME, "SD restore needs NocSif running on the watch.\n\nOn other firmware, copy the files onto the card yourself."); return
        folder = self._restore_pick()
        if not folder:
            return
        if not os.path.isdir(os.path.join(folder, "sd")):
            messagebox.showerror(APP_NAME, "No microSD backup (an sd/ folder) in that folder."); return
        if not messagebox.askyesno(APP_NAME, "Put the backed-up microSD files back on the card? Existing files with the same names are overwritten; others are left alone."):
            return
        self.show_page("flash"); self.flash_out.delete("1.0", "end")
        self._restore_sd(folder)

    # ---- NVS-only backup / restore (esptool; any firmware) -------------------------------------
    def backup_nvs_only(self):
        dest = os.path.join(self._backup_folder(), "nvs.bin")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if not messagebox.askyesno(APP_NAME, "Back up JUST the NVS settings (device name, Wi-Fi credentials, phone bonds, passcode, prefs) into\n%s ?\n\nNothing is written to the watch — a few seconds."
                                   % dest):
            return
        self._erase_flow("backing up NVS",
                         lambda p: (0 if (flasher.read_nvs(p, dest, self._fl) == 0 and os.path.exists(dest)
                                          and os.path.getsize(dest) == flasher.NVS_SIZE) else 1),
                         reconnect=True, ok_status="NVS backup saved to " + dest)

    def restore_nvs_only(self):
        folder = self._restore_pick()
        if not folder:
            return
        nvs = os.path.join(folder, "nvs.bin")
        if not (os.path.isfile(nvs) and os.path.getsize(nvs) == flasher.NVS_SIZE):
            messagebox.showerror(APP_NAME, "No nvs.bin (a %d-byte NVS backup) in that folder." % flasher.NVS_SIZE); return
        if not messagebox.askyesno(APP_NAME, "Restore the NVS settings from\n%s ?\n\nThis overwrites the watch's current settings, Wi-Fi credentials, phone bonds and passcode with the backed-up ones. The firmware is not touched."
                                   % nvs):
            return
        self._erase_flow("restoring NVS",
                         lambda p: flasher.write_image_at(p, flasher.NVS_OFFSET, nvs, self._fl),
                         reconnect=True, ok_status="NVS restored from " + nvs)

    # ---- erase (Flash+SD / Flash / SD / NVS) ---------------------------------------------------
    def _erase_flow(self, label, work_fn, reconnect, ok_status=None):
        """Shared single esptool op (erase / nvs read / nvs write): drop the bridge, prompt download mode,
        run work_fn(port)->rc on a worker, then reconnect (the firmware still boots) or show the
        blank-board notice (a whole-flash erase, reconnect=False)."""
        port = self._current_port()
        if not port:
            messagebox.showerror(APP_NAME, "No port — plug the watch in first."); return
        if self._busy_block(label):
            return
        self.disconnect(keep_kind=True)
        if not self._download_mode_prompt():
            self._show_watch_page(); return
        self.show_page("flash"); self.flash_out.delete("1.0", "end"); self._prog_busy()
        self.busy = True
        def work():
            rc = work_fn(port)
            if rc != 0:
                raise RuntimeError("%s failed (esptool exited %d) — see the log above; re-enter download mode (hold BOOT, tap RST) and try again." % (label, rc))
            time.sleep(2.0)
            return port
        def done(_p):
            self.busy = False; self._prog_set(100)
            self._failed_ports.pop(port, None); self._probed_ports.pop(port, None)
            if ok_status:
                self.set_status(ok_status)
            if reconnect:
                self._post_flash_notice()
                self.connect(port, auto=True)
            else:
                messagebox.showinfo(APP_NAME, "Flash erased — the watch is now BLANK and will not boot until you use Flash NocSif to Watch.\n\nPress RST if the screen isn't already blank.")
                self.kind = "blank"; self._show_watch_page()
        def fail(e):
            self.busy = False; self._show_watch_page()
        self.run_bg(work, done, label, on_error=fail)

    def erase_flash_only(self):
        if not messagebox.askyesno(APP_NAME, "Erase the ENTIRE flash?\n\nThis wipes the firmware AND all settings, Wi-Fi credentials, phone bonds and the passcode.\n\n⚠ It leaves the watch BLANK and non-booting until you Flash NocSif to Watch afterwards.", icon="warning"):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — erase the whole flash and leave a blank board?", icon="warning"):
            return
        self._erase_flow("erasing the flash", lambda p: flasher.erase_flash(p, self._fl), reconnect=False)

    def erase_nvs(self):
        if not messagebox.askyesno(APP_NAME, "Erase the NVS settings?\n\nResets the watch to first-boot defaults — device name, Wi-Fi credentials, paired-phone (BLE) bonds, passcode/PIN, brightness/theme and other preferences are cleared. The firmware stays installed and the watch still boots.", icon="warning"):
            return
        self._erase_flow("erasing NVS settings", lambda p: flasher.erase_nvs(p, self._fl), reconnect=True)

    def erase_sd_only(self):
        if self._busy_block("an SD format"):
            return
        if not self.bridge:
            messagebox.showwarning(APP_NAME, "Formatting the microSD needs NocSif running on the watch."); return
        if not messagebox.askyesno(APP_NAME, "Format the microSD card?\n\nEVERYTHING on the card is erased (captures, audio, notes, voice memos, tracks, macros, the firmware image). The card is reformatted as FAT and the NocSif folders are recreated.", icon="warning"):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — erase the whole card now?", icon="warning"):
            return
        self.show_page("flash"); self.flash_out.delete("1.0", "end")
        def work(b):
            self._fl("== formatting the microSD ==")
            return b.sd_format()
        def done(_r):
            self._fl("== card formatted =="); self.set_status("microSD formatted"); self.files_load(); self._prog_set(100)
        self.run_bridge(work, done, "formatting the microSD")

    def erase_flash_sd(self):
        port = self._current_port()
        if not port:
            messagebox.showerror(APP_NAME, "No port — plug the watch in first."); return
        if self._busy_block("an erase"):
            return
        if not self.bridge:
            if messagebox.askyesno(APP_NAME, "No NocSif on this watch, so the card can't be formatted from here — erase the FLASH only?\n\n(To wipe the card, remove it and format it on a computer.)", icon="warning"):
                self.erase_flash_only()
            return
        if not messagebox.askyesno(APP_NAME, "Erase the flash, the microSD AND the NVS settings?\n\nThe card is formatted first (everything on it erased), then the whole flash is erased — which includes the NVS settings (device name, Wi-Fi credentials, phone bonds, passcode). Nothing is left.\n\n⚠ The watch is left BLANK and non-booting until you Flash NocSif to Watch afterwards.", icon="warning"):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — format the card and erase the whole flash?", icon="warning"):
            return
        self.show_page("flash"); self.flash_out.delete("1.0", "end"); self._prog_busy()
        b = self.bridge
        self.busy = True
        def work():
            self._fl("== formatting the microSD first (while NocSif still runs) ==")
            b.sd_format()
            self._fl("== card formatted ==")
            self.ui_q.put(lambda: self.disconnect(keep_kind=True))
            time.sleep(1.0)
            if not self._ask_on_main(self._download_mode_prompt):
                raise RuntimeError("flash erase cancelled — the card was already formatted")
            self._fl("== erasing the ENTIRE flash ==")
            rc = flasher.erase_flash(port, self._fl)
            if rc != 0:
                raise RuntimeError("flash erase failed (esptool exited %d) — the card was formatted; re-enter download mode and retry the flash erase" % rc)
            time.sleep(2.0)
            return port
        def done(_p):
            self.busy = False; self._prog_set(100)
            self._failed_ports.pop(port, None); self._probed_ports.pop(port, None)
            messagebox.showinfo(APP_NAME, "Flash and microSD erased — the watch is now BLANK and won't boot until you Flash NocSif.\n\nPress RST if the screen isn't already blank.")
            self.kind = "blank"; self._show_watch_page()
        def fail(e):
            self.busy = False; self._show_watch_page()
        self.run_bg(work, done, "erasing flash + microSD", on_error=fail)

    def _fl(self, line):
        self.ui_q.put(lambda: (self.flash_out.insert("end", line + "\n"), self.flash_out.see("end")))
        m = None
        # esptool write-flash prints "… (NN %)"; read-flash (the backup) prints "… NN.N% DONE/TOTAL
        # bytes". Parse both so the bar tracks a long (~30-min) backup read, not just writes.
        if "%" in line and ("Writing" in line or "Reading" in line or "bytes" in line):
            try:
                m = int(line.rsplit("(", 1)[-1].split("%")[0].strip())     # "(NN %)" — writes
            except ValueError:
                m = None
            if m is None:
                mm = re.search(r"(\d+)\s*/\s*(\d+)\s*bytes", line)          # "DONE/TOTAL bytes" — reads
                if mm and int(mm.group(2)):
                    m = int(100 * int(mm.group(1)) / int(mm.group(2)))
        if m is not None:
            self._prog_set(m)

    # ---- Flash-tab progress bar: marquee "working…" until a real % arrives, then determinate --------
    def _prog_busy(self):
        """Start the indeterminate 'working…' marquee — a phase with no % yet (SD scan, esptool connect,
        an erase). The first determinate update (_prog_set) stops it automatically."""
        def a():
            self._prog_ind = True
            self.flash_prog.configure(mode="indeterminate"); self.flash_prog.start(14)
        self.ui_q.put(a)

    def _prog_set(self, value):
        """Determinate progress 0-100; cancels the marquee on the first real value."""
        def a():
            if getattr(self, "_prog_ind", False):
                self.flash_prog.stop(); self.flash_prog.configure(mode="determinate"); self._prog_ind = False
            self.flash_prog.configure(value=max(0, min(100, value)))
        self.ui_q.put(a)

    def _prog_idle(self):
        """Stop the marquee and reset to an empty bar — a cancelled or failed op."""
        def a():
            if getattr(self, "_prog_ind", False):
                self.flash_prog.stop(); self._prog_ind = False
            self.flash_prog.configure(mode="determinate", value=0)
        self.ui_q.put(a)

    def _release_dir(self, version):
        return os.path.join(CACHE_DIR, version.replace("/", "_"))

    def _fetch_release(self, want_parts):
        m = self.manifest or updater.fetch_manifest(updater.DEFAULT_REPO)
        self.manifest = m
        dest = self._release_dir(m.get("version", "unknown"))
        self._fl("fetching NocSif %s from %s" % (m.get("version"), updater.DEFAULT_REPO))
        files = updater.fetch_release(updater.DEFAULT_REPO, m, dest, want_parts=want_parts,
                                      progress=lambda n, d, t: self._prog_set((100 * d / t) if t else 0))
        for name, info in files.items():
            self._fl("  %s -> 0x%x (%s)" % (name, info["offset"], nbridge.human_size(os.path.getsize(info["path"]))))
        return files

    def _current_port(self):
        return self.port or (self.port_var.get().split(" ")[0] if self.port_var.get() else None)

    def _download_mode_prompt(self):
        """Reliable-flash guard: ask the user to put the watch in DOWNLOAD (boot) mode before esptool
        connects. Auto-reset over the native USB-Serial/JTAG is unreliable on this board (and stock
        firmware can hold the port), so entering the ROM loader by hand is the path that always works.
        Returns True to proceed; shown after the bridge is dropped (port free). Main (Tk) thread only."""
        return messagebox.askokcancel(
            APP_NAME,
            "Put the watch in DOWNLOAD (boot) mode first, or flashing will error "
            "(“could not open port” / “no serial data received”):\n\n"
            "  1.  Hold the BOOT button.\n"
            "  2.  While holding BOOT, briefly press RST.\n"
            "  3.  Release BOOT.\n\n"
            "The screen goes blank — that is download mode. Then click OK to flash.\n\n"
            "If a flash still errors, repeat these three steps and try again.",
            icon="info")

    def _ask_on_main(self, fn):
        """Run a modal dialog fn() on the Tk main thread from a worker thread and return its result."""
        box, ev = {}, threading.Event()
        def run():
            try:
                box["v"] = fn()
            finally:
                ev.set()
        self.ui_q.put(run)
        ev.wait()
        return box.get("v")

    def _flash_flow(self, mode, local_bin=None, image=None, backup_first=None, then=None):
        """Runs a flashing operation on a background worker thread. mode is one of: update | new |
        wipe_keep | wipe_full | local | full_image. The bridge connection is closed first since esptool
        needs exclusive use of the port. backup_first, if given a path, writes a full flash backup there
        before anything gets erased (used by the stock-watch flows). then, if given a callable, runs
        once the watch reconnects and the bridge answers again (used for the microSD half of a
        complete restore)."""
        port = self._current_port()
        if not port:
            messagebox.showerror(APP_NAME, "No port — plug the watch in first."); return
        if self._busy_block("a flash"):
            return
        was_kind = self.kind
        self.disconnect(keep_kind=True)
        if not self._download_mode_prompt():
            self.kind = was_kind
            self._show_watch_page()
            return
        self.show_page("flash")
        self.flash_out.delete("1.0", "end")
        self._prog_busy()
        self.busy = True

        def work():
            if backup_first:
                self._fl("== backing up the whole flash to %s ==" % backup_first)
                if flasher.backup_full(port, backup_first, self._fl, progress=lambda d, t: self._prog_set(100 * d / t)) != 0 \
                        or not flasher.image_is_full_flash(backup_first):
                    raise RuntimeError("the pre-flash backup could not be read completely over USB (a region failed — see the flash log for the offset). NOTHING was changed on the watch. Fix or skip the backup, then flash again.")
                self._write_backup_meta(backup_first)
                self._fl("== backup verified (%s) ==" % nbridge.human_size(os.path.getsize(backup_first)))
            if mode == "full_image":
                self._fl("== writing the 16 MB image %s at 0x0 (through the ROM loader, ~4-5 min) ==" % os.path.basename(image))
                rc = flasher.write_image_at(port, 0, image, self._fl)
            elif mode == "local":
                files = self._fetch_release(want_parts=True)
                self._fl("flashing local image %s" % local_bin)
                rc = flasher.flash_app(port, local_bin, files["ota_data_initial.bin"]["path"], self._fl)
            elif mode == "update":
                files = self._fetch_release(want_parts=True)
                rc = flasher.flash_app(port, files["firmware.bin"]["path"], files["ota_data_initial.bin"]["path"], self._fl)
            else:   # mode == "new": full provision (bootloader + table + otadata + app), NO erase
                files = self._fetch_release(want_parts=True)
                parts = [(i["offset"], i["path"]) for i in files.values()]
                need = {"bootloader.bin", "partitions.bin", "ota_data_initial.bin", "firmware.bin"}
                if need - set(files):
                    raise RuntimeError("the published release lacks %s — full provisioning needs every part" % ", ".join(sorted(need - set(files))))
                self._fl("== writing %s (no erase; a fresh otadata boots the new app) ==" % flasher.describe_parts(parts))
                rc = flasher.flash_parts(port, parts, self._fl)
            if rc != 0:
                raise RuntimeError("esptool exited with code %d. Check the flash log above for the reason; if it failed to connect, re-enter download mode (hold BOOT, tap RST) and try again." % rc)
            self._fl("== done; waiting for the watch to boot ==")
            time.sleep(4.0)
            return mode

        def done(mode):
            self.busy = False
            self._prog_set(100)
            self._failed_ports.pop(port, None)
            self._probed_ports.pop(port, None)
            self._post_flash_notice()
            self.connect(port, auto=True, after_flash=then or mode)
        def fail(e):
            self.busy = False
            self.kind = was_kind
            self._show_watch_page()
        self.run_bg(work, done, "flashing", on_error=fail)

    # ---- complete backup/restore, covering both the flash and the microSD -------------------------
    def _backup_folder(self):
        os.makedirs(BACKUP_DIR, exist_ok=True)
        mac = ((self.ident or {}).get("rom") or {}).get("mac") or (self.version or {}).get("mac") or "watch"
        return os.path.join(BACKUP_DIR, "twatch-ultra_%s_%s" % (mac.replace(":", ""), dt.datetime.now().strftime("%Y%m%d-%H%M%S")))

    def backup_complete(self):
        """Backs up everything: copies every file on the microSD (over the bridge, ~150 KB/s) into
        <folder>/sd, then dumps the whole 16 MB flash into <folder>/flash.bin, and writes backup.json
        alongside. Without a bridge connection (a stock watch) only the flash gets backed up, since the
        card can't be read that way."""
        port = self._current_port()
        if not port:
            messagebox.showerror(APP_NAME, "No port — plug the watch in first."); return
        if self._busy_block("a backup"):
            return
        if not self.bridge:
            if messagebox.askyesno(APP_NAME, "No NocSif bridge on this watch, so the microSD can't be read from here — back up the flash only?"):
                self.backup_watch()
            return
        folder = self._backup_folder()
        self.show_page("flash")
        self.flash_out.delete("1.0", "end")
        self._prog_busy()
        b = self.bridge
        def scan():
            return b.walk("/sd", progress=lambda nd, nf: self.ui_q.put(lambda: self.set_status("scanning the card… %d folders, %d files" % (nd, nf))))
        def scanned(res):
            dirs, files = res
            total = sum(s for _, s in files)
            eta = total / 150000.0 + 2100
            if not messagebox.askyesno(APP_NAME, "Back up the watch completely into\n%s ?\n\n  microSD: %d files, %s (over the bridge, about %d s)\n  flash: 16 MB (about 30-35 min — one reliable continuous read)\n\nNothing is written to the watch."
                                       % (folder, len(files), nbridge.human_size(total), int(total / 150000.0) + 5)):
                self._prog_idle(); return
            self._backup_complete_run(port, folder, dirs, files, total)
        self._fl("== scanning the microSD (counting files — a full card can take a few minutes)… ==")
        self.run_bridge(lambda bb: scan(), scanned, "scanning the microSD")

    def _backup_complete_run(self, port, folder, dirs, files, total):
        self.busy = True
        b = self.bridge
        def work():
            os.makedirs(os.path.join(folder, "sd"), exist_ok=True)
            done = 0
            self._fl("== microSD: %d files, %s ==" % (len(files), nbridge.human_size(total)))
            for d in dirs:
                os.makedirs(os.path.join(folder, "sd", d[len("/sd/"):].replace("/", os.sep)), exist_ok=True)
            for remote, size in files:
                local = os.path.join(folder, "sd", remote[len("/sd/"):].replace("/", os.sep))
                os.makedirs(os.path.dirname(local), exist_ok=True)
                b.get(remote, local)
                done += size
                self._fl("  %s  (%s)" % (remote, nbridge.human_size(size)))
                self._prog_set(100.0 * done / total if total else 100)
            meta = {"created": dt.datetime.now().isoformat(timespec="seconds"), "app": APP_VERSION, "kind": self.kind,
                    "nocsif": self.version, "ident": self.ident, "port": port,
                    "sd": {"files": [{"path": r, "size": s} for r, s in files], "dirs": dirs, "bytes": total},
                    "flash": {"file": "flash.bin", "size": flasher.FLASH_TOTAL},
                    "nvs": {"file": "nvs.bin", "size": flasher.NVS_SIZE}}
            with open(os.path.join(folder, "backup.json"), "w", encoding="utf-8") as fh:
                json.dump(meta, fh, indent=2)
            # dumping the flash needs exclusive access to the port, so the bridge is closed first and the app reconnects after
            self.ui_q.put(lambda: self.disconnect(keep_kind=True))
            time.sleep(1.0)
            if not self._ask_on_main(self._download_mode_prompt):
                self._fl("== flash dump skipped (download mode declined) — the microSD backup is saved ==")
                self.ui_q.put(lambda: self.connect(port, auto=True))
                return folder
            self._fl("== flash: the whole 16 MB (one continuous read over USB, ~30-35 min) ==")
            dest = os.path.join(folder, "flash.bin")
            rc = flasher.backup_full(port, dest, self._fl, progress=lambda d, t: self._prog_set(100 * d / t))
            if rc != 0 or not flasher.image_is_full_flash(dest):
                raise RuntimeError("the flash could not be read completely over USB — a region failed (see the flash log for the exact offset). The microSD part of the backup IS saved; the flash image is not.")
            # NVS lives INSIDE the flash image (0x9000) — carve a standalone nvs.bin out of it (no extra
            # device read) so "Restore NVS only" can put just the settings back later.
            try:
                with open(dest, "rb") as fh:
                    fh.seek(flasher.NVS_OFFSET); nvs = fh.read(flasher.NVS_SIZE)
                if len(nvs) == flasher.NVS_SIZE:
                    with open(os.path.join(folder, "nvs.bin"), "wb") as nf:
                        nf.write(nvs)
                    self._fl("== nvs.bin (settings) carved from the flash image ==")
            except OSError:
                pass
            time.sleep(2.0)
            return folder
        def done(f):
            self.busy = False
            self._fl("== complete backup saved: %s ==" % f)
            self.set_status("backup saved to " + f)
            self.connect(port, auto=True)
        def fail(e):
            self.busy = False
            self.connect(port, auto=True)
        self.run_bg(work, done, "backing up the watch", on_error=fail)

    def _restore_sd(self, folder):
        """Restores a backed-up microSD folder back onto the card: recreates the directory tree, then uploads every file."""
        if not self.bridge:
            self.set_status("the bridge is not back yet — restore the microSD from Flash › Restore once it is")
            return
        src = os.path.join(folder, "sd")
        b = self.bridge
        def work():
            files = []
            for root, _, names in os.walk(src):
                rel = os.path.relpath(root, src).replace(os.sep, "/")
                remote_dir = "/sd" if rel == "." else "/sd/" + rel
                files += [(os.path.join(root, n), remote_dir + "/" + n, remote_dir) for n in names]
            total = sum(os.path.getsize(l) for l, _, _ in files) or 1
            self._fl("== microSD restore: %d files, %s ==" % (len(files), nbridge.human_size(total)))
            made = set()
            done = 0
            for local, remote, rdir in files:
                parts = rdir.split("/")
                for i in range(3, len(parts) + 1):           # builds each ancestor dir in turn, e.g. /sd/a/b needs /sd/a then /sd/a/b
                    d = "/".join(parts[:i])
                    if d not in made:
                        made.add(d)
                        try:
                            b.mkdir(d)
                        except nbridge.BridgeError:
                            pass
                b.put(local, remote)
                done += os.path.getsize(local)
                self._fl("  %s" % remote)
                self._prog_set(100.0 * done / total)
            return len(files)
        self.run_bg(work, lambda n: (self.set_status("microSD restored (%d files)" % n), self.files_load(), self._prog_set(100)), "restoring the microSD")

    def restore_complete(self, folder):
        """Restores from a complete-backup folder: asks the user which pieces to restore, writes the
        flash first (which reboots the watch and brings the bridge back up), then restores the card."""
        has_flash = flasher.image_is_full_flash(os.path.join(folder, "flash.bin"))
        has_sd = os.path.isdir(os.path.join(folder, "sd"))
        want = restore_prompt(self, has_flash, has_sd)
        if not want:
            return
        do_flash, do_sd = want
        if do_flash:
            if not self._confirm_full_image("flash.bin from " + os.path.basename(folder)):
                return
            self._flash_flow("full_image", image=os.path.join(folder, "flash.bin"), then=(lambda: self._restore_sd(folder)) if do_sd else None)
        elif do_sd:
            if not messagebox.askyesno(APP_NAME, "Put the backed-up microSD files back onto the card? Existing files with the same names are overwritten; others are left alone."):
                return
            self.show_page("flash")
            self.flash_out.delete("1.0", "end")
            self._restore_sd(folder)

    def _after_flash(self, mode):
        if callable(mode):                              # mode can also be a follow-up step, e.g. the microSD half of a restore
            self.after(1200, mode)
        elif mode in ("new", "wipe_full", "wipe_keep"):
            self.after(1500, self._post_provision_check)
        elif mode in ("update", "local"):
            self.after(1000, self.load_overview)

    def flash_local(self):
        path = filedialog.askopenfilename(title="firmware.bin (NocSif app image)", filetypes=[("ESP app image", "*.bin")])
        if not path:
            return
        if not messagebox.askyesno(APP_NAME, "Flash %s to the app slot (0x20000) + reset otadata?\nSettings are kept." % os.path.basename(path)):
            return
        self._flash_flow("local", local_bin=path)

    def flash_local_full(self):
        path = filedialog.askopenfilename(title="16 MB flash image (written at 0x0)", filetypes=[("flash image", "*.bin")])
        if not path:
            return
        if not flasher.image_is_full_flash(path):
            messagebox.showerror(APP_NAME, "That file is not a 16 MB flash image."); return
        if not self._confirm_full_image(os.path.basename(path)):
            return
        self._flash_flow("full_image", image=path, backup_first=self._backup_offer())

    def _confirm_full_image(self, name):
        return (messagebox.askyesno(APP_NAME, "Write %s over the ENTIRE flash (0x0 – 16 MB)?\n\nEverything on the watch is replaced: firmware, "
                                              "settings, credentials, bonds, the internal file store." % name, icon="warning")
                and messagebox.askyesno(APP_NAME, "Second confirmation — replace the whole flash now?", icon="warning"))

    def _backup_offer(self):
        """Prompts to back the flash up before a destructive operation; returns the chosen backup path, or None if declined."""
        if messagebox.askyesno(APP_NAME, "Back up the whole flash first (16 MB, about 30-35 min — one reliable continuous read)?\n\nA backup restores the watch exactly as it is now — recommended before the first NocSif flash on a stock watch."):
            return self._new_backup_path()
        return None

    def _new_backup_path(self):
        os.makedirs(BACKUP_DIR, exist_ok=True)
        mac = ((self.ident or {}).get("rom") or {}).get("mac") or (self.version or {}).get("mac") or "watch"
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        return os.path.join(BACKUP_DIR, "twatch-ultra_%s_%s.bin" % (mac.replace(":", ""), stamp))

    def _write_backup_meta(self, path):
        meta = {"created": dt.datetime.now().isoformat(timespec="seconds"), "kind": self.kind, "port": self.port,
                "ident": self.ident, "nocsif": self.version, "app": APP_VERSION}
        try:
            with open(path[:-4] + ".json", "w", encoding="utf-8") as fh:
                json.dump(meta, fh, indent=2)
        except Exception:
            pass

    def backup_watch(self):
        port = self._current_port()
        if not port:
            messagebox.showerror(APP_NAME, "No port — plug the watch in first."); return
        if self._busy_block("a backup"):
            return
        dest = self._new_backup_path()
        if not messagebox.askyesno(APP_NAME, "Read the whole 16 MB flash into\n%s ?\n\nNothing is written to the watch. Takes about 30-35 minutes — one reliable continuous read over USB." % dest):
            return
        self.disconnect(keep_kind=True)
        if not self._download_mode_prompt():
            self._show_watch_page()
            return
        self.show_page("flash")
        self.flash_out.delete("1.0", "end")
        self.busy = True
        def work():
            self._fl("== backing up the whole flash (one continuous read over USB, ~30-35 min) ==")
            rc = flasher.backup_full(port, dest, self._fl, progress=lambda d, t: self._prog_set(100 * d / t))
            if rc != 0 or not flasher.image_is_full_flash(dest):
                raise RuntimeError("the flash could not be read completely over USB — a region failed (see the flash log for the exact offset). If it keeps failing at the same offset, that flash region may be unreadable on this unit.")
            self._write_backup_meta(dest)
            time.sleep(2.0)
            return dest
        def done(d):
            self.busy = False
            self._fl("== backup saved: %s ==" % d)
            self.set_status("backup saved to " + d)
            self.connect(port, auto=True)
        def fail(e):
            self.busy = False
        self.run_bg(work, done, "backing up", on_error=fail)

    def restore_backup(self):
        """Pick a backup FOLDER and restore it: a complete backup (backup.json + flash.bin + sd/) runs the
        flash/microSD chooser; a folder holding just a 16 MB flash.bin (or a lone 16 MB .bin) restores the
        flash. A single flash-only .bin image is restored with "Flash a local 16 MB image…" instead."""
        os.makedirs(BACKUP_DIR, exist_ok=True)
        folder = filedialog.askdirectory(title="Pick a backup folder to restore", initialdir=BACKUP_DIR, mustexist=True)
        if not folder:
            return
        if os.path.isfile(os.path.join(folder, "backup.json")):
            self.restore_complete(folder)
            return
        # No manifest — accept a folder that holds a single 16 MB flash image (flash.bin, or one .bin).
        cand = os.path.join(folder, "flash.bin")
        if not flasher.image_is_full_flash(cand):
            bins = [os.path.join(folder, n) for n in os.listdir(folder)
                    if n.lower().endswith(".bin") and flasher.image_is_full_flash(os.path.join(folder, n))]
            if len(bins) == 1:
                cand = bins[0]
            elif len(bins) > 1:
                messagebox.showerror(APP_NAME, "That folder holds several 16 MB images — open the one backup's folder, "
                                               "or use “Flash a local 16 MB image…”."); return
            else:
                messagebox.showerror(APP_NAME, "No NocSif backup in that folder (expected backup.json or a 16 MB flash.bin).\n\n"
                                               "To restore a single flash .bin image, use “Flash a local 16 MB image…”."); return
        if not self._confirm_full_image(os.path.basename(cand)):
            return
        self._flash_flow("full_image", image=cand)

    def flash_lilygo(self):
        if self._busy_block("LilyGo firmware"):
            return
        variant = self.lg_variant.get()
        self.settings["lilygo_variant"] = variant
        save_settings(self.settings)
        if not messagebox.askyesno(APP_NAME, "Fetch LilyGo's latest factory image for the %s radio variant and write it over the ENTIRE flash?\n\n"
                                             "This puts the watch back to the way LilyGo ships it. Everything NocSif (settings, credentials, bonds) is gone." % variant.upper(), icon="warning"):
            return
        backup = self._backup_offer()
        port = self._current_port()
        if not port:
            messagebox.showerror(APP_NAME, "No port — plug the watch in first."); return
        self.disconnect(keep_kind=True)
        if not self._download_mode_prompt():
            self._show_watch_page()
            return
        self.show_page("flash")
        self.flash_out.delete("1.0", "end")
        self.busy = True
        def work():
            imgs = updater.lilygo_factory_images()
            if variant not in imgs:
                raise RuntimeError("no factory image for %s in LilyGoLib/firmware right now" % variant)
            img = imgs[variant]
            os.makedirs(LILYGO_DIR, exist_ok=True)
            dest = os.path.join(LILYGO_DIR, img["name"])
            if not (os.path.isfile(dest) and os.path.getsize(dest) == img["size"]):
                self._fl("fetching %s (%s) from LilyGo" % (img["name"], nbridge.human_size(img["size"])))
                updater.download_url(img["url"], dest, img["size"], progress=lambda d, t: self._prog_set(100 * d / t if t else 0))
            else:
                self._fl("using cached %s" % img["name"])
            if backup:
                self._fl("== backing up the whole flash to %s ==" % backup)
                if flasher.backup_full(port, backup, self._fl, progress=lambda d, t: self._prog_set(100 * d / t)) != 0 \
                        or not flasher.image_is_full_flash(backup):
                    raise RuntimeError("the pre-flash backup could not be read completely over USB (a region failed — see the flash log for the offset). NOTHING was changed on the watch. Fix or skip the backup, then flash again.")
                self._write_backup_meta(backup)
            self._fl("== writing %s at 0x0 (16 MB through the ROM loader, ~4-5 min) ==" % img["name"])
            rc = flasher.write_image_at(port, 0, dest, self._fl)
            if rc != 0:
                raise RuntimeError("esptool exited with code %d writing the LilyGo image. Check the flash log; if it failed to connect, re-enter download mode (hold BOOT, tap RST) and try again." % rc)
            self._fl("== done; the watch boots LilyGo's firmware ==")
            time.sleep(4.0)
            return "full_image"
        def done(mode):
            self.busy = False
            self._prog_set(100)
            self._failed_ports.pop(port, None)
            self._probed_ports.pop(port, None)
            self._post_flash_notice()
            self.connect(port, auto=True)
        def fail(e):
            self.busy = False
        self.run_bg(work, done, "flashing LilyGo firmware", on_error=fail)

    def flash_new_watch(self):
        if self._busy_block("a flash"):
            return
        stock = self.kind in ("stock", "blank")
        msg = ("Flash NocSif writes the COMPLETE published NocSif firmware:\n\n"
               "  0x0      bootloader.bin\n  0x8000   partitions.bin\n  0xf000   ota_data_initial.bin\n  0x20000  firmware.bin\n\n"
               "Nothing is erased — the images overwrite whatever is there and a fresh otadata boots the new app.\n%s"
               "Afterwards the app reconnects, checks the microSD and offers to set up the NocSif folders.\n\nContinue?"
               % ("For a guaranteed-clean start on a used/stock watch, use Erase Flash first (take the backup when offered).\n\n" if stock else "\n"))
        if not messagebox.askyesno(APP_NAME, msg):
            return
        backup = self._backup_offer() if stock else None
        self._flash_flow("new", backup_first=backup)

    def flash_nocsif(self):
        """The one 'get NocSif on the watch' button: if it's already running NocSif, update the app slot
        (settings kept); otherwise flash the full NocSif image. Neither path erases."""
        if self._busy_block("a flash"):
            return
        if self.kind == "nocsif":
            self.update_watch()
        else:
            self.flash_new_watch()

    def update_watch(self):
        if self._busy_block("an update"):
            return
        if not self.manifest:
            def done(m):
                self.manifest = m
                self.update_watch()
            self.run_bg(lambda: updater.fetch_manifest(updater.DEFAULT_REPO), done, "fetching manifest.json")
            return
        if not messagebox.askyesno(APP_NAME, "Flash the published NocSif app %s to the watch over USB?\n\nSettings, credentials and bonds are kept (only the app slot and otadata are written)." % self.manifest.get("version")):
            return
        self._flash_flow("update")

    def _post_provision_check(self):
        if not self.bridge:
            self.set_status("could not reconnect after flashing — unplug / replug and it will be picked up")
            return
        def work(b):
            return b.sd_info()
        def done(i):
            if i.get("present"):
                if messagebox.askyesno(APP_NAME, "microSD found (%s free of %s).\n\nSet up the NocSif folders now (firmware, audio, wifi, ble, notes, voice, tracks, wardrive)? Only missing ones are created."
                                       % (nbridge.human_size(i.get("free")), nbridge.human_size(i.get("total")))):
                    self.provision_sd()
            else:
                messagebox.showwarning(APP_NAME, "No microSD card in the watch.\n\nFiles, captures, audio, voice memos, notes, tracks and Update need one — insert a card, then use Files › Set up folders (or Format SD) later. The watch works without it otherwise.")
            self.load_overview()
        self.run_bridge(work, done, "checking the microSD")

    # ---- Files page --------------------------------------------------------------------------------
    def _build_files(self):
        f = self.pages["files"]
        bar = tk.Frame(f, bg=VOID); bar.pack(fill="x", pady=(6, 4))
        ttk.Button(bar, text="Back", command=self.files_up).pack(side="left")
        self.path_var = tk.StringVar(value="/sd")
        ttk.Entry(bar, textvariable=self.path_var, width=46, font=self.fonts.body).pack(side="left", padx=6)
        ttk.Button(bar, text="Go", command=self.files_load).pack(side="left")
        ttk.Button(bar, text="Refresh", command=self.files_load).pack(side="left", padx=(6, 0))
        cols = ("name", "size")
        self.files_tree = ttk.Treeview(f, columns=cols, show="headings", height=16)
        self.files_tree.heading("name", text="name"); self.files_tree.column("name", width=560, anchor="w")
        self.files_tree.heading("size", text="size"); self.files_tree.column("size", width=120, anchor="e")
        self.files_tree.tag_configure("dir", foreground=WHITE)
        self.files_tree.bind("<Double-1>", lambda e: self.files_open())
        self.files_tree.pack(fill="both", expand=True, pady=4)
        act = tk.Frame(f, bg=VOID); act.pack(fill="x", pady=4)
        ttk.Button(act, text="Download…", command=self.files_download).pack(side="left")
        ttk.Button(act, text="Upload…", command=self.files_upload).pack(side="left", padx=4)
        ttk.Button(act, text="Delete", command=self.files_delete).pack(side="left", padx=4)
        ttk.Button(act, text="New folder…", command=self.files_mkdir).pack(side="left", padx=4)
        ttk.Button(act, text="Set up folders", command=self.provision_sd).pack(side="left", padx=(18, 4))
        ttk.Button(act, text="Format SD…", style="Bad.TButton", command=self.format_sd).pack(side="left")
        ttk.Button(act, text="Open as USB drive…", command=self.open_as_drive).pack(side="right")
        self.files_prog = ttk.Progressbar(f, mode="determinate"); self.files_prog.pack(fill="x")
        self.files_info = tk.StringVar(value="")
        tk.Label(f, textvariable=self.files_info, bg=VOID, fg=STEEL, font=self.fonts.small, anchor="w").pack(anchor="w", pady=(2, 8))

    def files_load(self):
        p = self.path_var.get().strip() or "/sd"
        def work(b):
            return b.ls(p), b.sd_info()
        def done(res):
            (final, ents), info = res
            self.files_tree.delete(*self.files_tree.get_children())
            for e in ents:
                self.files_tree.insert("", "end", values=(e["n"], "" if e["d"] else nbridge.human_size(e["s"])), tags=("dir",) if e["d"] else ())
            self.path_var.set(final.get("path", p))
            self.files_info.set("%d entries%s  ·  card %s free of %s" % (final.get("n", 0), " (first 200 shown)" if final.get("trunc") else "",
                                                                        nbridge.human_size(info.get("free")), nbridge.human_size(info.get("total"))))
        self.run_bridge(work, done, "listing " + p)

    def _sel(self):
        sel = self.files_tree.selection()
        if not sel:
            return None, False
        vals = self.files_tree.item(sel[0], "values")
        return vals[0], "dir" in self.files_tree.item(sel[0], "tags")

    def files_open(self):
        name, is_dir = self._sel()
        if name and is_dir:
            self.path_var.set(self.path_var.get().rstrip("/") + "/" + name)
            self.files_load()
        elif name:
            self.files_download()

    def files_up(self):
        p = self.path_var.get().rstrip("/")
        i = p.rfind("/")
        self.path_var.set(p[:i] if i > 3 else "/sd")
        self.files_load()

    def files_download(self):
        name, is_dir = self._sel()
        if not name or is_dir:
            return
        dest = filedialog.asksaveasfilename(title="Save as", initialfile=name)
        if not dest:
            return
        remote = self.path_var.get().rstrip("/") + "/" + name
        def work(b):
            return b.get(remote, dest, progress=lambda d, t: self.ui_q.put(lambda: self.files_prog.configure(value=100 * d / t if t else 0)))
        self.run_bridge(work, lambda n: self.set_status("downloaded %s (%s)" % (name, nbridge.human_size(n))), "downloading " + name)

    def files_upload(self):
        src = filedialog.askopenfilename(title="Upload to the watch")
        if not src:
            return
        remote = self.path_var.get().rstrip("/") + "/" + os.path.basename(src)
        def work(b):
            return b.put(src, remote, progress=lambda d, t: self.ui_q.put(lambda: self.files_prog.configure(value=100 * d / t if t else 0)))
        self.run_bridge(work, lambda n: (self.set_status("uploaded %s (%s)" % (os.path.basename(src), nbridge.human_size(n))), self.files_load()), "uploading")

    def files_delete(self):
        name, is_dir = self._sel()
        if not name:
            return
        if not messagebox.askyesno(APP_NAME, "Delete %s '%s' from the card?%s" % ("folder" if is_dir else "file", name, "\n(folders must be empty)" if is_dir else "")):
            return
        remote = self.path_var.get().rstrip("/") + "/" + name
        self.run_bridge(lambda b: b.rm(remote), lambda r: self.files_load(), "deleting " + name)

    def files_mkdir(self):
        name = simple_prompt(self, "New folder", "Folder name:")
        if not name:
            return
        remote = self.path_var.get().rstrip("/") + "/" + name
        self.run_bridge(lambda b: b.mkdir(remote), lambda r: self.files_load(), "creating " + name)

    def provision_sd(self):
        self.run_bridge(lambda b: b.sd_provision(), lambda r: (self.set_status("folders ready (%d created)" % r.get("made", 0)), self.files_load()), "setting up the folders")

    def flash_provision(self):
        """Set up folders from the Flash tab — same sd.provision, but shown on the flash page (log + bar)."""
        if self._busy_block("folder setup"):
            return
        if not self.bridge:
            messagebox.showwarning(APP_NAME, "Setting up folders needs NocSif running on the watch, with a microSD inserted."); return
        self.show_page("flash"); self.flash_out.delete("1.0", "end"); self._prog_busy()
        self._fl("== setting up the NocSif microSD folders (firmware, audio, wifi, ble, notes, voice, tracks, wardrive)… ==")
        def done(r):
            self._fl("== folders ready (%d created) ==" % r.get("made", 0))
            self.set_status("folders ready (%d created)" % r.get("made", 0))
            self._prog_set(100); self.files_load()
        self.run_bridge(lambda b: b.sd_provision(), done, "setting up the folders")

    def open_as_drive(self):
        if not messagebox.askyesno(APP_NAME, "Switch the watch to File Share (USB mass storage)?\n\nThe card mounts on this computer as a normal drive — best for big or many files. This app's connection drops while File Share is on; set USB back to Detached on the watch (System › USB) or restart it, then it reconnects."):
            return
        def done(r):
            self.disconnect()
            self.set_status("File Share requested — the drive appears in a few seconds; the app reconnects once USB is back to Detached")
        self.run_bridge(lambda b: b.usb("msc"), done, "switching to File Share")

    def format_sd(self):
        if not messagebox.askyesno(APP_NAME, "Format the microSD card?\n\nEVERYTHING on the card is erased (captures, audio, notes, voice memos, tracks, macros, the firmware image). The card is reformatted as FAT and the NocSif folders are created again.", icon="warning"):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — erase the whole card now?", icon="warning"):
            return
        def work(b):
            r = b.sd_format()
            b.sd_provision()
            return r
        self.run_bridge(work, lambda r: (self.set_status("card formatted"), self.path_var.set("/sd"), self.files_load()), "formatting the card")

    # ---- Control page -------------------------------------------------------------------------------
    def _build_control(self):
        f = self.pages["control"]
        # packing the screen column first (side=right) reserves its fixed 205px; the menu column then fills what's left
        right = tk.Frame(f, bg=VOID); right.pack(side="right", fill="y", pady=6)
        ntheme.section(right, "screen", self.fonts).pack(anchor="w")
        self.shot_lbl = tk.Label(right, bg="#000", width=205, height=251, highlightthickness=1, highlightbackground=EDGE)
        self.shot_lbl.pack()
        ttk.Button(right, text="Live view", style="Accent.TButton", command=self.open_live).pack(fill="x", pady=(6, 2))
        sb = tk.Frame(right, bg=VOID); sb.pack(fill="x")
        ttk.Button(sb, text="Screenshot", command=self.screenshot).pack(side="left", fill="x", expand=True)
        ttk.Button(sb, text="Save…", command=self.save_shot).pack(side="left", fill="x", expand=True, padx=(4, 0))
        self.state_var = tk.StringVar(value="")
        tk.Label(right, textvariable=self.state_var, bg=VOID, fg=STEEL, font=self.fonts.small, wraplength=205, justify="left").pack(anchor="w")
        left = tk.Frame(f, bg=VOID); left.pack(side="left", fill="both", expand=True, padx=(0, 12), pady=6)
        ntheme.section(left, "menu · double-click launches", self.fonts).pack(anchor="w")
        self.menu_tree = ttk.Treeview(left, show="tree", height=9)
        self.menu_tree.bind("<Double-1>", lambda e: self.ctl_launch_selected())
        self.menu_tree.pack(fill="both", expand=True, pady=4)
        nav = tk.Frame(left, bg=VOID); nav.pack(fill="x")
        ttk.Button(nav, text="Load menu", command=self.load_menu).pack(side="left")
        ttk.Button(nav, text="Launch", style="Accent.TButton", command=self.ctl_launch_selected).pack(side="left", padx=4)
        ttk.Button(nav, text="Home", command=lambda: self.ctl("home")).pack(side="left", padx=4)
        ttk.Button(nav, text="Back", command=lambda: self.ctl("back")).pack(side="left")
        typ = tk.Frame(left, bg=VOID); typ.pack(fill="x", pady=(8, 0))
        self.type_var = tk.StringVar()
        ttk.Entry(typ, textvariable=self.type_var, width=22, font=self.fonts.body).pack(side="left")
        ttk.Button(typ, text="Type", command=lambda: self.ctl("type", text=self.type_var.get())).pack(side="left", padx=4)
        ttk.Button(typ, text="⌫", command=lambda: self.ctl("key", key="backspace")).pack(side="left")
        ttk.Button(typ, text="Enter", command=lambda: self.ctl("key", key="enter")).pack(side="left", padx=4)
        sl = tk.Frame(left, bg=VOID); sl.pack(fill="x", pady=(8, 0))
        tk.Label(sl, text="brightness", bg=VOID, fg=STEEL, font=self.fonts.small).grid(row=0, column=0, sticky="w")
        self.bright = ttk.Scale(sl, from_=24, to=255, orient="horizontal", length=220, command=lambda v: self._slider("bright", v))
        self.bright.set(200); self.bright.grid(row=0, column=1, padx=8)
        tk.Label(sl, text="volume", bg=VOID, fg=STEEL, font=self.fonts.small).grid(row=1, column=0, sticky="w")
        self.vol = ttk.Scale(sl, from_=0, to=255, orient="horizontal", length=220, command=lambda v: self._slider("vol", v))
        self.vol.set(170); self.vol.grid(row=1, column=1, padx=8)
        btn = tk.Frame(left, bg=VOID); btn.pack(fill="x", pady=(8, 0))
        for k, lbl in (("fn", "FN"), ("pwr", "PWR")):
            ttk.Button(btn, text=lbl, command=lambda k=k: self.ctl("button", k=k, l=0)).pack(side="left")
            ttk.Button(btn, text=lbl + " long", command=lambda k=k: self.ctl("button", k=k, l=1)).pack(side="left", padx=(2, 10))
        ttk.Button(right, text="Web remote (phone)…", command=self.open_companion).pack(fill="x", pady=(4, 0))

    def _slider(self, which, v):
        now = time.time()
        if now - self._slider_t.get(which, 0) < 0.15 or not self.bridge:
            return
        self._slider_t[which] = now
        self.ctl(which, v=int(float(v)), quiet=True)

    def ctl(self, action, quiet=False, **args):
        if not self.bridge:
            self.set_status("no NocSif watch connected"); return
        self.run_bridge(lambda b: b.ctl(action, **args), lambda r: None if quiet else self.set_status("sent " + action), None if quiet else action)

    def load_menu(self):
        def work(b):
            return b.menu()
        def done(m):
            self.menu_tree.delete(*self.menu_tree.get_children())
            def add(parent, rows):
                for r in rows or []:
                    node = self.menu_tree.insert(parent, "end", text=r.get("label", r.get("id")) + ("  ⚠ radio" if r.get("warn") else ""), values=(r.get("id", ""),), open=False)
                    add(node, r.get("sub"))
            for c in m.get("cats", []):
                node = self.menu_tree.insert("", "end", text=c.get("label", ""), values=("",), open=True)
                add(node, c.get("rows"))
        self.run_bridge(work, done, "loading the menu")

    def ctl_launch_selected(self):
        sel = self.menu_tree.selection()
        if not sel:
            return
        vals = self.menu_tree.item(sel[0], "values")
        if vals and vals[0]:
            self.ctl("launch", app=vals[0])

    def screenshot(self):
        def work(b):
            w, h, data = b.screenshot()
            return w, h, nbridge.rgb565_to_png(w, h, data)
        def done(res):
            w, h, png = res
            self._shot_png = png                          # kept at full 410×502 resolution for Save…
            full = tk.PhotoImage(data=png)
            self.shot_img = full.subsample(2, 2)          # displayed on the page at half size
            self.shot_lbl.configure(image=self.shot_img, width=w // 2, height=h // 2)
        self.run_bridge(work, done, "capturing the screen")

    def save_shot(self):
        if not self._shot_png:
            return
        dest = filedialog.asksaveasfilename(title="Save screenshot", defaultextension=".png", initialfile="nocsif.png")
        if dest:
            with open(dest, "wb") as fh:
                fh.write(self._shot_png)

    def open_live(self):
        if not self.bridge:
            self.set_status("no NocSif watch connected"); return
        if self.live and self.live.winfo_exists():
            self.live.lift(); return
        self.live = LiveView(self)

    def open_companion(self):
        messagebox.showinfo(APP_NAME, "The web remote (touch, screen mirror, casting from a phone) is served by the watch itself over WiFi:\n\n1. On the watch: System › Companion › Start\n2. Join the watch's network\n3. The page opens at %s\n\nOn this computer the Live view button does the same over USB." % COMPANION_URL)
        webbrowser.open(COMPANION_URL)

    # ---- Log page -----------------------------------------------------------------------------------
    def _build_log(self):
        f = self.pages["log"]
        bar = tk.Frame(f, bg=VOID); bar.pack(fill="x", pady=(6, 4))
        ttk.Button(bar, text="Fetch stored log", command=self.fetch_log).pack(side="left")
        ttk.Button(bar, text="Clear", command=lambda: self.log_txt.delete("1.0", "end")).pack(side="left", padx=4)
        self.autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="follow", variable=self.autoscroll).pack(side="left", padx=8)
        self.log_txt = scrolledtext.ScrolledText(f, bg=PIT, fg=BONE, insertbackground=BONE, font=self.fonts.small, relief="flat",
                                                 highlightthickness=1, highlightbackground=EDGE)
        self.log_txt.pack(fill="both", expand=True, pady=(0, 8))

    def fetch_log(self):
        self.run_bridge(lambda b: b.log_tail(4096), lambda t: (self.log_txt.insert("end", "---- stored log ----\n" + t + "\n"), self.log_txt.see("end")), "fetching the log")

    # ---- connecting, identifying the board, and background-worker plumbing -------------------------
    def refresh_ports(self):
        self._ports = nbridge.find_ports()

    def connect(self, port, auto=False, after_flash=None):
        """Opens the serial port and confirms the firmware's bridge is responding (via ping). If the
        port opens but nothing answers, falls back to identifying the board from the ROM side (a
        stock LilyGo build, a blank chip, or NocSif present but silent)."""
        if not port or self.busy:
            return
        try:
            b = nbridge.Bridge(port, log_cb=self.log_q.put)
        except Exception as e:
            if not auto:
                self.set_status("open %s failed: %s" % (port, e))
            self._failed_ports[port] = time.time()
            return
        self.port = port
        self.set_status("checking %s…" % port)
        def probe():
            try:
                b.ping()
            except Exception:
                b.close()
                self.ui_q.put(lambda: self.identify_port(port, after_flash=after_flash))
                return
            self.ui_q.put(lambda: self._connected(b, port, after_flash))
        threading.Thread(target=probe, daemon=True).start()

    def _connected(self, b, port, after_flash=None):
        if self.bridge:
            b.close()
            return
        self.bridge = b
        self.kind = "nocsif"
        self.ident = None
        self.port = port
        self.update_menu_state()
        self._show_watch_page()
        self.set_status("connected on " + port)
        self._log_thread_run = True
        threading.Thread(target=self._log_pump, daemon=True).start()
        self.load_overview()
        if after_flash:
            self._after_flash(after_flash)

    def identify_port(self, port, force=False, after_flash=None):
        """Called once the bridge fails to answer: queries the ROM bootloader for what's actually on
        the board (esptool resets the chip into the loader and back out again). Only runs once per
        port appearance unless force=True is passed."""
        if not port or self.busy:
            return
        if not force and port in self._probed_ports and time.time() - self._probed_ports[port] < 60:
            return
        self._probed_ports[port] = time.time()
        self.port = port
        self.set_status("no NocSif bridge on %s — asking the chip's ROM loader…" % port)
        self.card.set(name="identifying…", line="talking to the ROM loader on %s" % port, batt=None, linked=False, kind="none", status="")
        self.busy = True
        def work():
            return flasher.identify(port, self._fl)
        def done(idn):
            self.busy = False
            self.ident = idn
            if not idn:
                self.kind = "none"
                self._failed_ports[port] = time.time()
                self.card.set(name="no answer", line="a device is on %s but the ROM loader did not answer" % port, batt=None, linked=False, kind="none", status="")
                self.set_status("nothing answered on %s" % port)
            elif idn.get("nocsif"):
                self.kind = "silent"
                v = idn["nocsif"].get("version", "?")
                self.card.set(name="NocSif %s" % v, line="on the board but not answering  ·  MAC %s" % (idn["rom"].get("mac") or "?"), batt=None, linked=False, kind="stock", status=port)
                self.set_status("NocSif %s on %s is not answering — see Watch" % (v, port))
            elif idn.get("arduino"):
                self.kind = "stock"
                a = idn["arduino"]
                self.card.set(name="%s %s" % (a.get("project") or "LilyGo firmware", a.get("version") or ""),
                              line="stock T-Watch Ultra  ·  %s  ·  MAC %s" % (idn["rom"].get("flash_size") or "?", idn["rom"].get("mac") or "?"),
                              batt=None, linked=False, kind="stock", status=port)
                self.set_status("stock T-Watch Ultra on %s — see Watch for what the app can do" % port)
            else:
                self.kind = "blank"
                self.card.set(name="blank board", line="ESP32-S3 answers, no app image found  ·  MAC %s" % (idn["rom"].get("mac") or "?"), batt=None, linked=False, kind="stock", status=port)
                self.set_status("blank / unknown board on %s" % port)
            self.update_menu_state()
            self._show_watch_page()
            self.show_page("watch")
        def fail(e):
            self.busy = False
            self.kind = "none"
        self.run_bg(work, done, "identifying the board", on_error=fail)

    def disconnect(self, keep_kind=False):
        self._log_thread_run = False
        if self.live:
            self.live.close()
        if self.bridge:
            self.bridge.close()
            self.bridge = None
        if not keep_kind:
            self.kind = "none"
            self.ident = None
            self.card.set(name="no watch", line="plug a T-Watch Ultra in over USB-C", batt=None, linked=False, kind="none", status="")
            self._show_watch_page()
        ntheme.apply_accent(ntheme.ACCENT_DEFAULT, self.style, self.fonts)   # revert to NocSif purple once no watch supplies a theme
        self.update_menu_state()

    def _port_lost(self):
        if self.bridge:
            self.disconnect()
            self.set_status("watch unplugged")

    def _log_pump(self):
        b = self.bridge
        while self._log_thread_run and b is self.bridge and b is not None:
            try:
                b.pump_log(0.5)
            except Exception:
                if b is self.bridge:
                    self.ui_q.put(self._port_lost)
                break

    def _autoconnect_tick(self):
        """Auto-detects whatever gets plugged in, qFlipper-style, without user action. A port that gave
        no answer is retried every 20 seconds; a board already identified via the ROM side is left as
        it is until the user hits Re-check."""
        try:
            if self.bridge is None and not self.busy:
                ports = [d for d, desc in nbridge.find_ports()]
                if not ports:
                    if self.kind != "none":
                        self.port = None
                        self.disconnect()
                        self.set_status("watch unplugged")
                elif self.kind == "none" or self.port not in ports:
                    p = ports[0]
                    if time.time() - self._failed_ports.get(p, 0) > 20 and (p not in self._probed_ports or time.time() - self._probed_ports[p] > 60):
                        self.port_var.set(p)
                        self.connect(p, auto=True)
        finally:
            self.after(2000, self._autoconnect_tick)

    def _check_app_update(self):
        try:
            rel = updater.latest_app_release()
        except Exception:
            return
        if rel and updater.version_tuple(rel["version"]) > updater.version_tuple(APP_VERSION):
            self._app_release = rel
            def show():
                self.upd_link.configure(text="update to v%s ↓" % rel["version"])
                self.upd_link.bind("<Button-1>", lambda e: self.update_app())
            self.ui_q.put(show)

    def update_app(self):
        """Self-update: download the newest published app-v* build from GitHub and restart into it. On the
        packaged Windows .exe a detached helper .bat waits for this process to exit, swaps the exe, and
        relaunches. Running from source can't self-replace a script tree — point the user at git."""
        rel = self._app_release
        if not rel:                                          # not checked yet / footer not populated — check now
            def got(r):
                self._app_release = r
                if r and updater.version_tuple(r["version"]) > updater.version_tuple(APP_VERSION):
                    self.update_app()
                else:
                    messagebox.showinfo(APP_NAME, "You're on the latest app (v%s)." % APP_VERSION)
            self.run_bg(updater.latest_app_release, got, "checking for an app update")
            return
        ver = rel["version"]
        exe_url = exe_name = None
        for name, url in rel.get("assets", {}).items():      # prefer the windows-x64 exe, else any .exe
            if name.lower().endswith(".exe") and (exe_url is None or "windows" in name.lower()):
                exe_url, exe_name = url, name
        if not getattr(sys, "frozen", False):
            if messagebox.askyesno(APP_NAME, "This is the source version (v%s). Self-update replaces the packaged .exe — "
                                             "from source, pull the latest with `git pull`.\n\nOpen the releases page for v%s?"
                                             % (APP_VERSION, ver)):
                webbrowser.open(rel.get("url") or RELEASES_URL)
            return
        if not exe_url:
            messagebox.showinfo(APP_NAME, "Release v%s has no Windows .exe asset. Opening the releases page." % ver)
            webbrowser.open(rel.get("url") or RELEASES_URL); return
        if not messagebox.askyesno(APP_NAME, "Update to NocSif Desktop Bridge v%s?\n\nThe new build downloads, then the app "
                                             "closes, replaces itself, and reopens." % ver):
            return
        dest_dir = os.path.dirname(sys.executable) or HOME_DIR
        new_exe = os.path.join(dest_dir, "NocSifBridge-update-v%s.exe" % ver.replace("/", "_"))
        def work():
            updater.download_url(exe_url, new_exe,
                                 progress=lambda d, t: self.ui_q.put(lambda: self.set_status(
                                     "downloading app v%s… %d%%" % (ver, (100 * d // t) if t else 0))))
            if os.path.getsize(new_exe) < 1_000_000:
                raise RuntimeError("the download is too small (%d bytes) — aborting" % os.path.getsize(new_exe))
            return new_exe
        def done(path):
            self.set_status("update downloaded — restarting into v%s…" % ver)
            self._self_replace_and_restart(path)
        self.run_bg(work, done, "downloading app v%s" % ver)

    def _self_replace_and_restart(self, new_exe):
        """Windows self-replace: a detached .bat waits for THIS exe to unlock (this process exits), moves the
        new build over it, relaunches, then deletes itself. If the move never succeeds it relaunches the
        current build so the app never stays closed. Frozen builds only."""
        target = sys.executable
        bat = os.path.join(tempfile.gettempdir(), "nocsif_update_%d.bat" % os.getpid())
        lines = [
            "@echo off",
            "set N=0",
            ":retry",
            'move /Y "%s" "%s" >nul 2>&1' % (new_exe, target),
            "if not errorlevel 1 goto ok",
            "set /a N+=1",
            "if %N% GEQ 40 goto ok",                          # ~40 s of retries, then relaunch whatever is there
            "timeout /t 1 /nobreak >nul",
            "goto retry",
            ":ok",
            'start "" "%s"' % target,
            'del "%~f0"',
        ]
        try:
            with open(bat, "w", encoding="utf-8", newline="\r\n") as fh:
                fh.write("\n".join(lines) + "\n")
        except OSError as e:
            messagebox.showerror(APP_NAME, "Could not write the updater helper: %s" % e); return
        DETACHED_PROCESS = 0x00000008
        subprocess.Popen(["cmd", "/c", bat], creationflags=DETACHED_PROCESS, close_fds=True)
        if self.bridge:
            self.bridge.close()
        self.after(300, self.destroy)

    def run_bridge(self, work, done, label):
        if not self.bridge:
            self.set_status("no NocSif watch connected"); return
        b = self.bridge
        self.run_bg(lambda: work(b), done, label)

    def run_bg(self, work, done, label, on_error=None):
        if label:
            self.set_status(label + "…")
        def t():
            try:
                res = work()
            except Exception as e:
                # Bind the exception + its messages to plain locals NOW: Python deletes the `except ... as e`
                # name at the end of this block, so a deferred UI lambda that closed over `e` would raise
                # "cannot access free variable 'e'" when _tick runs it later — and the failure dialog would
                # silently never appear (the whole point of surfacing the error). Plain locals survive.
                err = e
                status = "%s failed: %s" % (label or "action", err)
                detail = "%s failed:\n%s" % (label or "action", err)
                self.ui_q.put(lambda: self.set_status(status))
                self._prog_idle()                       # stop the 'working…' marquee on any failure
                self.ui_q.put(lambda: messagebox.showerror(APP_NAME, detail))
                if on_error:
                    self.ui_q.put(lambda: on_error(err))
                return
            self.ui_q.put(lambda: (done(res) if done else None, label and self.set_status(label + " — done")))
        threading.Thread(target=t, daemon=True).start()

    def _tick(self):
        try:
            while True:
                fn = self.ui_q.get_nowait()
                try:
                    fn()
                except Exception as e:
                    print("ui:", e)
        except queue.Empty:
            pass
        n = 0
        try:
            while n < 200:
                line = self.log_q.get_nowait()
                self.log_txt.insert("end", line + "\n")
                n += 1
        except queue.Empty:
            pass
        if n and self.autoscroll.get():
            self.log_txt.see("end")
        self.after(80, self._tick)


class LiveView(tk.Toplevel):
    """Shows the watch's screen live, rendered at the panel's own native 410×502 pixels so mouse
    position maps to touch position 1:1. A background thread repeatedly polls the watch for the
    rectangle that changed (`mirror`), decodes its RLE data, and hands a PPM image patch over to the UI
    thread, which blits it into the big PhotoImage using Tk's native `copy` command (scaled 2x back up
    when running in the half-resolution mode, which cuts data volume 4x for a slow link). Mouse press,
    drag and release turn into touch events that ride along with the next poll; a press and its
    matching release are kept at least 80 ms apart so LVGL's ~30 ms input polling loop can see both."""
    W, H = 410, 502
    MIN_PRESS_MS = 80

    def __init__(self, app):
        super().__init__(app)
        self.app = app
        self.title("NocSif — live view")
        self.configure(bg=VOID)
        self.resizable(False, False)
        try:
            if sys.platform.startswith("win"):
                self.iconbitmap(os.path.join(BASE, "nocsif.ico"))
        except Exception:
            pass
        root = nchrome.apply(self, app.fonts, "NocSif — live view", resizable=False, minimizable=False, on_close=self.close)
        self.canvas = tk.Canvas(root, width=self.W, height=self.H, bg="#000", highlightthickness=1,
                                highlightbackground=EDGE2, cursor="hand2")
        self.canvas.pack(padx=12, pady=(8, 4))
        self.img = tk.PhotoImage(width=self.W, height=self.H)
        self.canvas.create_image(0, 0, anchor="nw", image=self.img)
        bar = tk.Frame(root, bg=VOID); bar.pack(fill="x", padx=12, pady=(0, 12))
        for k, lbl in (("fn", "FN"), ("pwr", "PWR")):
            ttk.Button(bar, text=lbl, command=lambda k=k: app.ctl("button", k=k, l=0, quiet=True)).pack(side="left")
            ttk.Button(bar, text=lbl + " long", command=lambda k=k: app.ctl("button", k=k, l=1, quiet=True)).pack(side="left", padx=(2, 8))
        self.cast = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="Cast (blank watch)", variable=self.cast, command=self._cast).pack(side="left", padx=8)
        self.half = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="half res", variable=self.half, command=self._resync).pack(side="left", padx=8)
        self.fps_var = tk.StringVar(value="")
        tk.Label(bar, textvariable=self.fps_var, bg=VOID, fg=STEEL, font=app.fonts.small).pack(side="right")
        self._want_full = True
        self.canvas.bind("<ButtonPress-1>", lambda e: self._touch(e, 1, press=True))
        self.canvas.bind("<B1-Motion>", lambda e: self._touch(e, 1))
        self.canvas.bind("<ButtonRelease-1>", lambda e: self._touch(e, 0))
        self.events = collections.deque()
        self.lock = threading.Lock()
        self.running = True
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.after(30, lambda: nchrome.fit(self))          # the frameless window chrome needs its size set explicitly
        threading.Thread(target=self._loop, daemon=True).start()

    def _touch(self, e, pressed, press=False):
        x = max(0, min(self.W - 1, int(e.x)))          # the canvas maps directly onto the panel, pixel for pixel
        y = max(0, min(self.H - 1, int(e.y)))
        with self.lock:
            if pressed and not press and self.events and self.events[-1][2] == 1 and not self.events[-1][3]:
                self.events[-1] = (x, y, 1, False)
            else:
                self.events.append((x, y, pressed, press))

    def _cast(self):
        self.app.ctl("cast", on=1 if self.cast.get() else 0, quiet=True)

    def _resync(self):
        self._want_full = True                          # switching resolution invalidates the current frame, so request a full one

    def _next_event(self, last_press_t):
        with self.lock:
            if not self.events:
                return None
            ev = self.events[0]
            if ev[2] == 0 and time.time() - last_press_t < self.MIN_PRESS_MS / 1000.0:
                return None
            return self.events.popleft()

    def _loop(self):
        seq, full = 0, True
        frames, t0, last_press = 0, time.time(), 0.0
        while self.running:
            b = self.app.bridge
            if b is None:
                break
            ev = self._next_event(last_press)
            if ev and ev[3]:
                last_press = time.time()
            scale = 2 if self.half.get() else 1
            if self._want_full:
                full, self._want_full = True, False
            try:
                final, raw = b.mirror_poll(seq, full=full, touch=ev[:3] if ev else None, scale=scale)
            except nbridge.BridgeError:
                full = True
                time.sleep(0.25)
                continue
            except Exception:
                break
            full = False
            if final.get("none"):
                if not self.events:
                    time.sleep(0.05)
                continue
            if len(raw) != int(final.get("raw", -1)):
                full = True
                continue
            seq = int(final["seq"])
            x, y, w, h = int(final["x"]), int(final["y"]), int(final["w"]), int(final["h"])
            sc = int(final.get("scale", 1))
            ppm = nbridge.ppm_from_rgb565(w, h, raw)
            self.app.ui_q.put(lambda x=x, y=y, ppm=ppm, sc=sc: self._paint(x, y, ppm, sc))
            frames += 1
            now = time.time()
            if now - t0 >= 1.0:
                self.app.ui_q.put(lambda f=frames / (now - t0), n=len(raw): self.fps_var.set("%.0f fps · last %s" % (f, nbridge.human_size(n))))
                frames, t0 = 0, now
        self.app.ui_q.put(lambda: self.fps_var.set("stopped"))

    def _paint(self, x, y, ppm, sc=1):
        if not self.running:
            return
        try:
            patch = tk.PhotoImage(data=ppm)
            if sc > 1:
                self.img.tk.call(self.img, "copy", patch, "-to", x * sc, y * sc, "-zoom", sc, sc)
            else:
                self.img.tk.call(self.img, "copy", patch, "-to", x, y)
        except tk.TclError:
            pass

    def close(self):
        self.running = False
        if self.cast.get():
            try:
                self.app.ctl("cast", on=0, quiet=True)
            except Exception:
                pass
        if self.app.live is self:
            self.app.live = None
        try:
            self.destroy()
        except tk.TclError:
            pass


def restore_prompt(parent, has_flash, has_sd):
    """Prompts for which parts of a complete backup to restore; returns (flash, sd) as booleans, or None if the user cancels."""
    win = tk.Toplevel(parent); win.title("Restore"); win.configure(bg=VOID); win.grab_set()
    tk.Label(win, text="Restore what?", bg=VOID, fg=WHITE, font=parent.fonts.h2).pack(padx=16, pady=(14, 6), anchor="w")
    vf, vs = tk.BooleanVar(value=has_flash), tk.BooleanVar(value=has_sd)
    cf = ttk.Checkbutton(win, text="Flash — firmware, settings, credentials, bonds (16 MB, the watch reboots)", variable=vf)
    cs = ttk.Checkbutton(win, text="microSD files — put every backed-up file back on the card", variable=vs)
    cf.pack(anchor="w", padx=16, pady=2); cs.pack(anchor="w", padx=16, pady=2)
    if not has_flash:
        cf.state(["disabled"])
    if not has_sd:
        cs.state(["disabled"])
    out = {"v": None}
    def ok():
        out["v"] = (vf.get() and has_flash, vs.get() and has_sd); win.destroy()
    row = tk.Frame(win, bg=VOID); row.pack(pady=12)
    ttk.Button(row, text="Restore", style="Accent.TButton", command=ok).pack(side="left", padx=4)
    ttk.Button(row, text="Cancel", command=win.destroy).pack(side="left", padx=4)
    parent.wait_window(win)
    v = out["v"]
    return v if v and (v[0] or v[1]) else None


def simple_prompt(parent, title, label):
    win = tk.Toplevel(parent); win.title(title); win.configure(bg=VOID); win.grab_set()
    tk.Label(win, text=label, bg=VOID, fg=BONE, font=parent.fonts.body).pack(padx=14, pady=(12, 4))
    var = tk.StringVar()
    ent = ttk.Entry(win, textvariable=var, width=32); ent.pack(padx=14); ent.focus_set()
    out = {"v": None}
    def ok(*_):
        out["v"] = var.get().strip(); win.destroy()
    ttk.Button(win, text="OK", command=ok).pack(pady=10)
    ent.bind("<Return>", ok)
    parent.wait_window(win)
    return out["v"]


if __name__ == "__main__":
    App().mainloop()

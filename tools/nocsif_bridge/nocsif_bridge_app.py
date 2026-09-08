r"""
NocSif Desktop Bridge — the computer-side companion app (PLAN §4.15, a qFlipper analog).

    python nocsif_bridge_app.py            (Windows / macOS / Linux; Python 3.9+, Tk 8.6)

Plug the watch in over USB-C and:
  Overview  — who is connected, running vs published firmware, one-click Update (USB flash)
  Health    — the hardware-defect check (pass / fail per subsystem) + the active self-tests
  Flash     — Flash new watch (blank board provisioning + microSD setup), Wipe & reflash (keeps
              settings), Full wipe (erases settings too), flash a local image
  Files     — the microSD: browse, download, upload, delete, new folder, set up folders, format
  Control   — drive the watch from the computer: menu tree, navigation, typing, brightness /
              volume, side buttons, screenshot; open the live-control web page
  Log       — the live serial log + the stored log ring

Requires: pyserial, esptool, requests (pip install -r requirements.txt). Talks to the watch through
nbridge.py (the console JSON protocol), flashes through flasher.py (esptool, --no-stub), and reads the
published firmware through updater.py (the public GitHub mirror).
"""
import collections
import json
import os
import queue
import sys
import threading
import time
import webbrowser
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nbridge      # noqa: E402
import flasher      # noqa: E402
import updater      # noqa: E402

APP_NAME = "NocSif Desktop Bridge"
APP_VERSION = "0.2.0"       # release_app.ps1 reads this; releases are tagged app-v<APP_VERSION>
GITHUB_URL = "https://github.com/silverwolf2r/Nocsif-Firmware"
RELEASES_URL = GITHUB_URL + "/releases"
WEBSITE_URL = ""            # eigencat.org — set when the operator wants it shown in the footer
CREDITS = "NocSif firmware + bridge by silverwolf2r"
CACHE_DIR = os.path.join(os.path.expanduser("~"), ".nocsif_bridge", "releases")
COMPANION_URL = "http://nocsif.local/"

BG, PANEL, EDGE, INK, DIM, ACCENT, WARN, OK_C, BAD_C = ("#0b0b0d", "#141418", "#2a2a31", "#c9c9cf", "#8c8c92",
                                                       "#8b7bd8", "#d8b24a", "#6fbf73", "#d86f6f")


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_NAME)
        self.geometry("980x680")
        self.minsize(820, 560)
        self.configure(bg=BG)
        self.bridge = None
        self.busy = False
        self.log_q = queue.Queue()
        self.ui_q = queue.Queue()
        self.manifest = None
        self.version = None
        self.local_bin = None
        self.shot_img = None
        self._slider_t = {}          # before the scales exist: their initial .set() fires the command
        self._shot_png = None
        self._failed_ports = {}      # port -> time of the last "attached but not answering" attempt
        self.live = None
        self._style()
        self._build()
        self.after(100, self._tick)
        self.refresh_ports()
        self.after(1500, self._autoconnect_tick)
        threading.Thread(target=self._check_app_update, daemon=True).start()

    # ---- styling --------------------------------------------------------------------------------
    def _style(self):
        s = ttk.Style(self)
        try:
            s.theme_use("clam")
        except tk.TclError:
            pass
        s.configure(".", background=BG, foreground=INK, fieldbackground=PANEL, bordercolor=EDGE,
                    font=("Segoe UI", 10) if sys.platform.startswith("win") else ("Helvetica", 11))
        s.configure("TNotebook", background=BG, borderwidth=0)
        s.configure("TNotebook.Tab", background=PANEL, foreground=DIM, padding=(14, 6))
        s.map("TNotebook.Tab", background=[("selected", BG)], foreground=[("selected", INK)])
        s.configure("TFrame", background=BG)
        s.configure("Panel.TFrame", background=PANEL)
        s.configure("TLabel", background=BG, foreground=INK)
        s.configure("Dim.TLabel", background=BG, foreground=DIM)
        s.configure("Head.TLabel", background=BG, foreground="#e6e6ea", font=("Georgia", 15))
        s.configure("TButton", background=PANEL, foreground=INK, borderwidth=1, padding=(10, 5))
        s.map("TButton", background=[("active", "#241f38")], foreground=[("disabled", "#5a5a60")])
        s.configure("Warn.TButton", foreground=WARN)
        s.configure("Bad.TButton", foreground=BAD_C)
        s.configure("Treeview", background=PANEL, fieldbackground=PANEL, foreground=INK, rowheight=22)
        s.configure("Treeview.Heading", background=BG, foreground=DIM)
        s.configure("TEntry", fieldbackground=PANEL, foreground=INK, insertcolor=INK)
        s.configure("TCombobox", fieldbackground=PANEL, foreground=INK)
        s.configure("Horizontal.TProgressbar", background=ACCENT, troughcolor=PANEL)
        s.configure("Horizontal.TScale", background=BG, troughcolor=PANEL)
        s.configure("TCheckbutton", background=BG, foreground=INK)

    def _build(self):
        top = ttk.Frame(self)
        top.pack(fill="x", padx=12, pady=(10, 4))
        ttk.Label(top, text="NocSif", style="Head.TLabel").pack(side="left", padx=(0, 16))
        ttk.Label(top, text="port").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(top, textvariable=self.port_var, width=34, state="readonly")
        self.port_box.pack(side="left", padx=6)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(side="left")
        self.conn_btn = ttk.Button(top, text="Connect", command=self.toggle_connect)
        self.conn_btn.pack(side="left", padx=(10, 0))
        self.status_var = tk.StringVar(value="not connected")
        ttk.Label(top, textvariable=self.status_var, style="Dim.TLabel").pack(side="left", padx=16)

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=12, pady=6)
        self.tab_overview = ttk.Frame(self.nb); self.nb.add(self.tab_overview, text="Overview")
        self.tab_health = ttk.Frame(self.nb);   self.nb.add(self.tab_health, text="Health")
        self.tab_flash = ttk.Frame(self.nb);    self.nb.add(self.tab_flash, text="Flash")
        self.tab_files = ttk.Frame(self.nb);    self.nb.add(self.tab_files, text="Files")
        self.tab_control = ttk.Frame(self.nb);  self.nb.add(self.tab_control, text="Control")
        self.tab_log = ttk.Frame(self.nb);      self.nb.add(self.tab_log, text="Log")
        self._build_overview(); self._build_health(); self._build_flash()
        self._build_files(); self._build_control(); self._build_log()

        foot = ttk.Frame(self)
        foot.pack(fill="x", padx=12, pady=(0, 8))
        ttk.Label(foot, text="%s v%s  ·  %s" % (APP_NAME, APP_VERSION, CREDITS), style="Dim.TLabel").pack(side="left")
        self.upd_link = self._link(foot, "", RELEASES_URL)      # text arrives when a newer release exists
        self.upd_link.pack(side="left", padx=14)
        self._link(foot, "GitHub", GITHUB_URL).pack(side="right")
        if WEBSITE_URL:
            self._link(foot, "website", WEBSITE_URL).pack(side="right", padx=(0, 12))

    def _link(self, parent, text, url):
        lbl = tk.Label(parent, text=text, fg=ACCENT, bg=BG, cursor="hand2", font=("Segoe UI", 10, "underline"))
        lbl.bind("<Button-1>", lambda e: webbrowser.open(url))
        return lbl

    # ---- Overview ---------------------------------------------------------------------------------
    def _build_overview(self):
        f = self.tab_overview
        grid = ttk.Frame(f); grid.pack(anchor="w", padx=16, pady=14)
        self.ov = {}
        rows = [("device", "name"), ("running version", "version"), ("slot / state", "slot"), ("last boot", "boot"),
                ("last crash", "crash"), ("battery", "batt"), ("uptime", "uptime"), ("MAC", "mac"),
                ("published version", "published"), ("update", "update")]
        for i, (label, key) in enumerate(rows):
            ttk.Label(grid, text=label, style="Dim.TLabel").grid(row=i, column=0, sticky="w", padx=(0, 18), pady=2)
            v = tk.StringVar(value="—")
            self.ov[key] = v
            ttk.Label(grid, textvariable=v).grid(row=i, column=1, sticky="w", pady=2)
        btns = ttk.Frame(f); btns.pack(anchor="w", padx=16, pady=6)
        ttk.Button(btns, text="Refresh", command=self.load_overview).pack(side="left")
        ttk.Button(btns, text="Check published version", command=self.check_published).pack(side="left", padx=8)
        self.update_btn = ttk.Button(btns, text="Update watch (USB flash)", command=self.update_watch, state="disabled")
        self.update_btn.pack(side="left")
        ttk.Label(f, text="Update flashes the published app image over USB (settings, credentials and bonds are kept).",
                  style="Dim.TLabel").pack(anchor="w", padx=16)

    def load_overview(self):
        def work(b):
            v = b.version(); s = b.status()
            return v, s
        def done(res):
            v, s = res
            self.version = v
            self.ov["name"].set(v.get("name") or "NocSif")
            self.ov["version"].set("%s  (built %s, IDF %s, elf %s)" % (v.get("version"), v.get("build"), v.get("idf"), v.get("elf")))
            self.ov["slot"].set("%s / %s%s" % (v.get("slot"), v.get("ota_state"), "  · SAFE MODE" if v.get("safe") else ""))
            self.ov["boot"].set(v.get("boot", "—"))
            self.ov["crash"].set(v.get("crash") or "none recorded")
            self.ov["batt"].set("%s%%  %s" % (s.get("batt"), "on USB power" if s.get("vbus") else "on battery"))
            self.ov["uptime"].set("%d s" % s.get("uptime_s", 0))
            self.ov["mac"].set(v.get("mac", "—"))
            self._compare_versions()
        self.run_bridge(work, done, "reading the watch")

    def check_published(self):
        def work():
            return updater.fetch_manifest(updater.DEFAULT_REPO)
        def done(m):
            self.manifest = m
            parts = ", ".join(p["file"] for p in m.get("parts", [])) or "firmware.bin"
            self.ov["published"].set("%s  (built %s · %s · %s)" % (m.get("version"), m.get("build"), nbridge.human_size(m.get("size")), parts))
            self._compare_versions()
        self.run_bg(work, done, "fetching manifest.json from GitHub")

    def _compare_versions(self):
        if not (self.version and self.manifest):
            return
        rel = updater.compare_versions(self.version.get("version"), self.manifest.get("version"))
        txt = {"same": "the watch runs the published version", "differs": "the watch runs a different build than the published one",
               "unknown": "—"}[rel]
        self.ov["update"].set(txt)
        self.update_btn.configure(state="normal" if self.bridge else "disabled")

    def update_watch(self):
        if not self.manifest:
            messagebox.showinfo(APP_NAME, "Check the published version first."); return
        if not messagebox.askyesno(APP_NAME, "Flash the published app image %s to the watch over USB?\n\nSettings, credentials and bonds are kept (only the app slot and otadata are written)." % self.manifest.get("version")):
            return
        self._flash_flow(mode="update")

    # ---- Health ----------------------------------------------------------------------------------
    def _build_health(self):
        f = self.tab_health
        bar = ttk.Frame(f); bar.pack(fill="x", padx=16, pady=(12, 4))
        ttk.Button(bar, text="Run hardware check", command=self.run_health).pack(side="left")
        ttk.Label(bar, text="active tests:", style="Dim.TLabel").pack(side="left", padx=(18, 4))
        for t, lbl in (("tone", "Speaker tone"), ("nfc", "NFC front-end"), ("lora", "LoRa RSSI probe"), ("gnss", "GNSS")):
            ttk.Button(bar, text=lbl, command=lambda t=t: self.run_test(t)).pack(side="left", padx=2)
        self.health_sum = tk.StringVar(value="")
        ttk.Label(f, textvariable=self.health_sum).pack(anchor="w", padx=16)
        cols = ("result", "subsystem", "detail")
        self.health_tree = ttk.Treeview(f, columns=cols, show="headings", height=18)
        for c, w in zip(cols, (70, 120, 640)):
            self.health_tree.heading(c, text=c)
            self.health_tree.column(c, width=w, anchor="w")
        self.health_tree.tag_configure("pass", foreground=OK_C)
        self.health_tree.tag_configure("fail", foreground=BAD_C)
        self.health_tree.tag_configure("skip", foreground=DIM)
        self.health_tree.pack(fill="both", expand=True, padx=16, pady=6)
        ttk.Label(f, text="Verdicts of the active tests print in the Log tab. 'skip' = not probed from here (a lazy worker that "
                          "isn't running, or hardware with no standalone test).", style="Dim.TLabel").pack(anchor="w", padx=16, pady=(0, 8))

    def run_health(self):
        def work(b):
            return b.health()
        def done(res):
            final, checks = res
            self.health_tree.delete(*self.health_tree.get_children())
            for k in checks:
                ok = k.get("ok")
                tag = "pass" if ok is True else ("fail" if ok is False else "skip")
                self.health_tree.insert("", "end", values=(tag.upper() if tag != "skip" else "skip", k["n"], k.get("d", "")), tags=(tag,))
            self.health_sum.set("pass %d · fail %d · not probed %d" % (final.get("pass", 0), final.get("fail", 0), final.get("skip", 0)))
        self.run_bridge(work, done, "running the hardware check")

    def run_test(self, t):
        self.run_bridge(lambda b: b.test(t), lambda r: self.set_status(r.get("msg", "started")), "test " + t)

    # ---- Flash -----------------------------------------------------------------------------------
    def _build_flash(self):
        f = self.tab_flash
        left = ttk.Frame(f); left.pack(side="left", fill="y", padx=16, pady=12)
        ttk.Label(left, text="Provision", style="Dim.TLabel").pack(anchor="w")
        ttk.Button(left, text="Flash new watch…", command=self.flash_new_watch).pack(fill="x", pady=2)
        self.erase_first = tk.BooleanVar(value=False)
        ttk.Checkbutton(left, text="erase the whole flash first", variable=self.erase_first).pack(anchor="w", pady=(0, 8))
        ttk.Label(left, text="Update", style="Dim.TLabel").pack(anchor="w")
        ttk.Button(left, text="Update (published app)", command=self.update_watch).pack(fill="x", pady=2)
        ttk.Button(left, text="Flash a local firmware.bin…", command=self.flash_local).pack(fill="x", pady=2)
        ttk.Label(left, text="Wipe", style="Dim.TLabel").pack(anchor="w", pady=(10, 0))
        ttk.Button(left, text="Wipe & reflash (keep settings)", style="Warn.TButton", command=self.wipe_keep).pack(fill="x", pady=2)
        ttk.Button(left, text="Full wipe (erase everything)", style="Bad.TButton", command=self.wipe_full).pack(fill="x", pady=2)
        ttk.Label(left, text="Every write goes through esptool\n(--no-stub) on the selected port.\nThe watch reboots afterwards and\nthe app reconnects on its own.",
                  style="Dim.TLabel", justify="left").pack(anchor="w", pady=(12, 0))
        right = ttk.Frame(f); right.pack(side="left", fill="both", expand=True, padx=(0, 16), pady=12)
        self.flash_prog = ttk.Progressbar(right, mode="determinate")
        self.flash_prog.pack(fill="x")
        self.flash_out = scrolledtext.ScrolledText(right, height=20, bg=PANEL, fg=INK, insertbackground=INK, font=("Consolas", 9), relief="flat")
        self.flash_out.pack(fill="both", expand=True, pady=6)

    def _fl(self, line):
        self.ui_q.put(lambda: (self.flash_out.insert("end", line + "\n"), self.flash_out.see("end")))
        m = None
        if "%" in line and "Writing" in line:
            try:
                m = int(line.split("(")[-1].split("%")[0].strip())
            except ValueError:
                m = None
        if m is not None:
            self.ui_q.put(lambda: self.flash_prog.configure(value=m))

    def _release_dir(self, version):
        return os.path.join(CACHE_DIR, version.replace("/", "_"))

    def _fetch_release(self, want_parts):
        m = self.manifest or updater.fetch_manifest(updater.DEFAULT_REPO)
        self.manifest = m
        dest = self._release_dir(m.get("version", "unknown"))
        self._fl("fetching %s from %s" % (m.get("version"), updater.DEFAULT_REPO))
        files = updater.fetch_release(updater.DEFAULT_REPO, m, dest, want_parts=want_parts,
                                      progress=lambda n, d, t: self.ui_q.put(lambda: self.flash_prog.configure(value=(100 * d / t) if t else 0)))
        for name, info in files.items():
            self._fl("  %s -> 0x%x (%s)" % (name, info["offset"], nbridge.human_size(os.path.getsize(info["path"]))))
        return files

    def _flash_flow(self, mode, local_bin=None):
        """mode: update | new | wipe_keep | wipe_full | local. Runs on a worker; disconnects first."""
        port = self.port_var.get().split(" ")[0]
        if not port:
            messagebox.showerror(APP_NAME, "Pick a port first."); return
        was_connected = self.bridge is not None
        self.disconnect()
        self.nb.select(self.tab_flash)
        self.flash_out.delete("1.0", "end")
        self.flash_prog.configure(value=0)

        def work():
            if mode == "local":
                files = self._fetch_release(want_parts=False) if not os.path.isfile(os.path.join(HERE, "ota_data_initial.bin")) else {}
                ota = files.get("ota_data_initial.bin", {}).get("path") or os.path.join(HERE, "ota_data_initial.bin")
                self._fl("flashing local image %s" % local_bin)
                rc = flasher.flash_app(port, local_bin, ota, self._fl)
            elif mode == "update":
                files = self._fetch_release(want_parts=False)
                rc = flasher.flash_app(port, files["firmware.bin"]["path"], files["ota_data_initial.bin"]["path"], self._fl)
            else:
                files = self._fetch_release(want_parts=True)
                parts = [(i["offset"], i["path"]) for i in files.values()]
                need = {"bootloader.bin", "partitions.bin", "ota_data_initial.bin", "firmware.bin"}
                if need - set(files):
                    raise RuntimeError("the published release lacks %s — full provisioning needs every part" % ", ".join(sorted(need - set(files))))
                rc = 0
                if mode == "wipe_full" or (mode == "new" and self.erase_first.get()):
                    self._fl("== erasing the WHOLE flash (settings included) ==")
                    rc = flasher.erase_flash(port, self._fl)
                elif mode == "wipe_keep":
                    self._fl("== erasing every region except nvs (settings kept) ==")
                    rc = flasher.erase_regions(port, flasher.WIPE_KEEP_NVS_REGIONS, self._fl)
                if rc == 0:
                    self._fl("== writing %s ==" % flasher.describe_parts(parts))
                    rc = flasher.flash_parts(port, parts, self._fl)
            if rc != 0:
                raise RuntimeError("esptool exited with %d — see the output above" % rc)
            self._fl("== done; waiting for the watch to boot ==")
            time.sleep(4.0)
            return mode

        def done(mode):
            self.flash_prog.configure(value=100)
            self.connect(port)
            if mode in ("new", "wipe_full", "wipe_keep"):
                self.after(1500, self._post_provision_check)
            elif was_connected:
                self.after(1500, self.load_overview)
        self.run_bg(work, done, "flashing")

    def flash_local(self):
        path = filedialog.askopenfilename(title="firmware.bin", filetypes=[("ESP app image", "*.bin")])
        if not path:
            return
        if not messagebox.askyesno(APP_NAME, "Flash %s to the app slot (0x20000) + reset otadata?\nSettings are kept." % os.path.basename(path)):
            return
        self._flash_flow("local", local_bin=path)

    def flash_new_watch(self):
        msg = ("Flash new watch writes the COMPLETE published firmware to a board:\n\n"
               "  0x0      bootloader.bin\n  0x8000   partitions.bin\n  0xf000   ota_data_initial.bin\n  0x20000  firmware.bin\n\n"
               "%s\nThen the app reconnects, checks the microSD and offers to set up the NocSif folders.\n\nContinue?"
               % ("The whole flash is ERASED first (settings included)." if self.erase_first.get() else "Existing settings (nvs) are left as they are."))
        if not messagebox.askyesno(APP_NAME, msg):
            return
        self._flash_flow("new")

    def wipe_keep(self):
        regions = "\n".join("  0x%06x  %s" % (o, nbridge.human_size(s)) for o, s in flasher.WIPE_KEEP_NVS_REGIONS)
        if not messagebox.askyesno(APP_NAME, "Wipe & reflash erases these regions, then writes the published firmware:\n\n%s\n\nKEPT: nvs (settings, WiFi credentials, phone bonds), phy_init.\nLOST: the LittleFS store, captures on flash, crash records, logs.\n\nContinue?" % regions):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — erase and reflash the watch now?", icon="warning"):
            return
        self._flash_flow("wipe_keep")

    def wipe_full(self):
        if not messagebox.askyesno(APP_NAME, "FULL WIPE erases the ENTIRE flash: firmware, settings, WiFi credentials, phone bonds, passcode, everything.\nThe published firmware is written afterwards so the watch boots like new.\n\nContinue?", icon="warning"):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — this cannot be undone. Erase everything?", icon="warning"):
            return
        self._flash_flow("wipe_full")

    def _post_provision_check(self):
        if not self.bridge:
            self.set_status("could not reconnect after flashing — unplug/replug and Connect")
            return
        def work(b):
            return b.sd_info()
        def done(i):
            if i.get("present"):
                if messagebox.askyesno(APP_NAME, "microSD found (%s free of %s).\n\nSet up the NocSif folders now (firmware, carts, wifi, ble, notes, voice, tracks, wardrive, ducky)? Only missing ones are created."
                                       % (nbridge.human_size(i.get("free")), nbridge.human_size(i.get("total")))):
                    self.provision_sd()
            else:
                messagebox.showwarning(APP_NAME, "No microSD card in the watch.\n\nFiles, captures, carts, voice memos, notes, tracks and Update need one — insert a card, then use Files › Set up folders (or Format SD) later. The watch works without it otherwise.")
            self.load_overview()
        self.run_bridge(work, done, "checking the microSD")

    # ---- Files -----------------------------------------------------------------------------------
    def _build_files(self):
        f = self.tab_files
        bar = ttk.Frame(f); bar.pack(fill="x", padx=16, pady=(12, 4))
        ttk.Button(bar, text="↑ up", command=self.files_up).pack(side="left")
        self.path_var = tk.StringVar(value="/sd")
        ttk.Entry(bar, textvariable=self.path_var, width=48).pack(side="left", padx=6)
        ttk.Button(bar, text="Go", command=self.files_load).pack(side="left")
        ttk.Button(bar, text="Refresh", command=self.files_load).pack(side="left", padx=(6, 0))
        cols = ("name", "size")
        self.files_tree = ttk.Treeview(f, columns=cols, show="headings", height=16)
        self.files_tree.heading("name", text="name"); self.files_tree.column("name", width=560, anchor="w")
        self.files_tree.heading("size", text="size"); self.files_tree.column("size", width=120, anchor="e")
        self.files_tree.tag_configure("dir", foreground="#e6e6ea")
        self.files_tree.bind("<Double-1>", lambda e: self.files_open())
        self.files_tree.pack(fill="both", expand=True, padx=16, pady=4)
        act = ttk.Frame(f); act.pack(fill="x", padx=16, pady=4)
        ttk.Button(act, text="Download…", command=self.files_download).pack(side="left")
        ttk.Button(act, text="Upload…", command=self.files_upload).pack(side="left", padx=4)
        ttk.Button(act, text="Delete", command=self.files_delete).pack(side="left", padx=4)
        ttk.Button(act, text="New folder…", command=self.files_mkdir).pack(side="left", padx=4)
        ttk.Button(act, text="Set up folders", command=self.provision_sd).pack(side="left", padx=(18, 4))
        ttk.Button(act, text="Format SD…", style="Bad.TButton", command=self.format_sd).pack(side="left")
        ttk.Button(act, text="Open as USB drive…", command=self.open_as_drive).pack(side="right")
        self.files_prog = ttk.Progressbar(f, mode="determinate"); self.files_prog.pack(fill="x", padx=16)
        self.files_info = tk.StringVar(value="")
        ttk.Label(f, textvariable=self.files_info, style="Dim.TLabel").pack(anchor="w", padx=16, pady=(2, 8))

    def files_load(self):
        p = self.path_var.get().strip() or "/sd"
        def work(b):
            info = b.sd_info()
            return b.ls(p), info
        def done(res):
            (final, ents), info = res
            self.files_tree.delete(*self.files_tree.get_children())
            for e in ents:
                self.files_tree.insert("", "end", values=(e["n"], "" if e["d"] else nbridge.human_size(e["s"])), tags=("dir",) if e["d"] else ())
            self.path_var.set(final.get("path", p))
            self.files_info.set("%d entries%s · card %s free of %s" % (final.get("n", 0), " (first 200 shown)" if final.get("trunc") else "",
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

    def open_as_drive(self):
        """Bulk transfers belong on File Share: the card mounts as a normal drive at USB speed. The
        console (and this connection) goes away until the watch's USB mode is set back to Detached."""
        if not messagebox.askyesno(APP_NAME, "Switch the watch to File Share (USB mass storage)?\n\nThe card mounts on this computer as a normal drive — best for big or many files. This app's connection drops while File Share is on; set USB back to Detached on the watch (System › USB) or restart it, then Connect again."):
            return
        def work(b):
            r = b.usb("msc")
            return r
        def done(r):
            self.disconnect()
            self.set_status("File Share requested — the drive appears in a few seconds; reconnect after switching USB back to Detached")
        self.run_bridge(work, done, "switching to File Share")

    def format_sd(self):
        if not messagebox.askyesno(APP_NAME, "Format the microSD card?\n\nEVERYTHING on the card is erased (captures, carts, notes, voice memos, tracks, macros, the firmware image). The card is reformatted as FAT and the NocSif folders are created again.", icon="warning"):
            return
        if not messagebox.askyesno(APP_NAME, "Second confirmation — erase the whole card now?", icon="warning"):
            return
        def work(b):
            r = b.sd_format()
            b.sd_provision()
            return r
        self.run_bridge(work, lambda r: (self.set_status("card formatted"), self.path_var.set("/sd"), self.files_load()), "formatting the card")

    # ---- Control ---------------------------------------------------------------------------------
    def _build_control(self):
        f = self.tab_control
        left = ttk.Frame(f); left.pack(side="left", fill="both", expand=True, padx=(16, 8), pady=12)
        ttk.Label(left, text="menu (double-click launches)", style="Dim.TLabel").pack(anchor="w")
        self.menu_tree = ttk.Treeview(left, show="tree", height=14)
        self.menu_tree.bind("<Double-1>", lambda e: self.ctl_launch_selected())
        self.menu_tree.pack(fill="both", expand=True, pady=4)
        nav = ttk.Frame(left); nav.pack(fill="x")
        ttk.Button(nav, text="Load menu", command=self.load_menu).pack(side="left")
        ttk.Button(nav, text="Launch", command=self.ctl_launch_selected).pack(side="left", padx=4)
        ttk.Button(nav, text="Home", command=lambda: self.ctl("home")).pack(side="left", padx=4)
        ttk.Button(nav, text="Back", command=lambda: self.ctl("back")).pack(side="left")
        typ = ttk.Frame(left); typ.pack(fill="x", pady=(8, 0))
        self.type_var = tk.StringVar()
        ttk.Entry(typ, textvariable=self.type_var, width=32).pack(side="left")
        ttk.Button(typ, text="Type", command=lambda: self.ctl("type", text=self.type_var.get())).pack(side="left", padx=4)
        ttk.Button(typ, text="⌫", command=lambda: self.ctl("key", key="backspace")).pack(side="left")
        ttk.Button(typ, text="Enter", command=lambda: self.ctl("key", key="enter")).pack(side="left", padx=4)
        sl = ttk.Frame(left); sl.pack(fill="x", pady=(8, 0))
        ttk.Label(sl, text="brightness", style="Dim.TLabel").grid(row=0, column=0, sticky="w")
        self.bright = ttk.Scale(sl, from_=24, to=255, orient="horizontal", length=220, command=lambda v: self._slider("bright", v))
        self.bright.set(200); self.bright.grid(row=0, column=1, padx=8)
        ttk.Label(sl, text="volume", style="Dim.TLabel").grid(row=1, column=0, sticky="w")
        self.vol = ttk.Scale(sl, from_=0, to=255, orient="horizontal", length=220, command=lambda v: self._slider("vol", v))
        self.vol.set(170); self.vol.grid(row=1, column=1, padx=8)
        btn = ttk.Frame(left); btn.pack(fill="x", pady=(8, 0))
        for k, lbl in (("fn", "FN"), ("pwr", "PWR")):
            ttk.Button(btn, text=lbl, command=lambda k=k: self.ctl("button", k=k, l=0)).pack(side="left")
            ttk.Button(btn, text=lbl + " long", command=lambda k=k: self.ctl("button", k=k, l=1)).pack(side="left", padx=(2, 10))
        ttk.Button(btn, text="Open live control (browser)", command=self.open_companion).pack(side="left", padx=(12, 0))
        right = ttk.Frame(f); right.pack(side="left", fill="y", padx=(8, 16), pady=12)
        ttk.Label(right, text="screen", style="Dim.TLabel").pack(anchor="w")
        self.shot_lbl = tk.Label(right, bg="#000", width=205, height=251)
        self.shot_lbl.pack()
        sb = ttk.Frame(right); sb.pack(fill="x", pady=6)
        ttk.Button(sb, text="Live view", command=self.open_live).pack(side="left")
        ttk.Button(sb, text="Screenshot", command=self.screenshot).pack(side="left", padx=4)
        ttk.Button(sb, text="Save…", command=self.save_shot).pack(side="left")
        self.state_var = tk.StringVar(value="")
        ttk.Label(right, textvariable=self.state_var, style="Dim.TLabel", wraplength=220, justify="left").pack(anchor="w")

    def _slider(self, which, v):
        now = time.time()
        if now - self._slider_t.get(which, 0) < 0.15 or not self.bridge:
            return
        self._slider_t[which] = now
        self.ctl(which, v=int(float(v)), quiet=True)

    def ctl(self, action, quiet=False, **args):
        if not self.bridge:
            self.set_status("connect first"); return
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
            self._shot_png = png
            self.shot_img = tk.PhotoImage(data=png)
            self.shot_lbl.configure(image=self.shot_img, width=w, height=h)
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
            self.set_status("connect first"); return
        if self.live and self.live.winfo_exists():
            self.live.lift(); return
        self.live = LiveView(self)

    def open_companion(self):
        messagebox.showinfo(APP_NAME, "The live-control page (touch, screen mirror, casting) is served by the watch itself over WiFi:\n\n1. On the watch: System › Companion › Start\n2. Join the watch's network from this computer\n3. The page opens at %s" % COMPANION_URL)
        webbrowser.open(COMPANION_URL)

    # ---- Log --------------------------------------------------------------------------------------
    def _build_log(self):
        f = self.tab_log
        bar = ttk.Frame(f); bar.pack(fill="x", padx=16, pady=(12, 4))
        ttk.Button(bar, text="Fetch stored log", command=self.fetch_log).pack(side="left")
        ttk.Button(bar, text="Clear", command=lambda: self.log_txt.delete("1.0", "end")).pack(side="left", padx=4)
        self.autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="follow", variable=self.autoscroll).pack(side="left", padx=8)
        self.log_txt = scrolledtext.ScrolledText(f, bg=PANEL, fg=INK, insertbackground=INK, font=("Consolas", 9), relief="flat")
        self.log_txt.pack(fill="both", expand=True, padx=16, pady=(0, 10))

    def fetch_log(self):
        self.run_bridge(lambda b: b.log_tail(4096), lambda t: (self.log_txt.insert("end", "---- stored log ----\n" + t + "\n"), self.log_txt.see("end")), "fetching the log")

    # ---- connection + workers -----------------------------------------------------------------------
    def refresh_ports(self):
        ports = nbridge.find_ports()
        items = ["%s  %s" % (d, desc) for d, desc in ports]
        self.port_box["values"] = items
        if items and not self.port_var.get():
            self.port_var.set(items[0])

    def toggle_connect(self):
        if self.bridge:
            self.disconnect()
        else:
            self.connect(self.port_var.get().split(" ")[0])

    def connect(self, port, auto=False):
        """Open the port, then prove the firmware's bridge answers (ping). A board that opens but never
        answers is blank or runs a build without the bridge — the app says so and points at Flash."""
        if not port:
            if not auto:
                messagebox.showerror(APP_NAME, "Pick a port first.")
            return
        try:
            b = nbridge.Bridge(port, log_cb=self.log_q.put)
        except Exception as e:
            if not auto:
                self.set_status("open %s failed: %s" % (port, e))
            self._failed_ports[port] = time.time()
            return
        self.set_status("checking %s…" % port)
        def probe():
            try:
                b.ping()
            except Exception:
                b.close()
                self._failed_ports[port] = time.time()
                self.ui_q.put(lambda: self.set_status("a board is attached on %s but no NocSif firmware answers — blank or older build? Flash › Flash new watch" % port))
                return
            self.ui_q.put(lambda: self._connected(b, port))
        threading.Thread(target=probe, daemon=True).start()

    def _connected(self, b, port):
        if self.bridge:                       # a manual connect raced the auto one
            b.close()
            return
        self.bridge = b
        self.conn_btn.configure(text="Disconnect")
        self.set_status("connected on " + port)
        self._log_thread_run = True
        threading.Thread(target=self._log_pump, daemon=True).start()
        self.load_overview()

    def disconnect(self):
        self._log_thread_run = False
        if self.live:
            self.live.close()
        if self.bridge:
            self.bridge.close()
            self.bridge = None
        self.conn_btn.configure(text="Connect")
        self.set_status("not connected — plug a watch in")

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
        """Like qFlipper: a watch that gets plugged in is picked up on its own. Ports that did not answer
        are retried every 20 s (a flash may have fixed them)."""
        try:
            if self.bridge is None and not self.busy:
                ports = [d for d, desc in nbridge.find_ports()]
                items = ["%s  %s" % (d, desc) for d, desc in nbridge.find_ports()]
                if items != list(self.port_box["values"]):
                    self.port_box["values"] = items
                    if items and not self.port_var.get():
                        self.port_var.set(items[0])
                for p in ports[:1]:                       # the best-ranked (ESP32-S3) port only
                    last = self._failed_ports.get(p, 0)
                    if time.time() - last > 20:
                        self.port_var.set(next((i for i in items if i.startswith(p + " ")), p))
                        self.connect(p, auto=True)
        finally:
            self.after(2000, self._autoconnect_tick)

    def _check_app_update(self):
        try:
            rel = updater.latest_app_release()
        except Exception:
            return
        if rel and updater.version_tuple(rel["version"]) > updater.version_tuple(APP_VERSION):
            url = rel.get("url") or RELEASES_URL
            def show():
                self.upd_link.configure(text="update available: v%s" % rel["version"])
                self.upd_link.bind("<Button-1>", lambda e: webbrowser.open(url))
            self.ui_q.put(show)

    def set_status(self, text):
        self.status_var.set(text)

    def run_bridge(self, work, done, label):
        """Run work(bridge) on a thread; done(result) on the UI thread."""
        if not self.bridge:
            self.set_status("connect first"); return
        b = self.bridge
        self.run_bg(lambda: work(b), done, label)

    def run_bg(self, work, done, label):
        if label:
            self.set_status(label + "…")
        def t():
            try:
                res = work()
            except Exception as e:
                self.ui_q.put(lambda: self.set_status("%s failed: %s" % (label or "action", e)))
                self.ui_q.put(lambda: messagebox.showerror(APP_NAME, "%s failed:\n%s" % (label or "action", e)))
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
    """The watch's screen, live, at 2× (410×502 = the panel's own pixels, so mouse → touch is 1:1).
    A polling thread asks the watch for the changed rectangle (`mirror`), decodes the RLE and hands a
    PPM patch to the UI thread, which copies it into the big PhotoImage with Tk's native `copy -zoom`.
    Mouse press / drag / release become touch events that ride the next poll; a press and its release
    are kept ≥ 80 ms apart so LVGL's ~30 ms input poll sees both."""
    W, H, SCALE = 205, 251, 2
    MIN_PRESS_MS = 80

    def __init__(self, app):
        super().__init__(app)
        self.app = app
        self.title("NocSif — live view")
        self.configure(bg=BG)
        self.resizable(False, False)
        self.canvas = tk.Canvas(self, width=self.W * self.SCALE, height=self.H * self.SCALE, bg="#000", highlightthickness=0, cursor="hand2")
        self.canvas.pack(padx=10, pady=(10, 4))
        self.img = tk.PhotoImage(width=self.W * self.SCALE, height=self.H * self.SCALE)
        self.canvas.create_image(0, 0, anchor="nw", image=self.img)
        bar = ttk.Frame(self); bar.pack(fill="x", padx=10, pady=(0, 10))
        for k, lbl in (("fn", "FN"), ("pwr", "PWR")):
            ttk.Button(bar, text=lbl, command=lambda k=k: app.ctl("button", k=k, l=0, quiet=True)).pack(side="left")
            ttk.Button(bar, text=lbl + " long", command=lambda k=k: app.ctl("button", k=k, l=1, quiet=True)).pack(side="left", padx=(2, 8))
        self.cast = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="Cast (blank watch)", variable=self.cast, command=self._cast).pack(side="left", padx=8)
        self.fps_var = tk.StringVar(value="")
        ttk.Label(bar, textvariable=self.fps_var, style="Dim.TLabel").pack(side="right")
        self.canvas.bind("<ButtonPress-1>", lambda e: self._touch(e, 1, press=True))
        self.canvas.bind("<B1-Motion>", lambda e: self._touch(e, 1))
        self.canvas.bind("<ButtonRelease-1>", lambda e: self._touch(e, 0))
        self.events = collections.deque()
        self.lock = threading.Lock()
        self.running = True
        self.protocol("WM_DELETE_WINDOW", self.close)
        threading.Thread(target=self._loop, daemon=True).start()

    def _touch(self, e, pressed, press=False):
        x = max(0, min(self.W * self.SCALE - 1, int(e.x)))
        y = max(0, min(self.H * self.SCALE - 1, int(e.y)))
        with self.lock:
            if pressed and not press and self.events and self.events[-1][2] == 1 and not self.events[-1][3]:
                self.events[-1] = (x, y, 1, False)        # coalesce moves: only the latest matters
            else:
                self.events.append((x, y, pressed, press))

    def _cast(self):
        self.app.ctl("cast", on=1 if self.cast.get() else 0, quiet=True)

    def _next_event(self, last_press_t):
        with self.lock:
            if not self.events:
                return None
            ev = self.events[0]
            if ev[2] == 0 and time.time() - last_press_t < self.MIN_PRESS_MS / 1000.0:
                return None                               # hold the release until the press has landed
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
            try:
                final, raw = b.mirror_poll(seq, full=full, touch=ev[:3] if ev else None)
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
                full = True                               # a fragment went missing — resync
                continue
            seq = int(final["seq"])
            x, y, w, h = int(final["x"]), int(final["y"]), int(final["w"]), int(final["h"])
            ppm = nbridge.ppm_from_rgb565(w, h, raw)
            self.app.ui_q.put(lambda x=x, y=y, ppm=ppm: self._paint(x, y, ppm))
            frames += 1
            now = time.time()
            if now - t0 >= 1.0:
                self.app.ui_q.put(lambda f=frames / (now - t0), n=len(raw): self.fps_var.set("%.0f fps · last %s" % (f, nbridge.human_size(n))))
                frames, t0 = 0, now
        self.app.ui_q.put(lambda: self.fps_var.set("stopped"))

    def _paint(self, x, y, ppm):
        if not self.running:
            return
        try:
            patch = tk.PhotoImage(data=ppm)
            self.img.tk.call(self.img, "copy", patch, "-to", x * self.SCALE, y * self.SCALE, "-zoom", self.SCALE, self.SCALE)
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


def simple_prompt(parent, title, label):
    win = tk.Toplevel(parent); win.title(title); win.configure(bg=BG); win.grab_set()
    ttk.Label(win, text=label).pack(padx=14, pady=(12, 4))
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

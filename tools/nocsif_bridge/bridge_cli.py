r"""
NocSif Desktop Bridge — command line (PLAN §4.15). The same actions as the app, for scripting and tests.

    python bridge_cli.py [--port COM7] ping | version | status | health | test tone|nfc|lora|gnss
    python bridge_cli.py ls [/sd/path] | get <remote> <local> | put <local> <remote> | rm <p> | mkdir <p>
    python bridge_cli.py sd info | sd provision | sd format --yes
    python bridge_cli.py ctl launch <id> | ctl back | ctl home | ctl type "<text>" | ctl key enter|backspace
                         ctl bright <0-255> | ctl vol <0-255> | ctl button fn|pwr [--long] | ctl touch x y s
    python bridge_cli.py menu | state | screenshot out.png | log [n] | usb detached|cdc|hid|msc | reboot
    python bridge_cli.py tail                    (live log until Ctrl-C)
"""
import argparse
import hashlib
import json
import sys
import time

import nbridge


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port (default: the first ESP32-S3 USB-Serial/JTAG port)")
    ap.add_argument("--quiet-log", action="store_true", help="do not echo the watch's log lines")
    ap.add_argument("cmd", nargs="+")
    a = ap.parse_args()

    port = a.port or nbridge.default_port()
    if not port:
        sys.exit("no serial port found — plug the watch in (ports: %s)" % nbridge.find_ports())
    log = None if a.quiet_log else (lambda l: print("   | " + l))
    b = nbridge.Bridge(port, log_cb=log)
    c = a.cmd
    t0 = time.time()
    try:
        if c[0] == "ping":
            print(b.ping())
        elif c[0] == "version":
            print(json.dumps(b.version(), indent=2))
        elif c[0] == "status":
            print(json.dumps(b.status(), indent=2))
        elif c[0] == "health":
            final, checks = b.health()
            for k in checks:
                ok = k.get("ok")
                mark = "PASS" if ok is True else ("FAIL" if ok is False else "skip")
                print("%-5s %-12s %s" % (mark, k["n"], k.get("d", "")))
            print("pass=%d fail=%d skip=%d" % (final.get("pass", 0), final.get("fail", 0), final.get("skip", 0)))
        elif c[0] == "test":
            print(b.test(c[1]))
        elif c[0] == "ls":
            final, ents = b.ls(c[1] if len(c) > 1 else "/sd")
            for e in ents:
                print("  %s %10s  %s" % ("d" if e["d"] else "-", "" if e["d"] else nbridge.human_size(e["s"]), e["n"]))
            print("%s: %d entries%s" % (final.get("path"), final.get("n", 0), " (truncated)" if final.get("trunc") else ""))
        elif c[0] == "get":
            n = b.get(c[1], c[2], progress=lambda d, t: print("\r  %s / %s" % (nbridge.human_size(d), nbridge.human_size(t)), end=""))
            print("\n  %d bytes in %.1f s  sha256 %s" % (n, time.time() - t0, hashlib.sha256(open(c[2], "rb").read()).hexdigest()[:16]))
        elif c[0] == "put":
            n = b.put(c[1], c[2], progress=lambda d, t: print("\r  %s / %s" % (nbridge.human_size(d), nbridge.human_size(t)), end=""))
            print("\n  %d bytes in %.1f s  sha256 %s" % (n, time.time() - t0, hashlib.sha256(open(c[1], "rb").read()).hexdigest()[:16]))
        elif c[0] == "rm":
            print(b.rm(c[1]))
        elif c[0] == "mkdir":
            print(b.mkdir(c[1]))
        elif c[0] == "sd":
            if c[1] == "info":
                i = b.sd_info()
                print("present=%s total=%s free=%s" % (i.get("present"), nbridge.human_size(i.get("total")), nbridge.human_size(i.get("free"))))
            elif c[1] == "provision":
                print(b.sd_provision())
            elif c[1] == "format":
                if "--yes" not in c:
                    sys.exit("refusing: this erases the whole card — add --yes")
                print(b.sd_format())
        elif c[0] == "ctl":
            act = c[1]
            if act == "launch":
                print(b.ctl("launch", app=c[2]))
            elif act in ("back", "home"):
                print(b.ctl(act))
            elif act == "type":
                print(b.ctl("type", text=c[2]))
            elif act == "key":
                print(b.ctl("key", key=c[2]))
            elif act == "bright":
                print(b.ctl("bright", v=int(c[2])))
            elif act == "vol":
                print(b.ctl("vol", v=int(c[2])))
            elif act == "button":
                print(b.ctl("button", k=c[2], l=1 if "--long" in c else 0))
            elif act == "touch":
                print(b.ctl("touch", x=int(c[2]), y=int(c[3]), s=int(c[4])))
            elif act == "cast":
                print(b.ctl("cast", on=int(c[2])))
            else:
                sys.exit("unknown ctl action")
        elif c[0] == "menu":
            print(json.dumps(b.menu(), indent=1)[:4000])
        elif c[0] == "state":
            print(json.dumps(b.state(), indent=2))
        elif c[0] == "screenshot":
            w, h, data = b.screenshot()
            with open(c[1], "wb") as fh:
                fh.write(nbridge.rgb565_to_png(w, h, data))
            print("  %dx%d frame (%d bytes) in %.1f s -> %s" % (w, h, len(data), time.time() - t0, c[1]))
        elif c[0] == "log":
            print(b.log_tail(int(c[1]) if len(c) > 1 else 2048))
        elif c[0] == "usb":
            print(b.usb(c[1]))
        elif c[0] == "reboot":
            print(b.reboot())
        elif c[0] == "mirror-bench":
            secs = float(c[1]) if len(c) > 1 else 10.0
            seq, full, frames, none, byts, t_end = 0, True, 0, 0, 0, time.time() + secs
            biggest = 0
            while time.time() < t_end:
                final, raw = b.mirror_poll(seq, full=full)
                full = False
                if final.get("none"):
                    none += 1
                    time.sleep(0.05)
                    continue
                seq = final["seq"]
                frames += 1
                byts += int(final.get("raw", 0))
                biggest = max(biggest, int(final.get("raw", 0)))
                print("  frame %d: %dx%d at (%d,%d) raw %s%s" % (frames, final["w"], final["h"], final["x"], final["y"],
                                                                 nbridge.human_size(final.get("raw", 0)), " FULL" if final.get("full") else ""))
            print("%d frames + %d no-change polls in %.0f s -> %.1f fps of changes; %s raw total, biggest %s"
                  % (frames, none, secs, frames / secs, nbridge.human_size(byts), nbridge.human_size(biggest)))
        elif c[0] == "tail":
            b.log_cb = lambda l: print(l)
            print("-- live log on %s (Ctrl-C to stop) --" % port)
            while True:
                b.pump_log(1.0)
        else:
            sys.exit("unknown command %s" % c[0])
    except nbridge.BridgeError as e:
        sys.exit("bridge: %s" % e)
    except KeyboardInterrupt:
        pass
    finally:
        b.close()


if __name__ == "__main__":
    main()

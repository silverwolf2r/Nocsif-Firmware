# NocSif Architecture (living doc)

## Runtime base — RESOLVED: Option B (user-confirmed; built through M4)
**Native C++/ESP-IDF core owns all hardware + LVGL + web server + Flipper-format parsers;
embed MicroPython as a component that runs user-facing drop-in scripts.**

This is essentially Bruce's proven shape (native core + a `/scripts` layer) but swapping Bruce's
JavaScript engine for **MicroPython**, which is the one change that buys Python affinity.
Backed by a 7-agent research + adversarial-verify pass (2026-08-06).

### Why not MicroPython-as-platform (Option A)?
Not because MicroPython is "too slow" — that worry is mostly wrong (see below). The decider is
**driver reality**: the CO5300 QSPI AMOLED has a *proven, maintained C/ESP-IDF + LVGL driver*
(`kodediy/esp_lcd_co5300`) but **no tested MicroPython+LVGL driver**. Option B reuses solved
work; Option A would mean first-of-kind CO5300 bring-up in MicroPython on our highest-visibility
deliverable (the UI). Option A stays a viable runner-up only if Python-everywhere dev velocity
is judged worth that schedule risk.

### The C++ / Python boundary
| Native C++/ESP-IDF core (compile once) | MicroPython drop-in layer (extend, no reflash) |
|---|---|
| CO5300 display + LVGL (core-2, partial buffers) | capability recipes, sequencing, UI flows in script |
| CST9217 touch, AXP2101 PMU, XL9555 gating | `.sub`/`.nfc`/DuckyScript orchestration |
| SX1262 / ST25R3916 / USB+BLE HID drivers | reuse of curated PC Python logic |
| WiFi web server + USB-MSC file drop | new importers dropped in as `.py`, no reflash |
| Flipper file-format **parsers** | glue calling native bindings |

## Is MicroPython "too heavy / slow"? (verdict)
- **Too heavy (fit)? No.** ~1.6–2 MB image on 16 MB flash; heap lives in PSRAM. Must build the
  **octal-SPIRAM S3 variant** or PSRAM silently isn't exposed.
- **Too slow for logic/UI? No.** Parsing/menus/callbacks run at ms timescales. LVGL FPS is set by
  its C render engine + QSPI bus bandwidth — identical in C or Python. Real MicroPython risks are
  **GC jitter + long-run UI degradation**, not raw speed.
- **Too slow for RF/NFC timing? No.** The µs/ns deadlines live in **silicon** (SX1262 modem,
  ST25R3916 framing engine, BLE controller), not CPU loops.

## Honest corrections from the adversarial pass (change expectations, not the destination)
1. **"Python parity" is smaller than it sounds.** MicroPython ≠ CPython: no numpy/cryptography/
   pyserial/requests/C-extensions, partial asyncio, trimmed stdlib. What ports = **pure-logic
   parsing/orchestration**, a curated subset. Don't expect PC scripts to run unchanged.
2. **Goals "import Flipper files" + "extend without reflash" need ZERO interpreter** — they're
   data-driven, parsed by native code. Only "reuse my Python" needs the VM. Keep those separate;
   make embedding MicroPython a **gated spike**, not an early commitment.
3. **NFC is from-scratch driver work, not "just wrap a lib."** The pure-Python ST25R3916 libs are
   high-level I²C UID readers — no card emulation / low-level framing needed for `.nfc` parity.
4. **USB HID keyboard: native USB HID beats BLE HID.** ESP32-S3 native USB can be a composite **MSC+HID**
   device (same port as the file drop). Wired HID = no pairing prompt, works on locked/hardened
   hosts. Use it as the first capability; BLE HID is the flakier fallback.
5. **Sub-GHz is worse than "no OOK TX":** the SX1262 also **cannot do raw RF capture** — it only
   demodulates a pre-configured LoRa/FSK modem. Blind capture-and-replay of unknown remotes is
   out; only the in-band FSK subset of `.sub` is replayable. Real OOK `.sub` parity needs an
   external CC1101 — **physically hard on a sealed watch with no GPIO header**, so treat true OOK
   replay as likely out-of-scope for the stock form factor.
6. **USB-MSC concurrency footgun:** exposing one filesystem to host (MSC) *and* firmware at once
   corrupts FAT. Prefer **SD-over-MSC** (if there's a slot) or single-owner-at-a-time.

## Fantasi (the user's reference) — what it actually is
`github.com/soeinova/Fantasi` (GPL-3.0). Corrections: (a) it **does not run on the T-Watch Ultra
yet** — ESP32-S3 is "Coming soon"; working targets are Flipper-family MCUs (Flipper Zero, Kiisu,
Chameleon Ultra, Proxmark3). (b) It is **not** a `.sub`/`.nfc` importer or a scripting runtime —
it's C/FreeRTOS + a **native relocatable-ELF app loader** (compiled apps dropped on device, run
with `launch`, no reflash — like Flipper FAPs). No Python, no Flipper parser.
- **Steal:** its **synthetic-FAT-over-USB-MSC** file drop — the watch mounts on a PC like a normal
  USB stick; drag files in, zero tooling. *This* is its "no-fuss import" reputation.
- **Reject:** native-ELF-per-architecture apps — the opposite of source portability / Python reuse.

## Layered model (unchanged)
L0 drivers (LilyGoLib-derived) · L1 capability HAL · L2 runtime + importers · L3 LVGL launcher/UX
(star motif) · L4 WiFi web UI companion · L5 on-watch browser (v2/v3).

## Milestone chain (Option B, risk-first) — M0–M4 shipped
The original sequencing retired the hardest risks first, and **M0–M4 are built and verified on-device**:
board bring-up + power gating (M0) → CO5300 + LVGL + CST9217 touch (M1 — hardest driver first) → SD-over-MSC
file drop (M2) → native composite **USB MSC + HID + DuckyScript** (M3–M4). The **MicroPython decision gate**
(embed the VM only after porting one real PC script to measure how much survives) was deliberately *not* forced
early — it's deferred to the platform-layers phase (**L2**). The current milestone order lives in
`docs/PLAN.md` §4 (which renumbers past M4 into the radio suites).

## Reference material (harvest, don't ship wholesale)
- **Bruce** (pr3y/Bruce) — proven native-core + script-layer shape; capability logic + Flipper parsers.
- **LilyGoLib** + `kodediy/esp_lcd_co5300` — CO5300/CST9217/XL9555/AXP2101/SX1262 drivers.

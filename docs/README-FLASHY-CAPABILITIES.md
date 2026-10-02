# Flashy capabilities — README candidate

Punchy, demo-worthy bullets for the README's "Current capabilities" section. Each is real today
unless marked, and the caveats are kept on purpose — the honest version survives a live demo, the
overclaim doesn't.

- **RF "tripwire" on a band near you.** Sub-GHz **Signal Alerts**: name a frequency window, set a
  sensitivity, arm "Background watch," and the wrist buzzes (Alert Center) the moment RF energy
  crosses the threshold in that window — with a rough distance estimate. The band classifier labels
  ranges ("VHF public-safety / weather," "FRS/GMRS," "315/433/868/915 ISM," "700 MHz public-safety /
  LTE," "cellular," "paging"…). *Caveat:* the antenna is matched for 915 MHz, so VHF/UHF
  public-safety bands read heavily attenuated — you'll catch **strong, nearby** bursts, not weak /
  distant ones, and it detects RF **presence**, not decoded voice. Best as "something just keyed up
  in this band right next to me" (a 433 MHz remote, a nearby 915 LoRa node, a strong local TX).
- **OpenSesame on your wrist.** A de Bruijn B(2,n) OOK brute-force sweeps every fixed-code
  combination so a tolerant static-code receiver in range sees its code — for your own garage / gate
  opener. Static-code only (useless vs rolling codes), bounded + ISM-gated.
- **Synthesize & transmit a known fixed-code remote** — Princeton/PT2262, CAME, Nice FLO,
  Holtek/HS2XX encoder waveforms from a key you supply, or replay a captured `.sub`. A clean "my
  watch opens my gate" trick (your gear only).
- **Flipper-compatible `.sub` workflow.** Capture FSK/LoRa packets, save as a Flipper `.sub`, carry
  it on the SD card, trade files with a Flipper, replay later. (Packet capture, not raw-OOK — know
  the protocol / syncword.)
- **Mini spectrum analyzer in your pocket.** A 52-bin max-hold band survey with a noise floor and
  auto-detected signals, re-spannable 150–960 MHz — then long-press a signal to **direction-find**
  it.
- **Cross-radio "hot / cold" hunt.** Pin a BLE tracker, a Wi-Fi AP, or a LoRa signal and walk it
  down with a compass-needle bearing dial (no magnetometer) and a rising "getting warmer" beep —
  eyes-free. Find a hidden AirTag, a rogue AP, or a chirping LoRa node.
- **Anti-stalker wrist alert.** Passive scan flags Apple Find My / AirTag, Tile and Samsung SmartTag
  that have been **following you over time** and buzzes the Tracker alert (allowlist for your own
  tags).
- **Drone / Remote-ID spotter.** Decode OpenDroneID broadcasts to show a nearby drone's ID, position
  and vector — **and the operator's location**.
- **BLE pop-up storm (lab flex).** Flood Apple / Android / Windows / Samsung proximity pop-ups
  (AirPods setup, Fast Pair, Swift Pair, Galaxy buds/watch) with a fresh random MAC each cycle — an
  "advertisement-resilience test." Optional mode bonds whoever taps a pop-up.
- **Wrist travel-router with a conscience.** Share a hotspot to friends through the watch with a
  **Pi-hole DNS filter** and your own **WireGuard** tunnel on the forwarded traffic — a pocket secure
  gateway. It can even relay a hotel captive-portal to your phone to sign in.
- **BadUSB that fits on a watch.** DuckyScript over USB **or** BLE HID (US/GB/DE), macros from
  `/sd/nocsif/ducky`, plus a built-in "Grab Wi-Fi from PC" payload — plug in (or pair) and it types.
  (Your own machines.)
- **Boot a laptop off your watch.** Download a Tails / Linux `.iso` straight to the SD (resumable),
  then present the watch to a host as a read-only bootable USB disk — a wearable boot stick.
- **Wardrive from your wrist.** GPS + Wi-Fi monitor logging to WiGLE-1.4 CSV with an on-watch map,
  importable to wigle.net.
- **Catch a WPA handshake, crack later.** Deauth-to-reconnect (own net) → passive 4-way / PMKID
  capture → one-tap hashcat-22000 export to SD. Legacy WEP networks can be recovered on-device
  (Klein/PTW/FMS) and auto-rejoined.
- **Live Wireshark over USB.** Stream monitor-mode frames to your PC in real time via the bundled
  extcap (no "pull the pcap off the card later").
- **Phone-as-remote / watch-as-remote.** The companion web UI mirrors the screen and takes touch
  from a browser — demo the watch on a projector, or remote-control a second NocSif. (Open AP;
  owner-only.)
- **Card-skimmer sniff at the pump.** A passive BLE scan flags the cheap serial modules skimmers use.
  (Heuristic — treat hits as "worth a look," not proof.)
- **LAN recon that looks like nmap.** Sweep the subnet, fingerprint a host's OS / type (router /
  printer / NAS / camera), grab banners, pull a TLS cert, traceroute, and drop into netcat — all
  from the wrist, on a network you're authorized to test.
- **Shopping-cart soundboard.** Play the cart-wheel-release tones (and any WAV / MP3) through the
  speaker.
- **Unexpected flex: a guitar / piano tuner.** The mic's YIN pitch detector powers an instrument
  tuner and a chromatic piano tuner — a "wait, your hacking watch tunes my guitar?" moment.
- **Off-grid two-watch text.** LoRa P2P messaging between NocSif watches (plaintext, no
  infrastructure).
- **Signal Blast bench source.** A bounded / swept CW carrier for antenna VSWR tuning and
  receiver-resilience testing of your own gear.

# Future state
Features planned for future releases of NocSif. 
Every risk-first milestone is shipped; what follows is capability expansion. This is feature want
menu, not a hard sequence — items are built one work-package per branch.


# Planned Features 


### WiFi
--------------

Host discovery on the joined LAN (ping sweep + ARP-cache MAC + mDNS/NBNS/SSDP names + OUI vendor).

TCP port scan + banner on a chosen host (connect-scan, bounded socket pool). 

Adopt a profile (clone MAC + hostname, own gear) — primitives already shipped, just needs the "tap a discovered host → adopt" flow.

APSTA NAT router (share WiFi / route your phone's internet) — (BLE off?, on-device RAM validation, AP locked to upstream channel).

DNS blocklist / Pi-hole on the watch's own AP (extends the existing :53 captive-portal responder into forward-or-block).

WireGuard VPN: tunnel the watch's own traffic (vendor esp_wireguard, import .conf from /sd), route the AP clients through it, auto-drop the tunnel on home WiFi (via the Governor / BSSID-places).
most of the above should be able to work in conjunction with each other

finish rogue-AP / evil-twin, add responder 

management-frame-flood / deauth-flood detectors

flag nearby tools (Flipper-BLE / pwnagotchi / deauther / Pineapple)

a cross-radio (WiFi+BLE) presence census

an audible/visual radio-event alerter

camera-glasses (Ray-Ban Meta) detection

Flock ALPR-camera detect-and-direction-find (now ungated by BLE + Signal Hunt + GNSS).

ESP-NOW device-to-device link

DNS-sinkhole / walled-garden on the own AP 

DIAL / Chromecast control

wireless-HID-over-WiFi console with an auditable keystroke witness log

USB-Ethernet emulation.

DNS tunneling


--------------
### BLE expansion
--------------

BLE GATT write / subscribe-notify / characteristic testing; custom GATT-server emulation 

HID mouse / media / gamepad; 

decoders for Continuity / Handoff / AirDrop and Fast Pair / Swift Pair

card-skimmer detection

anti-stalking "a tracker is following me" alert

BLE Advertisement Resilience Test — controlled adverts (volume + malformed) against a device you own or are authorized to assess, a controlled-scope counterpart to advertisement-flood detection (a target under test, never saturation of bystander devices).
...BLE Spam


--------------
### Off-grid & LoRa
--------------

mesh telemetry

store-and-forward 

traceroute

MQTT internet gateway

MeshCore and Reticulum/RNode stacks

repeater/router node

a narrowband in-band FSK .sub subset

cross-band BLE/web→LoRa bridge

GNSS-time-synced collision-avoiding mesh slots

a Sub-GHz RF Carrier / Resilience Test — a controlled continuous-carrier (CW) output for antenna & matching characterization (VSWR / tuning) and interference-resilience testing of a receiver you own, ideally in a shielded / controlled environment (a test primitive for your own equipment, not a jammer technically?)


--------------
### General
--------------

image / GIF viewer, 

QR display, 

bubble level / inclinometer 

audio tools (theremin, data-over-sound acoustic modem, dB/SPL meter, live spectrum analyzer + tuner).

USB host with peripheral class drivers, U2F/FIDO CTAP over HID, MicroPython drop-in script layer, Flipper-format parsers + an app/script manager

GPX route navigation & track-back, localization / i18n

head-coupled 3D idle scene

the USB-C "Backpack" co-processor

# Things that need to be fixed

## Companion web remote 
  Extremely laggy and the live control is terrible to use due to lag. Additionally, the live control should pull up the phones keyboard when tapping into something that uses a keyboard.

## Carts Files
  The audio doesnt seem to work, more research needed.

## Desktop App
  
  Should be able to search for updated desktop app to pull down and update itself from github. When I make changes to the app everyone should be able to update.
  
  The default files should just be the completely necessary folders/files. Currently the following should be remove 
  
  folder ducky 
  
  autorun.ico 
  
  autorun.inf
  
  nocsif_test.txt

  nocsif -> firmware.bin (the firmware folder contains the real one that gets changed.
  
  Control just shows a black screen.

## BLE phone companion 
  Dismissing notifications on watch does not dismiss on phone.

## Control center 
  Needs to match/light up appropriately i.e. I dont want it lighting up for everything Wifi should only be lit when actively doing something like monitor mode and bluetooth should follow suit. 
  (Currently wifi is correct but bluetooth is not) the other buttons should also correspond correctly like dnd and airplane mode (possibly private bool drift).

## Voice memo 
  Volume is too low when replaying a voice memo.


# Things that need to be tested

USB composite mode switching under load · PC · cycle Detached/CDC/HID/MSC with WiFi+BLE up — no display hang; File Share mounts SD. (Historical DMA-hang class.)

BLE GATT Explore · any BLE device · services/characteristics enumerate and reads return values.

WiFi anomaly detectors · generate the condition · deauth/disassoc-rate + duplicate-SSID/evil-twin detectors fire.

Band Survey / Channel Activity false-positives · leave it on an idle band a long while · confirm it does not slowly "detect" noise. (I flagged the hit-count max-hold drift — this is also a fix candidate.)

Desktop app hardware check and flash menu


## Things I Cant Test Cause im lazy and poor
BLE Drone Detection live decode · a Remote-ID broadcaster (drone or ODID sim) · Basic ID + drone location/vector + operator position decode.

LoRa P2P RX / round-trip · a 2nd LoRa node @915 MHz · send from one, receive on the other; inbox populates.


## Not Testing cause im just lazy
Wardrive → WiGLE CSV · outdoors, walk/drive · valid WiGLE-1.4 CSV with geotagged APs. (Now testable — GPS confirmed.)

WiFi live-PCAP → Wireshark · PC + Wireshark + tools/extcap/ · monitor-mode frames stream live into Wireshark.

Type-scale everywhere (§4.13) · nothing extra · flip Compact/Default/Large and confirm all text reflows. (AI found ~84 hand-bound font faces in ui.c that likely won't)

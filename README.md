# Pico-W-NWII-Example

Minimal Raspberry Pi Pico W firmware that pairs with a Nintendo Wii as a Wii Remote, using
[NWII-LIB-HID](external/NWII-LIB-HID) on top of BTstack. It mirrors Pico-W-NS-Example: the library
owns the Wii Remote protocol, and this project only wires up Bluetooth, pins and storage.

## Pins (active low, internal pull-ups)

| Pin | Function |
| --- | --- |
| GP1 | Hold at power-up: forget the saved Wii and wait for SYNC |
| GP0 | Hold: pin the pointer to the centre (otherwise it traces a demo circle) |
| GP14 / GP15 | A / B |
| GP16 / GP17 | 1 / 2 |
| GP18 / GP19 / GP20 | + / - / Home |
| GP21 | Tap: cycle extension (none → Nunchuk → Classic Controller Pro) |

## Use

1. Build and flash (`Pico-W-NWII-Example.uf2`).
2. Open the USB serial port to watch the log. `NWII_EXAMPLE_HCI_DUMP` in CMakeLists.txt also
   prints every HCI packet.
3. Press the red SYNC button on the Wii. The Pico answers the PIN request, the Wii opens the HID
   channels, and the address is saved so later boots reconnect on their own.

## Free for everyone

Released into the public domain under [The Unlicense](LICENSE). Written with the help of
**Claude Opus** (Anthropic).

Not affiliated with or endorsed by Nintendo. Nintendo and Wii are trademarks of their respective owners.

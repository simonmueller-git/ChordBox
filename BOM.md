# Bill of materials

One ChordBox. These are the parts in the build this firmware was written for. No prices or shop links:
the generic modules below are sold under many names, so match them by the description and pinout.

| Qty | Part | What to look for | Used for |
|---|---|---|---|
| 1 | ESP32-S3 dev board, **N16R8** | ESP32-S3-DevKitC-1 style with WROOM-1, 16 MB flash, 8 MB octal PSRAM, two USB-C ports | The brain. Native USB-C is the MIDI port |
| 1 | 2.0" screen + knob module | ST7789, 320×240, EC11 push knob and a KEY0 button on one 12-pin header (`GND VDD SCL SDA RES DC CS BLK TRA TRB PSH K0`), **3.3 V only**. Ours is sold as Estardyn | Left side: chords, MAIN knob, SHIFT |
| 1 | I2C OLED with 4 buttons | 128×64, SSD1306 or SH1106, on an 8-pin header (`GND VCC SCL SDA K4 K3 K2 K1`) | Middle: the bass screen and its 4 buttons |
| 1 | Rotary encoder with push, on a breakout | 5-pin `GND S1 S2 KEY 5V` | Middle knob: bass settings |
| 1 | 4×4 hard-button keypad | 8 signal pins plus an NC pin at each end. Same pinout as Adafruit 3844 (`NC C1 C2 C3 C4 R1 R2 R3 R4 NC`). Other wiring works too: KEYPAD SETUP learns it | The 16 pads |
| about 30 | Dupont jumper wires | Female ends for the board's headers, or solder directly | Wiring |
| 1 | USB-C cable | Data-capable | Board's native port to the Ableton Move (MIDI and power) |
| 1 | USB-C cable | Data-capable | Board's COM port to a computer, for flashing and the console |

Optional: hot glue or similar, to keep Dupont connectors from working loose. If your screen module's K0
button seems dead, check that wire first.

See [docs/WIRING.md](docs/WIRING.md) for the connections.

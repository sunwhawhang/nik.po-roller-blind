# Smart Roller Blind Motor

Modified fork of [nik-po’s original project](https://github.com/nik-po/nik.po-roller-blind).

A DIY motorized roller blind unit built around an ESP32-C3, a TB6612FNG
motor driver, and a 12V worm gear motor — running [ESPHome](https://esphome.io)
and integrating natively with [Home Assistant](https://www.home-assistant.io/).
No cloud, no subscription, no app to sign into. It keeps working if your
internet goes down.

## What it does

- **Native Home Assistant integration** — shows up as a `cover` entity with
  full position control
- **Built-in web interface** — control it from any browser on your network,
  Home Assistant or not
- **Guided calibration** — jog to the physical ends and mark them. A
  single trip is enough. Nudge and re-mark either end to adjust it.
- **Physical buttons** — tap or hold to jog before calibration; press to
  move or stop once calibrated.
- **Estimated travel limits** — normal moves and jogs stop slightly inside
  the marks. The controller estimates position without sensors.
- **Reverse switches for motor and buttons** — wired something backwards?
  Flip a toggle instead of re-soldering
- **OTA updates** — flash once over USB, update over WiFi from then on
- **Multiple blinds, one master** — pressing the physical buttons on the
  device fires `esphome.roller_master_up` / `_down` / `_stop` events every
  time, whether or not anything is listening. Add a Home Assistant
  automation that triggers on those events and calls `cover.open_cover` /
  `cover.close_cover` / `cover.stop_cover` on your other blinds, and one
  set of buttons drives the whole group

## Hardware

| Part | Approx. price |
|---|---|
| ESP32-C3 SuperMini | $2.00 |
| TB6612FNG motor driver board | $1.50 |
| JGY-370 12V 40RPM worm gear motor | $8.00 |
| Mini-360 buck converter | $0.50 |
| DC jack, 5.5 × 2.1 mm | $0.50 |
| 2× tactile push buttons | $0.20 |
| 12V power adapter, 1A+ | $3.00 |
| Silicone wire 26AWG + heat shrink | $2.00 |
| **Total, per blind** | **under $20.00** |

This fork keeps the original hardware and wiring. The web setup learns
travel in each direction from the motor drive during calibration.

You'll also need a printed enclosure to hold it all together. The STL,
photos, and an illustrated step-by-step assembly PDF are on
**[the MakerWorld page](https://makerworld.com/en/models/3292116-smart-roller-blind-motor-esp32-home-assistant#profileId-3735590)** — this repo covers the
electronics and firmware, that page covers the print and the physical
build.

## Getting started

Read **[`INSTRUCTIONS.txt`](INSTRUCTIONS.txt)** — it's the full build guide:
flashing, generating your own security keys, soldering, WiFi setup, and
calibration, written for someone who has never flashed a microcontroller
before.

Use this fork's source tree. Keep `firmware/roller_motion.h` and
`firmware/www/` beside the YAML. Upstream release ZIPs do not include this
fork's calibration.

The wiring diagram alone lives at [`hardware/wiring/wiring-diagram.svg`](hardware/wiring/wiring-diagram.svg) —
open it in any browser.

## Repository layout

```
INSTRUCTIONS.txt           the full build guide
firmware/
  roller-motor-dist.yaml   ESPHome config — no WiFi baked in (set up from
                           your phone on first boot), placeholder security
                           keys you generate yourself (required)
  www/                     source for the built-in web interface
hardware/wiring/
  wiring-diagram.svg       soldering diagram
```

## Calibrate in the web interface

Mark either end in Settings, jog to the opposite end and mark it. Up/Down
unlock after those two marks. Use Nudge and re-mark an end to adjust it
without starting over. The other end remains the reference.

A single trip measures motor drive in one direction and estimates the other.
For longer blinds, Settings suggests an optional return to the first end.
Jog back and mark it again to measure the other direction independently.
The estimated direction has a larger stopping margin until then. After power loss
during a move, mark a known physical endpoint to resume normal motion.

The estimate can drift. Start with supervised moves, and use physical limit
sensors if you need a hard cutoff.

### Validate the motion model

```sh
clang++ -std=c++17 -I firmware tests/roller_motion_test.cpp -o /tmp/roller-motion-test
/tmp/roller-motion-test
```

## Firmware note

`firmware/roller-motor-dist.yaml` ships with placeholder values for the API
encryption key and OTA password on purpose — building this project means
generating your own (the instructions show you how) rather than sharing
one key across every blind built from this repo.

## License

This project is licensed under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) —
you're free to share and adapt it, as long as you credit the source, don't
use it commercially, and share your own version under the same license.
Full legal text: <https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode>.

If you build one, I'd love to see it.

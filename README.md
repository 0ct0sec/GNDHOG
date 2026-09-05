# GNDHOG ZERO — Betaflight & Meshtastic for Cardputer Zero

![GNDHOG ZERO mascot](store/icon.png)

By **0ct0**. A pocket terminal for the flight line and the walk into the hedge.

Plug a **Betaflight flight controller** into your M5Stack Cardputer Zero: run
CLI commands, check arming blockers, and back up or restore a tune. Plug in a
**Meshtastic radio** instead: chat, read heard nodes, and keep the conversation
on the device. Add GNSS for range, bearing, and saved places.

Your laptop can stay home. The tree that ate your quad has declined to comment.

## Quick start

1. **Remove the propellers before working on a flight controller.** Keep an
   antenna on the VTX and provide airflow when it is powered.
2. Install the ARM64 `.deb` from [Releases](https://github.com/0ct0sec/GNDHOG/releases)
   on your Cardputer Zero, then launch **GNDHOG ZERO** from APPLaunch. Store
   publication is pending first-release review.
3. Set the Cardputer's **Host/Slave switch to HOST**. Connect the FC or a radio
   already running Meshtastic firmware to **USB-A** with a data cable.
4. Select its serial port and press **Enter**. **M** switches the proposed
   Betaflight/Meshtastic protocol if the picker guessed wrong.
5. **Esc** opens the menu and backs out. **Fn+1** opens help.

To install a downloaded package from a terminal on the Cardputer:

```sh
sudo apt install --no-install-recommends ./bfcli_0.1.0_arm64.deb
```

This app is for **Cardputer Zero running Linux**. The ESP32 Cardputer and
Cardputer ADV need different software. A connector fitting is not a compatibility test.

## What it does

| On the flight line | On the mesh |
|--------------------|-------------|
| Betaflight CLI, history, and command completion | Direct and broadcast text conversations |
| Field check with raw evidence and saved reports | Heard-node list, message state, and local transcripts |
| Configurator-style backup files and paced restore | Quick messages for flying, recovery, and the trip home |
| Confirmations for motor and erase commands | Position sharing with confirmation |
| Supported VTX pit-mode guard and MCU temperature warnings | GNSS range/bearing, compass, and saved marks |

It runs locally. No account, phone, or internet connection is needed to use it.
You still need the attached hardware: the Cardputer does not grow a LoRa radio
because the menu contains the word mesh.

## Keys

| Key | Betaflight terminal |
|-----|---------------------|
| `Enter` | send the command |
| `Tab` | complete a command or parameter |
| `Fn+F` / `Fn+X` | previous / next command |
| `Fn+Z` / `Fn+C` | move the cursor left / right |
| `Fn+L` / `Fn+M` | scroll output up / down |
| `Fn+2` | field check |
| `Fn+5` / `Fn+6` | create a backup / browse backups |
| `Fn+8` | send `save` |
| `Esc` | menu / back / exit |

| Key | Meshtastic |
|-----|------------|
| `Enter` | open a conversation / send the typed message |
| `Tab` in a conversation | quick messages |
| `F` / `Fn+4` | locate the selected node or chat peer |
| `M` on the node list | saved marks |
| `M` in Locate | save that node's last position |
| `G` / `Fn+6` | GNSS status |
| `P` / `Fn+5` | share your position, after confirmation |
| `I` / `Fn+2` | radio information |
| `Fn+0` | disconnect |

The keyboard follows the Cardputer Zero's printed layers. The
[field guide](docs/FIELD_GUIDE.md#keys) covers editing shortcuts and the full
Fn rack. HUD sounds and volume live in **Menu → Sound & display**.

## Connecting a flight controller

USB-A is the normal route. A UART connection is also possible; use the
correct signal voltage, wiring, and baud rate for your hardware. See the
[connection guide](docs/FIELD_GUIDE.md#connecting-a-flight-controller).

For a supported SmartAudio or Tramp VTX, GNDHOG can offer a **bench VTX guard**.
It asks before requesting pit mode and checks the reported state. Unsupported
hardware stays unchanged. Readback is not a measurement of RF output.

While the CLI is idle, temperature checks can warn at **70 °C** and escalate at
**80 °C** when Betaflight reports MCU core temperature. That is the FC's reading,
not a VTX temperature measurement. If it is overheating, unplug USB and battery
power. **Disconnecting the serial link does not remove USB power.**

## Field check

Press **Fn+2** for a snapshot of arming blockers, sensors, battery, and scheduler
information. **V** shows the raw output, **R** refreshes it, and **S** saves a
report. Expected `CLI` and `MSP` blockers stay distinguishable from faults.
Missing readings stay unknown. Optimism is not a sensor.

The check queries `status`, `tasks`, and `version` without changing settings or
writing FC flash. Betaflight's `tasks` command resets its temporary maximum-time
statistics after printing them. This is diagnostic evidence, not a clearance
to fly; it cannot inspect your solder joints, motor direction, or failsafe.

## Backups

Use **Fn+5** to create a backup and **Fn+6** to browse saved ones. Files use
Betaflight Configurator-style names so you can copy them between devices.
Restore sends commands one at a time and waits for each prompt. A different
`board_name` is flagged before you proceed.

**A restore is not persistent until you run `save`.** Review the result before
saving. A backup from another target is not a universal tune with a creative filename.

Backups, mesh transcripts, marks, and diagnostic reports stay under
`~/.local/share/bfcli/` by default. Keep another copy of anything you cannot
replace. Deleting a conversation removes only this device's copy.

## Meshtastic

Use a separate radio running Meshtastic firmware, such as a suitably configured
M5Stack Unit C6L. Configure its region and channel before heading out; GNDHOG
refuses sends when the radio reports no region or transmit disabled.

The first row is the broadcast channel. Other rows are nodes the radio has
heard, with the age of that observation. **Heard is not online.** A quiet relay
and a flat battery are remarkably committed to the same user experience.

Direct messages show pending, routing acknowledgement, or failure. A routing
ACK is not a read receipt. Broadcasts have no recipient acknowledgement.
Transcripts reload on the next launch; an unresolved message stays unresolved.

Ordinary chat sends when you press Enter. Position sharing and SOS ask first.
Auto-share is off by default, lasts only for the current session, and stops
when the radio disconnects. The radio's own configured mesh traffic is separate
from what GNDHOG requests.

## Finding things: GNSS, Locate, and marks

Connect a supported NMEA GNSS receiver through the configured UART. The
**Cap LoRa-1262 GPS receiver** or a **GPS Unit on Grove** can supply your position;
those connections share a UART, so use one receiver at a time. Configure power
and UART routing in the launcher as needed. GNDHOG does not drive the cap's
SX1262 radio; Meshtastic still needs its own radio.

**Locate** shows range and bearing to a node's last reported position or a
saved mark. It shows the age of their position separately from when their
radio was heard. A tracker with a stale fix is still a tracker with a stale fix.

For heading while standing still, calibrate and align the compass under
**Menu → Position & GNSS → Compass**, away from magnets and motors. Without a
usable compass, Locate uses GNSS course while moving or shows north-up.
[Receiver setup and compass controls](docs/FIELD_GUIDE.md#finding-things-locate-marks-quick-messages-sos)
are in the field guide.

Save the car as a mark before walking away. Save a tracker's last position
before its battery leaves the conversation. Marks stay on your device and
are never transmitted.

**SOS broadcasts to your mesh channel. It does not contact emergency services
or guarantee delivery or rescue.** Review the shown position and check for replies.

## Safety

- **Props off for FC bench work.** Motor and erase confirmations are software
  guardrails, not a reason to leave propellers fitted.
- Restoring the saved VTX flight state exits CLI, discards unsaved CLI changes,
  and reboots the FC. Read the disconnect prompt.
- Normal USB-A operation cannot cut FC power. An attached battery is a separate
  supply. Advanced EXT thermal-trip behavior is described in the
  [field guide](docs/FIELD_GUIDE.md#connecting-a-flight-controller).
- GNDHOG does not flash firmware or deliberately select the 1200-baud DFU trigger.
- Coordinates and chat transcripts are saved locally. Export or share them deliberately.

## Documentation and development

- [Field guide](docs/FIELD_GUIDE.md): full controls, wiring, GNSS, compass,
  VTX guard, and thermal behavior.
- [Build and installation](docs/DEVELOPMENT.md): host tests, ARM64 build,
  simulator, and package details.
- [Publishing](docs/PUBLISHING.md): store requirements and release checklist.
- [Report a bug](https://github.com/0ct0sec/GNDHOG/issues): include the commit
  shown in **About**, what was connected, and what happened.

## License

[MIT](LICENSE). Built by **0ct0**. Gravity remains proprietary and has no issue tracker.

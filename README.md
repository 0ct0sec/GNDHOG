# GNDHOG ZERO — Betaflight CLI for the M5Stack Cardputer Zero

![GNDHOG ZERO mascot](store/icon.png)

By **0ct0**. Props off. Shell on. The groundhog has the USB cable and has
declined to discuss what happened to the lawn.

GNDHOG ZERO turns the M5Stack Cardputer Zero into a field terminal for
Betaflight. Plug a flight controller into USB-A and the full CLI lands on the
built-in 320×170 screen, with command history, completion, and SD-card backup
and restore. No laptop. No browser. No ceremony involving a folding table and
three adapters that all claim to be data cables.

This is a native ARM64 Linux application. It writes directly to `/dev/fb0`,
reads the TCA8418 keyboard through evdev, and speaks to the FC over a serial
port. No Arduino, ESP-IDF, X11, SDL, LVGL, or background service. One `poll()`
loop at 30 fps runs the whole suspiciously small department.

```text
┌─────────────────────────────────────────────────────┐
│ GNDHOG ZERO  ttyACM0 AIR65 C                  ready │
│ # Betaflight / STM32G47X (G473) 2026.6.0-alpha      │
│ Voltage: 4.12V (1S battery - OK)                    │
│ Arming disable flags: RXLOSS CLI                    │
│ -- CLI ready --                                     │
│ # set gyro_lpf1_static_hz = 0█                      │
│ Tab complete  Fn+F history  Esc menu  Fn+1 help     │
└─────────────────────────────────────────────────────┘
```

## Quick start

1. Remove the propellers. This is step one because fingers remain difficult to
   package as replacement parts.
2. Build the ARM64 binary with `make arm64`, then install it with
   `sudo ./tools/install.sh --add-groups`.
3. Log out and back in if the installer added `video`, `input`, `dialout`, or `audio`.
   Linux does not renegotiate group membership because the operator looked
   sincere.
4. Put the Cardputer Zero **Host/Slave switch in HOST**, then connect the flight
   controller to **USB-A**.
5. Launch **GNDHOG ZERO** from APPLaunch. If the port picker appears, choose the
   CDC-ACM device for the FC, usually `/dev/ttyACM0`.
6. If a controllable VTX is reported, accept the optional bench pit-mode guard.
   GNDHOG verifies the state before it calls the guard active.
7. Press **Fn+2** for a no-config-write field check. Press **Esc** for the menu. Make
   a backup before changing anything expensive, airborne, or both.

## What it does

- **Full CLI terminal.** Run `status`, `get`, `set`, `diff`, `resource`,
  `vtxtable`, or whatever other instruction you intend to regret responsibly.
  Scrollback holds 3000 lines; command history survives the session.
- **Verified bench VTX guard.** Before entering CLI mode, GNDHOG asks a
  Betaflight-reported, ready VTX to enter pit mode and reads the state back.
  Nothing is saved. If the VTX cannot confirm pit mode, GNDHOG requests pit-off
  and verifies that cleanup; if even cleanup is unconfirmed, it keeps the
  reboot-restoration prompt. It never relabels an arbitrary power index as
  “off.” A guarded disconnect can reboot the FC so its saved flight state reloads.
- **Field check, with receipts.** One key captures `status`, `tasks`, and
  `version`, then turns the FC's own evidence into an at-a-glance arming,
  gyro, receiver, battery, I2C, MCU-temperature, and runtime summary. Expected `CLI`/`MSP`
  blockers are separated from faults such as `RXLOSS`, `NOGYRO`, `FAILSAFE`,
  and `LOAD`. The untouched command output remains in the terminal, and an
  optional report can be saved for later. Missing core evidence is marked
  “unknown”; optional unsupported devices are omitted. The groundhog does not
  diagnose by séance.
- **Tab completion.** Commands and `set`/`get` parameter names complete in
  place. About 140 common parameters are built in, then the index learns names
  already seen in `dump` and `diff` output. It does not interrogate the FC
  behind your back. The terminal has boundaries, unlike the average bench.
- **Configurator-shaped backups.** One key runs `diff all` and writes the same
  filename shape used by Betaflight Configurator, so files can move in either
  direction without a customs interview.
- **Prompt-aware restore.** Saved commands go back one line at a time. GNDHOG
  waits for each completed response, shows progress, and counts FC rejections.
  It does not confuse optimism with an acknowledgement byte.
- **A keyboard that can type Betaflight.** The Cardputer has 46 keys and no
  obvious underscore. Betaflight named nearly everything in `snake_case`.
  This administrative conflict has been settled below.
- **Fighter-HUD sound cues.** Short synthesized chirps mark navigation,
  selections, FC link state, completed work, and alarms. GNDHOG targets the
  built-in `ES8388Audio`/`ES8389Audio` ALSA card by name instead of Linux's
  HDMI default, and falls back to silence if that exact device is absent.
- **Offline About screen.** Open **Menu → About GNDHOG ZERO**, or press **A**
  on the port picker, for the mascot, author, source commit, and abbreviated
  crash report. Enter or Escape returns to the previous screen. No FC is
  opened merely to display paperwork.

The visible product name is GNDHOG ZERO. The executable, install paths, data
directory, desktop filename, and `BFCLI_DATA_DIR` override remain `bfcli` so
existing launchers, backups, history, and `config.ini` files keep working.
Renaming every path would be branding. Breaking every path would be branding
with a necktie.

## The keyboard problem, and the fix

Betaflight parameters look like `gyro_lpf1_static_hz`, and assignments need
`=`. Neither character has an obvious home on the 46-key thumb keyboard, so
GNDHOG gives them one:

| Chord | Character |
|-------|-----------|
| **Sym + L** | **`_`** |
| **Sym + 0** | **`=`** |
| Sym + U | `-` |
| Sym + J | `:` |
| Sym + K | `"` |
| Sym + W / E | `{` `}` |
| Sym + V / B | `<` `>` |
| Sym + N / M | `?` <code>&#124;</code> |

The rest of the Sym layer (``[ ] ; ' ` \\ , . / *`` plus a numeric keypad)
follows the keycodes emitted by the v5 overlay.

Ten unusual Sym positions arrive through placeholder keycodes such as `KEY_RO`,
`KEY_MUHENKAN`, and the Japanese IME block. Those codes have no US-layout
character, so GNDHOG supplies configurable defaults. If your overlay disagrees,
the key test settles the argument without requiring a rebuild.

If a character is wrong on your unit:

1. Open **Menu → Keymap & key test** and press it. The screen reports the raw
   scan code, keycode, decoded layer, and resulting character.
2. Add an override to `~/.local/share/bfcli/config.ini`, then restart:

   ```ini
   sym.0x28 = _
   ```

No rebuild is required. The same screen prints the complete Sym layer and
highlights `_` and `=`, because the two smallest characters caused the largest
meeting.

A plain USB keyboard on the hub also works and is decoded from keycodes alone.

## Keys

| Key | Action |
|-----|--------|
| `Tab` | complete a command or parameter |
| `Fn+F` / `Fn+X` | previous / next command in history |
| `Fn+Z` / `Fn+C` | move the cursor left / right |
| `Fn+K` / `Fn+N` | start / end of line |
| `Fn+L` / `Fn+M` | scroll output up / down |
| `Fn+Backspace` | delete forward |
| `Ctrl+U` / `Ctrl+K` | kill to start / end of line |
| `Ctrl+W` | delete the previous word |
| `Ctrl+C` / `Ctrl+L` | clear the line / screen |
| `Esc` | menu, then back, then exit |

Hold `Fn` and tap a number for the quick rack: **1** help, **2** field check,
**3** version, **4** diff, **5** backup, **6** backups, **7** tasks, **8** save,
**9** menu, **0** disconnect.

## HUD sounds

The startup sweep and terse avionics-style cues are generated in memory; there
are no WAV assets to lose or decode. Open the menu to toggle **HUD sounds** or
change **HUD volume** in 10% steps. Both settings persist in the existing
`config.ini`; `--mute` is a launch-wide override the menu cannot undo, and it
does not change the saved choice. `/opt/bfcli/bin/bfcli --sound-test` plays the
bounded startup cue at 70% and exits with a failure if discovery, mixer setup,
PCM output, or drain does not complete.

On Cardputer Zero, HDMI is commonly ALSA card 0 and the built-in speaker codec
is card 1. GNDHOG therefore opens `hw:ES8389Audio,0` (or the earlier
`ES8388Audio` identity) explicitly. The output worker never stalls the UI, caps
the codec playback controls at their 0 dB point, bounds synthesized PCM, and
collapses repeated navigation chirps. Critical temperature and link-loss cues
clear less important queued sounds so the warning is not waiting behind a
thumb-operated sonata.

If the menu reports a silent fallback, check `cat /proc/asound/cards` for the
exact codec, confirm the launcher user belongs to `audio`, and inspect
`/dev/snd/pcmC*D*p`. `--mute` and `sound.enabled = 0` are deliberate silence;
an absent codec or `libasound.so.2` is reported as unavailable rather than
quietly sending the cues to HDMI.

## Connecting a flight controller

Set the **Host/Slave switch to HOST**, then plug the FC into **USB-A**. A normal
Betaflight USB connection appears as a CDC-ACM device such as `/dev/ttyACM0`
(commonly `0483:5740`), and GNDHOG ranks it for automatic connection.

A spare FC UART on the Grove connector works too; select its port and baud on
the connect screen. Grove signal pins are **3.3 V**. The presence of 5 V on the
power pin does not grant the data pins diplomatic immunity from 5 V.

In Slave/device mode the internal hub is disconnected and USB-A is offline.
The FC cannot enumerate through a wire the hardware has removed from the
conversation.

Before sending `#` to enter the CLI, GNDHOG makes one read-only
`MSP_VTX_CONFIG` probe. If Betaflight reports a ready VTX, the app offers pit
mode. Acceptance sends the runtime request and queries the FC again; the guard
is only labelled active when the FC reports pit mode back. Betaflight notes
that some SmartAudio hardware cannot enter pit mode after power-up. Those and
all unsupported arrangements continue to CLI without a fabricated success.
After an unconfirmed request, GNDHOG requests pit-off and verifies the cleanup;
if that readback also fails, disconnect continues to offer a reboot restore.
This protects supported VTX hardware; it is not proof of RF output or a
substitute for an antenna and airflow.

Once CLI is ready, GNDHOG runs an automatic `status` capture for the FC MCU
core temperature, then repeats it every 30 seconds while the CLI is otherwise
idle. The raw responses remain in the terminal. A reported value at or above
Betaflight's default 70 C alarm opens a warning; a later rise to 80 C escalates
to critical. This reading is not a VTX temperature sensor. On integrated AIO
hardware it is useful nearby heat evidence, not a measurement of the
transmitter die. A target that does not report core temperature is sampled
once, marked unavailable, and not polled repeatedly.

The normal USB-A connection remains warning-only: closing its serial device does
**not** remove USB 5 V, and Cardputer Zero does not expose that connector's VBUS
as a switchable launcher rail. If the stack is hot, unplug the FC's USB lead and
any battery supply.

A one-shot **EXT thermal trip** is offered only for a deliberately commissioned
EXT USB adapter. GNDHOG must prove all of these facts from current kernel
readback before the arm prompt appears: the selected FC belongs to the internal
`05e3:0610` hub's USB4 branch, the EXT selector reports USB rather than GPIO,
the switched `ext_5v_out` rail is on, and this process can write it. GNDHOG does
not reroute the EXT pins or energize the rail to manufacture eligibility.

If the operator arms that trip, a fresh 80 C-or-higher MCU sample first writes
an incident under `~/.local/share/bfcli/diagnostics`, then requests EXT 5 V off,
verifies an off readback, and closes serial. The notice states what happened and
why. A failed write/readback instead says **POWER CUT FAILED — UNPLUG NOW**.
Neither result pretends that an attached battery was removed. The cutoff is
latched with no automatic re-enable; physically disconnect the FC and battery
before exiting or rebooting because APPLaunch can restore its saved EXT rail
state when it starts again.

## Field check

Press **Fn+2**, or choose **Menu → Run field check**. GNDHOG runs three CLI
queries in sequence without changing settings or writing FC flash:

```text
status     current sensors, battery, rates, and arming disable flags
tasks      scheduler load evidence
version    firmware, target, build, and MSP API identity
```

Betaflight's `tasks` command resets its transient maximum-execution-time
statistics after printing them. The field check therefore does not alter
configuration or flight behavior, but it is not literally side-effect-free.

The summary prioritizes active arming blockers and gives a short next action
for every current Betaflight flag it recognizes. `CLI` and `MSP` are expected
while attached here and do not become fake faults. A nonzero I2C count is
reported as cumulative since boot, scheduler percentages are shown as evidence
rather than graded against an invented threshold, and absent fields remain
unknown. Press **V** for the raw terminal transcript, **R** to run a fresh
snapshot, or **S** to save a report under
`~/.local/share/bfcli/diagnostics`.

When `status` contains `Core temp=...`, the summary compares the MCU reading
with Betaflight's 70 C default core-temperature alarm and marks 80 C or higher
as critical. Targets that do not expose this line remain temperature-unknown;
GNDHOG does not manufacture a sensor.

This is a fast flight-line snapshot, not an airworthiness verdict. It cannot
inspect solder joints, prop condition, motor direction, control-surface
movement, failsafe behavior in the air, or whether the pilot has made peace
with the tree line. Props-off inspection and functional checks still belong to
the operator.

## Build

```sh
make arm64      # cross-build; requires g++-aarch64-linux-gnu
make            # host build for simulator and self-checks
make test       # checks, including a complete session against a fake FC
make package    # ARM64 AppStore .deb plus manifest/package validation
```

`make arm64` links libstdc++ and libgcc statically. The resulting binary needs
only `libc.so.6` and requests at most `GLIBC_2.38`; Debian trixie supplies 2.41.
The dependency hearing is therefore adjourned.

Build natively on the Cardputer with one job. The ARM side of its memory split
is 256 MB, and parallel C++ compilation can convert that fact into a memorial:

```sh
make -j1
```

Every build refreshes its source identity. **About** and `--version` show the
short Git commit, adding `-dirty` when tracked changes or untracked files are
present. An unchanged build does not rewrite the generated header. Outside a
Git checkout, the identity is `unknown`; the binary does not forge papers.

The mascot is compiled into the executable. Builds do not need an image
library or an artwork download.

## Cardputer Zero AppStore package

The Cardputer Zero store metadata lives in `app-builder.json`: package identity,
listing copy, 320×170 screenshots, square icon, categories, author, license,
share code, and permissions. Build the upload artifact in WSL or another
Debian-based ARM64 cross-build environment:

```sh
make package
# dist/bfcli_0.1.0_arm64.deb
```

The package target cross-builds the binary, stages the install tree, creates no
systemd service, and checks both the manifest and the finished Debian archive
with `tools/validate-app-store.py`. Run only the metadata check with
`make store-check`.

The permission manifest enables `additional_hardware` for the framebuffer,
evdev keyboard, backlight, and USB or UART serial device. Camera, microphone,
IMU, network, background service, and external display stay disabled. The app
is a serial terminal, not a hardware buffet.

Install a built package for a device smoke test with:

```sh
sudo apt install --no-install-recommends ./dist/bfcli_0.1.0_arm64.deb
```

For submission, use the current
[Cardputer Zero AppBuilder](https://github.com/CardputerZero/AppBuilder) and run
`czdev publish --deb dist/bfcli_0.1.0_arm64.deb` from this repository. Follow the
publisher prompts and attach the real-device demo requested for a first release.

## Install

```sh
sudo ./tools/install.sh --add-groups
```

The installer puts the binary at `/opt/bfcli/bin/bfcli`, creates the wrapper
`/opt/bfcli/run-bfcli`, and installs an APPLaunch entry at
`/usr/share/APPLaunch/applications/bfcli.desktop` under the visible name
**GNDHOG ZERO**. The launcher mascot goes to
`/usr/share/APPLaunch/share/images/gndhog-zero_100.png`.

An existing binary is retained as `bfcli.prev`. Installation copies to a
temporary file and renames it into place, so interruption does not leave half
an executable wearing the correct permissions. On ARM64 the candidate must
pass its self-check before replacement.

`--uninstall` removes the current install, desktop entry, and icon. It does not
restore an older binary or delete user backups. No service is installed, the
boot partition is untouched, and the vendor image remains exactly where the
vendor abandoned it.

The runtime user needs the **video**, **input**, **dialout**, and **audio** groups
for the framebuffer, keyboard, serial port, and HUD sounds. The installer reports missing groups
and `--add-groups` can add them. Log out and back in after a group change; Unix
will not recognize personal growth mid-session.

Inspect available ports before launching:

```sh
/opt/bfcli/bin/bfcli --list-ports
```

## Backups

Backups live under `~/.local/share/bfcli/backups`, unless `BFCLI_DATA_DIR`
points elsewhere. Names follow
`BTFL_cli_<craft>_<timestamp>_<board>_backup.txt`, matching Betaflight
Configurator so files can be copied between them.

Writes use a temporary file, `fsync`, `rename`, and a directory `fsync`. This
is less glamorous than losing a tune because the battery left during the one
millisecond when the filesystem was feeling philosophical.

Restore ignores blank and comment lines, sends the remaining commands one at
a time, and waits for every completed prompt. If the backup `board_name`
differs from the attached FC, the confirmation displays the mismatch in red.

**A restore is not persistent until you run `save`.** GNDHOG sends the
configuration. Betaflight decides whether it survives the reboot. Pressing
Enter harder does not change the storage model.

## Safety

- Commands that can spin motors (`motor 1 1100`, `dshotprog`) or erase
  configuration (`defaults`, `flash_erase`) require confirmation. A held key
  cannot accept the dialog; modifier and repeat state are cleared when it
  opens.
- **Without an active VTX guard, disconnect only closes the port.** With a
  guard active, GNDHOG asks whether to restore the saved flight state. Restore
  sends `exit`, deliberately discarding unsaved CLI changes and rebooting the
  FC; cancel closes the link and leaves pit mode active until the FC is
  rebooted or power-cycled.
- GNDHOG never selects 1200 baud. Betaflight interprets that as a request to
  reboot into DFU.
- A board already in DFU mode is detected and reported instead of being opened
  as a CLI target.
- Completion learns from output already received. It sends no hidden discovery
  commands to the FC.
- Automatic safety traffic is limited to the connection-time VTX
  capability/readback sequence and one `status` temperature capture. None of
  it writes flash. Pit mode is requested only after operator confirmation and
  must be reported back before success.

Remove propellers before bench work that can arm or drive motors. Software
confirmation is a guardrail. It is not a small orange force field.

## Development

```sh
./build/bfcli --sim              # built-in fake flight controller
./build/bfcli --preview out/     # write one PPM per screen
./build/bfcli --selftest         # run the check suite
./build/bfcli --list-ports       # enumerate visible serial targets
./build/bfcli --about            # credits, no FC connection
./build/bfcli --mute             # session-only silent launch
./build/bfcli --sound-test       # bounded exact-card playback check
./build/bfcli --version          # author and source commit
./build/bfcli --help
```

`--sim` creates a pseudo-terminal answering as a BETAFPVG473_V2 running
Betaflight 2026.6.0-alpha, including realistic field-check evidence and a
`diff all`, plus a ready SmartAudio VTX that acknowledges pit mode. The complete
connect → guard → diagnose → backup → restore path can
therefore be exercised without hardware.
It does not exercise the physical keyboard, framebuffer, USB link, or an actual
flight controller. Even imaginary hardware has a job description.

## Layout

| Path | What lives there |
|------|------------------|
| `src/keys.cpp` | v5 46-key matrix and layer decoding |
| `src/bfsession.cpp` | CLI conversation, prompt detection, backup and restore |
| `src/diagnostics.cpp` | version-tolerant field evidence parser and saved report formatter |
| `src/term.cpp` | scrollback, wrapping, and line editing |
| `src/bfcommands.cpp` | command tables, completion, and risk classification |
| `src/display.cpp` | framebuffer and backlight access |
| `src/audio.cpp` | exact-card ALSA routing and synthesized HUD cues |
| `src/serialport.cpp` | port discovery and termios |
| `src/ui.cpp` | screen rendering |
| `src/brand.h` | project identity, author, and About copy |
| `src/simfc.cpp` | simulated flight controller |
| `src/mascot.cpp` | embedded mascot renderer |
| `assets/` | approved black/white/transparent mascot masters for each native size |
| `store/` | AppStore icon and current 320×170 UI screenshots |
| `app-builder.json` | package identity and AppStore listing contract |
| `packaging/` | Debian control, APPLaunch entry, wrapper, and copyright data |
| `tools/build-info.sh` | source-commit identity refreshed by Make |
| `tools/install.sh` | staged device installation and APPLaunch entry |
| `tools/package.sh` | policy-compliant ARM64 Debian package builder |
| `tools/validate-app-store.py` | local store metadata and archive policy gate |

That is the whole machine: one terminal, one mascot, one serial wire, and no
cloud account asking whether the quad is still you.

## License

GNDHOG ZERO is released under the [MIT License](LICENSE).

# GNDHOG ZERO — Betaflight CLI for the M5Stack Cardputer Zero

![GNDHOG ZERO mascot](assets/gndhog-zero-source.png)

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

## What it does

- **Full CLI terminal.** Run `status`, `get`, `set`, `diff`, `resource`,
  `vtxtable`, or whatever other instruction you intend to regret responsibly.
  Scrollback holds 3000 lines; command history survives the session.
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

**The ten unusual assignments are partly inferred.** The vendor overlay routes
those Sym positions through placeholder keycodes such as `KEY_RO`,
`KEY_MUHENKAN`, and the Japanese IME block. Those codes carry no US-layout
meaning, so GNDHOG assigns one. That is an educated mapping, not a treaty.

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

Hold `Fn` and tap a number for the quick rack: **1** help, **2** status,
**3** version, **4** diff, **5** backup, **6** backups, **7** tasks, **8** save,
**9** menu, **0** disconnect.

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

## Build

```sh
make arm64      # cross-build; requires g++-aarch64-linux-gnu
make            # host build for simulator and self-checks
make test       # checks, including a complete session against a fake FC
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

The runtime user needs the **video**, **input**, and **dialout** groups for the
framebuffer, keyboard, and serial port. The installer reports missing groups
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
- **Disconnect closes the port. It does not send `exit`.** Betaflight treats
  `exit` as a reboot, so it remains an explicit quick-menu command labelled
  accordingly.
- GNDHOG never selects 1200 baud. Betaflight interprets that as a request to
  reboot into DFU.
- A board already in DFU mode is detected and reported instead of being opened
  as a CLI target.
- Completion learns from output already received. It sends no hidden discovery
  commands to the FC.

Remove propellers before bench work that can arm or drive motors. Software
confirmation is a guardrail. It is not a small orange force field.

## Evidence and open ground

Capability claims are separated here because a compiled branch, a simulated
flight controller, and a live wire are three different witnesses.

**Verified on x86-64 Linux and ARM64 under QEMU.** The 187 self-checks cover the
session state machine against a simulated Betaflight FC over a real pty: CLI
entry, prompt detection through a streaming `diff`, complete `diff all`
capture, and a 40-line restore. Keyboard decoding is checked against the v5
matrix. Preview output covers the screen layouts. The suite also passes under
AddressSanitizer and UndefinedBehaviorSanitizer. Seven isolated Git scenarios
cover clean, dirty, unknown, and post-commit build identities.

**Verified on the bench Cardputer Zero.** The ARM64 binary passes all 187
checks natively. A bounded `--about` run opens the real framebuffer and evdev
input, renders the inspected 320×170 About screen, and exits cleanly before
APPLaunch is restored. That check opens no FC serial port.

**Implemented against documented Linux interfaces.** Framebuffer geometry and
pixel bitfields come from `FBIOGET_VSCREENINFO` and
`FBIOGET_FSCREENINFO`; conversion handles formats beyond plain RGB565. The
keyboard is found and rechecked by evdev name (`tca8418c`), never trusted by
event number. Serial ports are ranked using USB identity from sysfs. Backlight
range is read from sysfs, and writes are read back.

**Not yet established on real FC hardware.** Physical readability, actual
TCA8418 key events, the ten inferred Sym assignments, USB-A enumeration of a
specific FC, and end-to-end restore timing over CDC-ACM still need direct
flight-controller observation. The key-test screen and `config.ini` override
exist because inference is useful, but it is not solder.

**Deliberately absent.** No monitor-mode Wi-Fi, CSI, camera, IMU, GPIO, or I²C
experiments. GNDHOG uses the framebuffer, evdev, one serial port, and the sysfs
backlight. It does not become a spectrum analyzer because the README used an
adjective.

## Development

```sh
./build/bfcli --sim              # built-in fake flight controller
./build/bfcli --preview out/     # write one PPM per screen
./build/bfcli --selftest         # run the check suite
./build/bfcli --list-ports       # enumerate visible serial targets
./build/bfcli --about            # credits, no FC connection
./build/bfcli --version          # author and source commit
./build/bfcli --help
```

`--sim` creates a pseudo-terminal answering as a BETAFPVG473_V2 running
Betaflight 2026.6.0-alpha, including a realistic `diff all`. The complete
connect → backup → restore path can therefore be exercised without hardware.
The simulator is a witness with excellent availability and no propellers.

## Layout

| Path | What lives there |
|------|------------------|
| `src/keys.cpp` | v5 46-key matrix and layer decoding |
| `src/bfsession.cpp` | CLI conversation, prompt detection, backup and restore |
| `src/term.cpp` | scrollback, wrapping, and line editing |
| `src/bfcommands.cpp` | command tables, completion, and risk classification |
| `src/display.cpp` | framebuffer and backlight access |
| `src/serialport.cpp` | port discovery and termios |
| `src/ui.cpp` | screen rendering |
| `src/brand.h` | project identity, author, and About copy |
| `src/simfc.cpp` | simulated flight controller |
| `src/mascot.cpp` | embedded mascot renderer |
| `assets/` | supplied mascot source and launcher icon |
| `tools/build-info.sh` | source-commit identity refreshed by Make |
| `tools/install.sh` | staged device installation and APPLaunch entry |

That is the whole machine: one terminal, one mascot, one serial wire, and no
cloud account asking whether the quad is still you.

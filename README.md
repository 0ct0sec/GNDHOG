# GNDHOG ZERO — Betaflight CLI for the M5Stack Cardputer Zero

![GNDHOG ZERO mascot](assets/gndhog-zero_100.png)

By **0ct0**. Props off. Shell on.

A field terminal for your tinywhoops. Plug a flight controller into the
Cardputer Zero's USB-A port, and get the full Betaflight CLI on the built-in
320×170 screen — plus one-key config backup and restore to the SD card.

Native ARM64 Linux. No Arduino, no ESP-IDF, no X11, no SDL, no LVGL. It draws
straight into `/dev/fb0` and reads the TCA8418 keyboard through evdev.

```
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

- **Full CLI terminal** — everything `status`, `get`, `set`, `diff`, `resource`,
  `vtxtable` can do, with 3000 lines of scrollback and command history.
- **Tab completion** for CLI commands and `set`/`get` parameter names. The
  parameter index is seeded with ~140 common names and then learns every name
  that scrolls past in a `dump` or `diff` — no extra queries to the FC.
- **Backup** — one key runs `diff all` and writes a file named exactly the way
  Betaflight Configurator names its backups, so the files are interchangeable.
- **Restore** — sends a saved backup back line by line, waiting for each prompt,
  with a progress bar and a count of anything the FC rejected.
- **A solved keyboard.** Betaflight parameters are all `snake_case`, and the
  46-key Cardputer keyboard has no obvious underscore. See below.
- **About** — mascot, author, source commit, and a short incident report from
  the lawn. Open **Menu → About GNDHOG ZERO**, or press **A** on the port picker.
  Enter/Escape returns to the screen you came from; no FC connection is needed.

The executable, install paths, and data directory deliberately keep their
original `bfcli` names. Existing launcher references, backups, command history,
`config.ini`, and `BFCLI_DATA_DIR` keep working through the rename.

## The keyboard problem, and the fix

Every Betaflight parameter looks like `gyro_lpf1_static_hz`, and half of them
are set with `=`. Neither character has an obvious home on a 46-key thumb
keyboard, so they are placed where they are easiest to reach:

| Chord | Character |
|-------|-----------|
| **Sym + L** | **`_`** |
| **Sym + 0** | **`=`** |
| Sym + U | `-` |
| Sym + J | `:` |
| Sym + K | `"` |
| Sym + W / E | `{` `}` |
| Sym + V / B | `<` `>` |
| Sym + N / M | `?` `|` |

The rest of the Sym layer (`[ ] ; ' \` \ , . / *` and a numeric keypad) follows
the keycodes the v5 overlay already emits.

**These assignments are partly inference.** The vendor overlay routes about ten
Sym positions through placeholder keycodes (`KEY_RO`, `KEY_MUHENKAN`, and the
Japanese IME block) that carry no US-layout meaning, so this app assigns them.
If one is wrong on your unit:

1. Open **Menu → Keymap & key test** and press the key. It shows the raw scan
   code, keycode, decoded layer, and resulting character.
2. Put a correction in `~/.local/share/bfcli/config.ini` and restart:
   ```ini
   sym.0x28 = _
   ```

No rebuild needed. The same screen prints the whole Sym layer as a reference
card, with `_` and `=` highlighted.

A plain USB keyboard on the hub also works, and is decoded from keycodes alone.

## Keys

| | |
|---|---|
| `Tab` | complete a command or parameter |
| `Fn+F` / `Fn+X` | previous / next command in history |
| `Fn+Z` / `Fn+C` | move the cursor left / right |
| `Fn+K` / `Fn+N` | start / end of line |
| `Fn+L` / `Fn+M` | scroll the output up / down |
| `Fn+Backspace` | delete forward |
| `Ctrl+U` / `Ctrl+K` | kill to start / end of line |
| `Ctrl+W` | delete the previous word |
| `Ctrl+C` / `Ctrl+L` | clear the line / the screen |
| `Esc` | menu, then back, then exit |

Hold `Fn` and tap a number for shortcuts: **1** help, **2** status,
**3** version, **4** diff, **5** backup, **6** backups, **7** tasks, **8** save,
**9** menu, **0** disconnect.

## Connecting a flight controller

Set the **Host/Slave switch to HOST** and plug the FC into the **USB-A** port.
It enumerates as a USB CDC-ACM device (`/dev/ttyACM0`, usually `0483:5740`) and
GNDHOG ZERO connects to it automatically.

A spare FC UART wired to the Grove connector works too — pick the port and baud
on the connect screen. Grove signal pins are **3.3 V**; the 5 V pin being 5 V
does not make the data pins 5 V tolerant.

In Slave/device mode the internal hub disconnects and USB-A goes offline, so
the FC will not be visible.

## Build

```sh
make arm64      # cross build (needs g++-aarch64-linux-gnu)
make            # host build, for the simulator and self-tests
make test       # self-checks, including an end-to-end run against a fake FC
```

`make arm64` links libstdc++ and libgcc statically, so the binary depends only
on `libc.so.6` and needs at most `GLIBC_2.38` — comfortably below trixie's 2.41.

On the device itself, build with `-j1`: the ARM side of the memory split is
256 MB, and parallel compiles will not fit.

```sh
make -j1
```

Every build refreshes its Git identity: About and `--version` show the short
source commit, with `-dirty` for tracked edits or untracked source. An unchanged
build does not rewrite the generated header. Builds outside a Git checkout
report `unknown` rather than inventing a revision. The mascot is embedded in
the executable; building it needs no image library or artwork download.

## Install

```sh
sudo ./tools/install.sh --add-groups
```

This puts the binary in `/opt/bfcli/bin/bfcli`, a wrapper at
`/opt/bfcli/run-bfcli`, and an APPLaunch entry at
`/usr/share/APPLaunch/applications/bfcli.desktop`, displayed as **GNDHOG ZERO**.
Its supplied mascot is installed at
`/usr/share/APPLaunch/share/images/gndhog-zero_100.png`. The previous binary is
kept as `bfcli.prev`. `--uninstall` removes the current install, desktop entry,
and icon; it does not restore an older version or delete user backups.
On ARM64, a failed self-test aborts installation. No service is installed,
nothing on the boot partition is touched, and the vendor image is left alone.

GNDHOG ZERO needs the **video** (framebuffer), **input** (keyboard) and **dialout**
(serial) groups. The installer reports which are missing.

Check what it can see before launching:

```sh
/opt/bfcli/bin/bfcli --list-ports
```

## Backups

Files land in `~/.local/share/bfcli/backups` (override with `BFCLI_DATA_DIR`)
and are named `BTFL_cli_<craft>_<timestamp>_<board>_backup.txt` — the same shape
Configurator uses, so you can copy them either direction.

Writes are durable: temp file, `fsync`, `rename`, then `fsync` on the directory.

Restore skips blank and comment lines (Betaflight ignores them anyway, and they
are two thirds of the file), sends the rest one line at a time, and waits for
each prompt. If the file's `board_name` differs from the connected FC, the
confirmation says so in red. **Nothing is kept until you run `save`.**

## Safety

- Commands that can spin a motor (`motor 1 1100`, `dshotprog`) or wipe the
  config (`defaults`, `flash_erase`) ask first. A held key cannot answer a
  dialog — modifier and repeat state is dropped whenever one opens.
- **Disconnect only closes the port.** `exit` is deliberately not sent, because
  on Betaflight that reboots the flight controller. It is available as an
  explicit command in the quick menu, labelled as such.
- 1200 baud is never selected: Betaflight reads it as "reboot to DFU".
- A board in DFU mode is detected and reported rather than connected to.
- Nothing is sent to the FC that you did not ask for. The completion index is
  built by reading output that already scrolled past.

## Evidence and what is untested

The reference hardware document asks for capability claims to be separated by
evidence level, so:

**Verified on a dev host (x86-64 Linux) and ARM64 under QEMU.** 187 self-checks pass,
including the full session state machine driven against a simulated Betaflight
FC over a real pty: CLI entry, prompt detection across a streaming `diff` (the
case where comment lines momentarily look like a bare prompt), a complete
`diff all` capture, and a 40-line restore. Keyboard decoding is tested against
the exact v5 matrix. Rendering is verified by inspecting `--preview` output.
The same suite passes under AddressSanitizer/UndefinedBehaviorSanitizer.
Seven isolated Git scenarios check clean/dirty/unknown build identity and
rebuilding after a commit without editing source.

**Verified on the bench Cardputer Zero.** The ARM64 build passes all 187 checks
natively. A bounded `--about` run opens the real framebuffer and evdev input,
exits cleanly, and leaves the captured 320×170 About frame in `/dev/fb0`.
No FC serial port is opened for this check. APPLaunch is restored afterward.

**Implemented against documented Linux interfaces.**
Framebuffer geometry and pixel format are read via `FBIOGET_VSCREENINFO` /
`FBIOGET_FSCREENINFO` rather than assumed, with a conversion path if the
bitfields are not plain 5/6/5. The keyboard is found by evdev name (`tca8418c`),
never by event number, and the name is re-checked after opening. Serial ports
are ranked by USB identity from sysfs. Backlight `max_brightness` is read, and
a write is confirmed by reading back.

**Not established.** These checks do not use a real flight controller.
Specifically untested: physical panel readability at this font size,
actual key presses through the TCA8418 event stream, whether the ten
inferred Sym characters match your unit, USB-A enumeration of a specific FC,
and end-to-end restore timing over a real CDC-ACM link. The key-test screen and
`config.ini` overrides exist precisely because the Sym mapping is the part most
likely to need correcting on first contact.

**Deliberately not attempted.** No monitor-mode Wi-Fi, no CSI, no camera, no
IMU, no GPIO or I²C poking. GNDHOG ZERO uses the framebuffer, evdev, a serial port,
and one sysfs backlight — nothing else.

## Development

```sh
./build/bfcli --sim              # run against a built-in fake flight controller
./build/bfcli --preview out/     # one PPM per screen, for layout inspection
./build/bfcli --selftest         # the check suite
./build/bfcli --list-ports       # what the machine can see
./build/bfcli --about            # credits on the panel, no FC connection
./build/bfcli --version          # author and source commit; no hardware access
./build/bfcli --help
```

`--sim` spins up a pseudo-terminal answering as a BETAFPVG473_V2 running
Betaflight 2026.6.0, including a realistic `diff all`, so the whole
connect → backup → restore path can be exercised with no hardware attached.

## Layout

| | |
|---|---|
| `src/keys.cpp` | the v5 46-key matrix and the layer decoder |
| `src/bfsession.cpp` | CLI conversation, prompt detection, backup/restore |
| `src/term.cpp` | scrollback, wrapping, line editing |
| `src/bfcommands.cpp` | command tables, completion, risk classification |
| `src/display.cpp` | framebuffer and backlight |
| `src/serialport.cpp` | port discovery and termios |
| `src/ui.cpp` | all screen rendering |
| `src/brand.h` | project identity, author and About copy |
| `assets/` | supplied mascot source and launcher icon |
| `tools/build-info.sh` | build commit identity, refreshed by Make |
| `src/simfc.cpp` | the fake flight controller |

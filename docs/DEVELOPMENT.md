# Building and packaging GNDHOG ZERO

[Back to the user guide](../README.md). This page is for contributors and release maintainers.

## Build

On Debian/Ubuntu or WSL, install the build tools first:

```sh
sudo apt install build-essential g++-aarch64-linux-gnu binutils-aarch64-linux-gnu dpkg-dev python3 qemu-user
```

```sh
make arm64      # cross-build; requires g++-aarch64-linux-gnu
make            # host build for simulator and self-checks
make test       # checks, including a complete session against a fake FC
make package    # ARM64 AppStore .deb plus manifest/package validation
```

`make arm64` links libstdc++ and libgcc statically. Check the resulting ELF's
GLIBC requirements against `packaging/control.in` when changing toolchains.
HUD audio loads ALSA and Pulse client libraries at runtime, so the package
declares both even though they are absent from ELF `NEEDED` entries. The Pulse
client uses the existing PipeWire/Pulse session; the package starts no audio service.

Prefer a host cross-build. If building on the Cardputer itself, use one job:

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
evdev keyboard, backlight, and USB or UART serial device, and `imu` for the
BMM150 magnetometer and BMI270 accelerometer the compass reads through IIO.
Camera, microphone, network, background service, and external display stay
disabled. The app is a serial terminal with a compass, not a hardware buffet.

Install a built package for a device smoke test with:

```sh
sudo apt install --no-install-recommends ./dist/bfcli_0.1.0_arm64.deb
```

For submission, use the current
[Cardputer Zero AppBuilder](https://github.com/CardputerZero/AppBuilder) and run
`czdev publish --deb dist/bfcli_0.1.0_arm64.deb` from this repository. The publisher uploads the binary to a release on your packages fork and opens a metadata PR. First releases require a short real-device video in that PR and manual maintainer approval; a simulator slideshow does not satisfy that requirement. See [the release checklist](PUBLISHING.md).

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

## Development

```sh
./build/bfcli --sim              # built-in fake flight controller
./build/bfcli --sim-mesh         # built-in fake Meshtastic radio
./build/bfcli --mesh --port DEV  # open DEV as a radio, not a Betaflight CLI
./build/bfcli --gnss /dev/serial0  # LoRa Cap GNSS receiver
./build/bfcli --no-gnss          # do not open a GNSS receiver at all
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

`--sim-mesh` does the same for the radio: a three-node database, a primary
channel, an EU_868 LoRa configuration, routing acknowledgements (and refusals),
inbound text and position packets, and console log lines deliberately
interleaved with the framed protobuf, because that is what real firmware does
and it is the part that breaks a decoder.
It does not exercise the physical keyboard, framebuffer, USB link, or an actual
flight controller. Even imaginary hardware has a job description.

## Layout

| Path | What lives there |
|------|------------------|
| `src/keys.cpp` | v5 46-key matrix and layer decoding |
| `src/bfsession.cpp` | CLI conversation, prompt detection, backup and restore |
| `src/protowire.cpp` | protobuf wire format, by hand, with no runtime |
| `src/meshtastic.cpp` | Meshtastic framing, messages, helpers, transcripts |
| `src/meshsession.cpp` | radio link, node database, chat, delivery state |
| `src/gnss.cpp` | NMEA 0183 parser for the LoRa Cap receiver |
| `src/compass.cpp` | BMM150 through IIO: tilt compensation, calibration, alignment |
| `src/marks.cpp` | saved places: the file format and its limits |
| `src/quickmsg.cpp` | canned messages, their config overrides, and `{pos}` |
| `src/diagnostics.cpp` | version-tolerant field evidence parser and saved report formatter |
| `src/term.cpp` | scrollback, wrapping, and line editing |
| `src/bfcommands.cpp` | command tables, completion, and risk classification |
| `src/display.cpp` | framebuffer and backlight access |
| `src/audio.cpp` | exact-card ALSA routing and synthesized HUD cues |
| `src/serialport.cpp` | port discovery and termios |
| `src/ui.cpp` | screen rendering |
| `src/brand.h` | project identity, author, and About copy |
| `src/simfc.cpp` | simulated flight controller |
| `src/simmesh.cpp` | simulated Meshtastic radio |
| `src/mascot.cpp` | embedded mascot renderer |
| `assets/` | approved black/white/transparent mascot masters for each native size |
| `store/` | AppStore icon and 320×170 software-rendered UI previews |
| `docs/` | Field guide, build instructions, and publishing checks |
| `app-builder.json` | package identity and AppStore listing contract |
| `packaging/` | Debian control, APPLaunch entry, wrapper, and copyright data |
| `tools/build-info.sh` | source-commit identity refreshed by Make |
| `tools/install.sh` | staged device installation and APPLaunch entry |
| `tools/package.sh` | policy-compliant ARM64 Debian package builder |
| `tools/validate-app-store.py` | local store metadata and archive policy gate |

That is the whole machine: one terminal, one mascot, one serial wire, and no
cloud account asking whether the quad is still you — or, on the other end of
that wire, whether the mesh is still yours.

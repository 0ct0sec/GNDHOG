# GNDHOG ZERO field guide

[Back to the quick start](../README.md). Controls, connection details, and the small print that matters when the quad is in a hedge.

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

With a radio attached the screens change and so do the keys:

| Key | Action |
|-----|--------|
| `Enter` | open the highlighted conversation, or send the typed message |
| `F` / `Fn+4` | locate: compass rose, range, and bearing to the node (or to the open chat's peer) |
| `M` | saved marks; on the Locate screen, mark that node's last position |
| `Tab` | quick messages, from inside a conversation |
| `I` / `Fn+2` | radio firmware, region, preset, and channel |
| `G` / `Fn+6` | LoRa Cap GNSS status |
| `P` / `Fn+5` | transmit this station's own position (asks first) |
| `L` | the radio's console log |
| `N` | back to the node list from the log |
| `Fn+9` / `Esc` | menu |
| `Fn+0` | disconnect |

## HUD sounds

The startup sweep and terse avionics-style cues are generated in memory; there
are no WAV assets to lose or decode. Open the menu to toggle **HUD sounds** or
change **HUD volume** in 10% steps. Both settings persist in the existing
`config.ini`; `--mute` is a launch-wide override the menu cannot undo, and it
does not change the saved choice. `/opt/bfcli/bin/bfcli --sound-test` plays the
bounded startup cue at 70% and exits with a failure if discovery, mixer setup,
PCM output, or drain does not complete.

On Cardputer Zero, HDMI is commonly ALSA card 0 and the built-in speaker codec
is card 1. GNDHOG therefore targets the vendor session sink
`alsa_output.platform-sound.stereo-fallback` while PipeWire is running. Outside
that session it opens `default:CARD=ES8389Audio` (or the earlier `ES8388Audio`
identity) explicitly. Neither path uses generic default audio. The output
worker never stalls the UI, caps the codec playback controls at their 0 dB
point, bounds synthesized PCM, and collapses repeated navigation chirps.
Critical temperature and link-loss cues clear less important queued sounds so
the warning is not waiting behind a thumb-operated sonata.

If the menu reports a silent fallback, check `cat /proc/asound/cards` for the
exact codec, confirm the launcher user belongs to `audio`, and inspect
`/dev/snd/pcmC*D*p`. `--mute` and `sound.enabled = 0` are deliberate silence;
an absent codec or `libasound.so.2` is reported as unavailable rather than
quietly sending the cues to HDMI.

## Connecting a flight controller

Set the **Host/Slave switch to HOST**, then plug the FC into **USB-A**. A normal
Betaflight USB connection appears as a CDC-ACM device such as `/dev/ttyACM0`
(commonly `0483:5740`), and GNDHOG ranks it for automatic connection.

A spare FC UART on the Grove connector works too; select its port on the
connect screen and set its rate with **B**, which moves the rate of whichever
protocol **Enter** is about to speak and leaves the other two peers alone.
Grove signal pins are **3.3 V**. The presence of 5 V on the
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

## Meshtastic

Set the **Host/Slave switch to HOST** and plug the radio into **USB-A**, or wire
one to Grove and pick its baud — the radio's rate is its own (`mesh.baud`) and
setting it does not disturb the flight controller's. An M5Stack Unit C6L enumerates as its own
ESP32-C6 USB serial device (`303a:1001`), which is enough for the picker to
propose the mesh protocol; **M** overrides the proposal either way, because a
board can be flashed with anything.

On connect GNDHOG sends the reference client's resynchronisation burst, asks for
the configuration, and reads the reply stream: the radio's own node number,
device metadata, the LoRa and position configuration, the channel list, and the
node database. Framed protobuf and the firmware's debug console share one wire,
so bytes that are not part of a frame are kept and shown as the **radio log**
rather than discarded as noise.

```text
┌─────────────────────────────────────────────────────┐
│ GNDHOG ZERO  GNDH mesh nodes         [==] 96% ready │
│>ALL  Broadcast - LongFast                           │
│ GNDH GNDHOG BENCH   (this radio)                now │
│*HILL HILLTOP RELAY                              12m │
│ VAN2 VAN 2                                       1h │
│ !a1b2c3d4  snr 6.2  1 hop  87%  1.4km NE Enter chat │
└─────────────────────────────────────────────────────┘
```

The node list is the conversation list. Row one is the broadcast channel; every
other row is a radio, with an unread marker and **how long ago it was heard**.
Nothing here says a node is "online", because the mesh does not know that: a
quiet node and a dead node look identical from here, and only one of them is a
problem.

Enter opens a conversation. Type and press Enter to send. A direct message goes
out asking for acknowledgement and is marked `.` while it waits, `+` when the
mesh returns a routing ACK for that packet id, and `!` with the reason — no
route, timeout, duty cycle limit — when it does not. A broadcast is marked
sent, because the mesh does not acknowledge one and a permanent spinner would
only be a lie with a nicer shape.

Transcripts live under `~/.local/share/bfcli/mesh`, one tab-separated file per
peer, and reload on the next launch. A message still waiting for an
acknowledgement when the app closed reloads as unresolved rather than as
delivered. **Menu → Mesh network** can export a conversation as readable text or
delete one; deleting removes this device's copy and nothing else.

The radio must be able to transmit before anything is sent. A radio with no LoRa
region set, or with transmit disabled, is reported as `no TX` in the state chip
and refuses the message with the reason instead of queueing it into a device
that is deliberately mute.

## GNSS: the cap's receiver, or a GPS unit on Grove

GNDHOG wants a position of its own, and it reads one from whatever NMEA 0183
receiver is on the header UART, `/dev/serial0` (a symlink to `/dev/ttyS0`).
Supported receiver connections include the AT6668 on the **Cap
LoRa-1262 GPS**, and an M5Stack **GPS Unit** plugged into the Grove socket.
Start at 115200 8N1 or select the receiver's configured rate. The app parses `GGA`, `RMC`, `GSV`
and the receiver's own `TXT` remarks (the cap's unit says `ANTENNA OPEN`
indoors; the status page repeats it without judging it). Change the device
with `gnss.device` in `config.ini`, name one for a single launch with
`--gnss DEV` — which opens it even if the saved switch is off — or skip it
with `--no-gnss`. Both switches are launch-wide session overrides in the way
`--mute` is: they do not rewrite the saved choice, and the menu toggle says so
rather than appearing to do nothing.

The Grove socket has two controls of its own on this board and GNDHOG touches
neither: its 5 V is switched (`/sys/class/leds/grove_5v_out`, the launcher's
GROVE5V setting), and its signal pins are a mux between I2C and the UART
(`grove_fun`, GPIO4, UART when off, which is the default). The cap's receiver
reaches the same UART through the EXT header, whose 5 V is the separate
`ext_5v_out` that M5Stack's cap application switches on. A receiver that is
powered and wired to the UART is all GNDHOG asks for.

**The rate is probed, not promised.** The receiver is the one peer whose port
GNDHOG owns outright, so `gnss.baud` can be checked against the wire. A UART
that carries bytes but no checksummed sentence within six seconds is a
receiver at some other rate: the port is reopened at each rate in the table —
9600 first, because that is what most receivers that are not at 115200 ship at
— for three seconds each, and the rate that answers becomes `gnss.baud`
(unless `--gnss-baud` named one for this launch, in which case it is used and
not saved). A UART that carries nothing is released at the first deadline
without walking the table, because on this board that node is also the Grove
header and holding it keeps a Grove-wired flight controller waiting; a UART
that carries readable text that is not NMEA is a radio's console, not a
receiver, and is released too. The **Baud rates** page still sets the rate by
hand and applies it at once by reopening the port.

Presence is proved by sentences arriving, never by the device node opening: that
UART exists whether or not anything is wired to it. A sentence counts only
with its checksum, because a wire sampled at the wrong rate throws up the odd
`$` followed by debris.

**Mesh radio and receiver together.** The radio is a USB device — an M5Stack
Unit C6L on the USB-A hub, say — and the receiver is the UART, so they are
different device nodes and never contend. The port picker lists both, tags the
radio's row `[mesh radio]` from its USB identity, and marks the receiver's row
`GNSS live` once sentences have arrived (`GNSS probe` while it is still
listening); **Enter** on a live receiver shows its status instead of opening a
link, and **G** shows it from the picker at any time. GNDHOG does not drive the cap's SX1262 radio. It reads the cap's GNSS receiver; mesh features require a separate radio running Meshtastic firmware. The cap's GNSS and a Grove GPS are the same UART, so only one of them is
present at a time.

GNSS starts independently of the mesh link. The picker reserves a live receiver for GNSS; a silent probe releases the UART so another serial device can use it. If a supposed mesh radio answers with checked NMEA sentences, GNDHOG closes that link and reports the receiver instead.

With a fix, the node list shows **range and bearing** to every node that
reported a position, and **Share my position** transmits this station's own
coordinate after a confirmation that says what it is about to do. Without a fix
it says so. It will not send a stale coordinate, and it will not borrow the
radio's idea of where it is and present that as this station's.

## Finding things: Locate, marks, quick messages, SOS

Two kinds of people carry a Meshtastic radio into a field with this device: a
pilot whose quad is now somewhere in that hedge, possibly with a tracker node
still chirping, and a hiker whose group has split up and whose car is a long
way back. Both want the same four things, and none of them are a keyboard.

**Locate.** Press **F** (or **Fn+4**) on a node, or **Enter** on a mark:

```text
┌─────────────────────────────────────────────────────┐
│ GNDHOG ZERO  locate                  [==] 96% ready │
│       N         HILLTOP RELAY                       │
│    .  |  .      !a1b2c3d4  ROUTER                   │
│  W    |    E    587m                (3x type)       │
│    . / \  .     042 NE              (2x type)       │
│      S          you 028  right 14                   │
│ -> them  o you  alt level  theirs 42m               │
│                 them 51.48180, 0.00420              │
│                 their fix heard 4m ago              │
│                 me   51.47790, -0.00150  9 sat      │
│                 my fix now  4.2 km/h                │
│                 heard 12m  snr 6.2  1 hop  87%      │
│ Enter chat  P share my fix  M mark theirs  Esc back │
└─────────────────────────────────────────────────────┘
```

The arrow is the true bearing to their last reported position. With a
calibrated compass the rose turns heading-up, the cardinal letters ride the
rim, and the arrow simply points where to walk; the line under the bearing
reads `you 038 mag  right 14`. Without one the rose is north-up and the dot on
the rim is your course over ground from the receiver, drawn only while you are
moving faster than a slow walk, with the line reading `you 038 track`. Stand
still without a compass and the line says so instead of guessing. The age of
*their* position is printed separately from how long ago the node was
*heard*, because a tracker that reports telemetry but has lost its own fix is
exactly the case where the two differ and the difference is the whole search.

**Compass.** The board carries a Bosch BMM150 on the auxiliary bus of its
BMI270, and Debian's `bmc150_magn` and `bmi270` drivers publish both under
`/sys/bus/iio/devices` as world-readable `in_magn_*_raw` and `in_accel_*_raw`
files, with the driver's scale and mount matrix beside them. GNDHOG reads them
five times a second while a screen is showing a heading, projects the field
onto the plane the accelerometer says is level, and smooths the result on the
unit circle so 359 and 1 average to 0. **Menu → Position & GNSS → Compass**
shows the live heading, field strength, tilt, and calibration state:

- **C** starts a calibration. Turn the device slowly through a full circle,
  level, and press **C** again. That measures the board's own magnetism (the
  hard-iron offset of every ferrous thing soldered next to the chip) and the
  Earth's field strength here, which is how a car bonnet or a motor is later
  recognised as *disturbed* rather than believed. Fewer than three quarters of
  a circle is refused, with the count so far.
- **A**, while walking straight at more than 2.5 km/h with a GNSS fix and the
  device pointed the way you are going, aligns the chip's x axis with
  "forward". Until then the heading is a rotation of the truth by however the
  chip happens to sit on the PCB.
- `compass.declination` in `config.ini` (degrees, east positive) turns
  magnetic into true; `compass.mirror = 1` is for a chip mounted upside down,
  which reads as a compass that turns the wrong way.

Both calibrations are saved to `config.ini` the moment they succeed. A heading
is only offered to the Locate screen when it is calibrated, fresh, level
within 40 degrees, and undisturbed; otherwise Locate falls back to the GNSS
track and says which it is using.

**Marks.** **Menu → Position & GNSS → Mark this spot** names your own fix; on
the Locate screen, **M** names that node's last position instead, so a quad
tracker's final report survives the radio's node database forgetting it (or
the tracker's battery). The **M** key on the node list opens the saved places
with their range and bearing from where you stand; **Enter** locates one and
**D** deletes it. Marks are stored in `~/.local/share/bfcli/marks.txt`, one
tab-separated line per place, and are never transmitted.

**Quick messages.** **Tab** in any conversation. The built-in set is what gets
said on a flight line or a trail: "OK, all good here", "Landed safe", "Crashed,
going to look for it", "Need help at {pos}", "Heading back to the car", "On my
way to you", "Stay put, I will come to you", "Battery low, going quiet", "Copy
that", "Flying now, off the radio". `{pos}` expands to your coordinate when the
receiver has a fix and to "(no GNSS fix)" when it does not; it never expands to
a coordinate you no longer have. Replace any slot in `config.ini`:

```ini
quickmsg.3 = Need a spotter at the gate
quickmsg.9 =
```

A blank slot removes that line; a slot past the built-in ten appends one.

**SOS.** **Menu → Position & GNSS → SOS broadcast** composes
`SOS from <name>: need help at <lat>, <lon> alt <m> <hh:mm:ss> UTC`, shows it,
and on **Enter** broadcasts the text and a position packet to everyone on the
primary channel, then opens the broadcast conversation so a reply is seen.
Without a fix it says so in the text, gives the last known coordinate if there
ever was one, and sends no position packet. The mesh does not acknowledge a broadcast. SOS reaches the selected mesh channel only; it does not contact emergency services or guarantee help. Check for replies before deciding whether to repeat it.

**Auto-share.** **Menu → Position & GNSS → Auto-share position** is off.
Switching it on asks first, then transmits your fix every 2 minutes whenever
there is a current one; Enter steps it to 5, then 15, then off. It is a
session setting that is never saved, so a device picked up tomorrow is as quiet
as it was shipped. Closing the radio link switches it off too.

The receiver is read only. GNDHOG never writes to the GNSS UART and never
configures the receiver.

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

## Backups

Backups live under `~/.local/share/bfcli/backups`, unless `BFCLI_DATA_DIR`
points elsewhere. Mesh conversations live beside them in `mesh/`, and saved
places in `marks.txt`. Names follow
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
  capability/readback sequence and idle `status` temperature polling. None of
  it writes flash. Pit mode is requested only after operator confirmation and
  must be reported back before success.
- On a mesh link, **nothing is transmitted that you did not ask for**. The only
  automatic traffic is the configuration request and a five-minute heartbeat,
  and both stay inside the USB cable. Position sharing, a quick message, and an
  SOS are each a separate, confirmed action. The only periodic transmission is
  the auto-share beacon, which is off until switched on, asks before it starts,
  and is never saved: the next launch starts with it off.
- A mark is a file on this device. Saving one, or locating one, transmits
  nothing.
- A mesh conversation is deleted from this device only. The mesh has no recall,
  and the other station keeps its copy.

Remove propellers before bench work that can arm or drive motors. Software
confirmation is a guardrail. It is not a small orange force field.

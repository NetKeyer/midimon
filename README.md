# midimon

A command-line MIDI event monitor for Linux. It listens to one or more MIDI devices, decodes every event it receives, and prints each one with a microsecond-resolution timestamp.

It is built on the ALSA sequencer API, so it works with USB MIDI devices, hardware MIDI interfaces, virtual ports (e.g. `snd-virmidi`, software synths, DAWs), and anything else that shows up in `aconnect -l`.

## Features

- Lists all available MIDI input devices and ports, and lets you choose which to monitor
- Decodes all standard MIDI messages (see [Decoded events](#decoded-events))
- Kernel-generated timestamps with microsecond precision
- Monitor a single device, several devices, or all of them at once
- Optional filtering of noisy realtime messages (Timing Clock, Active Sensing)
- Optional raw byte display
- Plain text output, easy to pipe into `grep`, `tee`, or a log file

## Building

### Requirements

- Linux with ALSA (any modern distribution)
- A C compiler (gcc or clang) and `make`
- ALSA development headers:

| Distribution | Command |
|---|---|
| Debian / Ubuntu / Mint | `sudo apt install build-essential libasound2-dev` |
| Fedora / RHEL | `sudo dnf install gcc make alsa-lib-devel` |
| Arch | `sudo pacman -S base-devel alsa-lib` |
| openSUSE | `sudo zypper install gcc make alsa-devel` |

### Compile

```
make
```

Or without make:

```
gcc -O2 -Wall -o midimon midimon.c -lasound
```

### Install (optional)

```
sudo make install                    # installs to /usr/local/bin
make install PREFIX=$HOME/.local     # or install somewhere else, no sudo needed
sudo make uninstall                  # remove it
```

## Usage

```
midimon [-l] [-p client:port | -p name] [-a] [-f] [-w] [-r]
```

| Option | Description |
|---|---|
| `-l` | List available MIDI input ports and exit |
| `-p PORT` | Monitor this port. `PORT` is either `client:port` (e.g. `20:0`) or a client name. May be given more than once |
| `-a` | Monitor all available input ports |
| `-f` | Filter out Timing Clock and Active Sensing messages |
| `-w` | Show wall-clock time (`HH:MM:SS.uuuuuu`) instead of seconds since start |
| `-r` | Also show the raw MIDI bytes for channel messages |

With no `-p` or `-a`, midimon lists the available devices and prompts you to choose.

### Examples

List devices:

```
$ ./midimon -l
  #  Addr   Device / Port
  -  -----  -------------
  0   14:0   Midi Through : Midi Through Port-0
  1   20:0   Keystation 49 : Keystation 49 MIDI 1
```

Pick a device interactively:

```
$ ./midimon
  #  Addr   Device / Port
  -  -----  -------------
  0   14:0   Midi Through : Midi Through Port-0
  1   20:0   Keystation 49 : Keystation 49 MIDI 1

Select device number(s) (comma-separated, or 'a' for all): 1
```

Monitor a specific port, hiding clock messages:

```
./midimon -p 20:0 -f
```

Monitor by name (uses ALSA's name matching):

```
./midimon -p Keystation
```

Monitor everything, with wall-clock timestamps, and save a log:

```
./midimon -a -f -w | tee midi.log
```

Sample output:

```
  Seconds       Source  Event
     2.481337   20:0   Note On    ch  1  C4   ( 60)  vel 92
     2.733902   20:0   Control    ch  1  CC  64 = 127  (Sustain)
     2.910455   20:0   Pitch Bend ch  1    +512  (raw  8704)
     3.102118   20:0   Note Off   ch  1  C4   ( 60)  vel 0
```

Press **Ctrl-C** to quit.

## Decoded events

| Category | Events |
|---|---|
| Channel voice | Note On, Note Off, Polyphonic Key Pressure, Control Change (common controllers named), Program Change, Channel Pressure, Pitch Bend |
| Extended controllers | 14-bit controllers, RPN, NRPN |
| System common | MTC Quarter Frame, Song Position Pointer, Song Select, Tune Request |
| System realtime | Timing Clock, Start, Continue, Stop, Active Sensing, System Reset |
| System exclusive | Full hex dump of the message |

Notes on the output:

- **Channels** are displayed as 1-16.
- **Note names** use the convention that MIDI note 60 is C4. Some manufacturers and DAWs call it C3, so check against your gear's manual if octave numbers look off by one.
- **Note On with velocity 0** is displayed as a Note Off, since that is how many devices send note-offs. It is labelled `(via Note On)` so you can tell the difference.
- **Pitch Bend** is shown both as a signed value (-8192 to +8191, 0 = centre) and as the raw 14-bit value (0 to 16383, 8192 = centre).
- Output is line-buffered/flushed after every event, so it works fine in pipes.

## About the timestamps

The timestamps come from the ALSA sequencer, not from the program itself. Each incoming event is stamped by the kernel at the moment it arrives, using a real-time queue with nanosecond resolution; midimon prints this to the microsecond. This means the times are accurate even if the program is briefly delayed in reading or printing.

- **Default mode:** seconds since midimon started listening.
- **`-w` mode:** wall-clock time, computed by adding the event time to the start time. This has a small fixed offset (typically microseconds) relative to the true wall clock, which is fine for correlating events with logs but not for cross-machine sync.

Keep in mind that the *physical* timing of MIDI is limited by the hardware: classic 5-pin DIN MIDI runs at 31.25 kbaud, so a 3-byte message takes about 1 ms to transmit. Timestamps mark when the message was received by the computer, not when the key was pressed.

## Troubleshooting

**"No MIDI input devices found."**
Check that your device is detected: `aconnect -l` (from `alsa-utils`) or `amidi -l`. For USB devices, try `lsusb` and `dmesg | tail` to confirm it enumerated. Make sure your user can access the sound devices (usually membership in the `audio` group on older systems).

**"Cannot open ALSA sequencer"**
The `snd_seq` kernel module may not be loaded: `sudo modprobe snd-seq`.

**A device is missing from the list**
Only ports that export MIDI output (i.e. things that *send* MIDI) are listed. The list is taken when midimon starts, so devices plugged in afterwards require a restart. Some applications, such as software synths, only expose an input port and so will not appear.

**Nothing prints when I play**
Try `-a` to make sure you picked the right port. Many devices expose several ports, and some controllers send their data on a secondary one. Also check that another program is not exclusively holding the device.

**Output is flooded with Timing Clock**
Use `-f`. Many sequencers and drum machines send 24 clock messages per beat.

**"input overrun, events lost"**
The kernel's input buffer filled up faster than midimon could drain it. This is rare, but can happen with very heavy SysEx or many devices at once. Redirecting output to a file (rather than a slow terminal) usually helps.

**Virtual ports for testing without hardware**
Load a virtual MIDI device with `sudo modprobe snd-virmidi`, then send data to it, for example:

```
aplaymidi -p 128:0 somefile.mid
```

(Use `aconnect -l` to find the right port number.) You can also use `amidi` or any MIDI software to send events to a virtual port.

## Limitations

- Linux/ALSA only (no macOS, Windows, or JACK-MIDI-only setups)
- Monitors input only; it does not send MIDI
- The device list is a snapshot taken at startup; hot-plugged devices need a restart
- Displays events, but does not interpret higher-level protocols (e.g. General MIDI, MMC, or manufacturer-specific SysEx content)

## Files

| File | Purpose |
|---|---|
| `midimon.c` | Program source |
| `Makefile` | Build and install rules |
| `README.md` | This file |

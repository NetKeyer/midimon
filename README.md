# midimon

A command-line MIDI event monitor for Linux and macOS. It listens to one or more MIDI devices, decodes every event it receives, and prints each one with a microsecond-resolution timestamp.

It also decodes **MoMIDI** ([Morse over MIDI](https://github.com/NetKeyer/MoMIDI-Spec)), turning the Note On/Off and aftertouch messages sent by MoMIDI keyers into millisecond timing for key-down, key-up and the gaps between them.

It uses each system's native MIDI layer and has no third-party dependencies:

- **Linux:** the ALSA sequencer, so it works with USB MIDI devices, hardware MIDI interfaces, virtual ports (e.g. `snd-virmidi`), software synths, DAWs, and anything else that shows up in `aconnect -l`.
- **macOS:** CoreMIDI, so it works with USB MIDI devices, the IAC Driver, network MIDI sessions, virtual sources created by other apps, and anything else that shows up in Audio MIDI Setup.

Windows is not supported yet.

## Features

- Runs on Linux (ALSA) and macOS (CoreMIDI), written in plain C with no external libraries
- Lists all available MIDI input devices and ports, and lets you choose which to monitor (or picks the device for you if there is only one)
- Decodes all standard MIDI messages (see [Decoded events](#decoded-events))
- Timestamps taken by the operating system when each event arrives, with microsecond precision
- Decodes MoMIDI v0.1 timing, version queries and SysEx manufacturer IDs (see [MoMIDI decoding](#momidi-decoding))
- Show both normal MIDI and MoMIDI (the default), only normal MIDI, or only MoMIDI
- Monitor a single device, several devices, or all of them at once
- Optional filtering of noisy realtime messages (Timing Clock, Active Sensing)
- Optional raw byte display
- Plain text output, easy to pipe into `grep`, `tee`, or a log file

## Building

### Requirements

- A C compiler (gcc or clang) and `make`
- **Linux:** ALSA development headers:

| Distribution | Command |
|---|---|
| Debian / Ubuntu / Mint | `sudo apt install build-essential libasound2-dev` |
| Fedora / RHEL | `sudo dnf install gcc make alsa-lib-devel` |
| Arch | `sudo pacman -S base-devel alsa-lib` |
| openSUSE | `sudo zypper install gcc make alsa-devel` |

- **macOS:** the Xcode command line tools (`xcode-select --install`). CoreMIDI ships with macOS, so nothing else is needed.

### Compile

```
make
```

The Makefile detects your operating system and picks the matching MIDI backend. Without make:

```
# Linux
cc -O2 -Wall -o midimon midimon.c mm_parser.c backend_alsa.c -lasound

# macOS
cc -O2 -Wall -o midimon midimon.c mm_parser.c backend_coremidi.c \
   -framework CoreMIDI -framework CoreFoundation
```

### Install (optional)

```
sudo make install                    # installs to /usr/local/bin
make install PREFIX=$HOME/.local     # or install somewhere else, no sudo needed
sudo make uninstall                  # remove it
```

## Usage

```
midimon [-l] [-p client:port | -p name] [-a] [-f] [-w] [-r] [-m | -M]
```

| Option | Description |
|---|---|
| `-l` | List available MIDI input ports and exit |
| `-p PORT` | Monitor this port. May be given more than once. On Linux, `PORT` is `client:port` (e.g. `32:0`) or a client name. On macOS it is the number from the list (e.g. `1` or `1:0`) or part of a device or port name (not case-sensitive) |
| `-a` | Monitor all available input ports |
| `-f` | Filter out Timing Clock and Active Sensing messages |
| `-w` | Show wall-clock time (`HH:MM:SS.uuuuuu`) instead of seconds since start |
| `-r` | Also show the raw MIDI bytes for channel messages |
| `-M` | Disable MoMIDI decoding (normal MIDI lines only) |
| `-m` | MoMIDI only: hide the normal MIDI lines. Cannot be combined with `-M` |
| `-h` | Show usage |

With no `-p` or `-a`, midimon lists the available devices. If there is only one MIDI device it uses it automatically; if there are several it prompts you to choose. On Linux the virtual "Midi Through" port that ALSA always provides is not counted, so a machine with a single real keyer still selects it automatically. Use `-a` or `-p` to include or pick ports explicitly.

The `Addr` column of the list (and the `Source` column of the output) identifies the device. On Linux it is the ALSA `client:port` address. On macOS it is the CoreMIDI source number followed by `:0`. The examples in this document use Linux-style addresses; on a Mac the same keyer might show up as `0:0`.

By default both the normal MIDI decoding and the MoMIDI decoding are displayed.

### Examples

List devices:

```
$ ./midimon -l
  #  Addr   Device / Port
  -  -----  -------------
  0   14:0   Midi Through : Midi Through Port-0
  1   32:0   HaliKey Pro : HaliKey Pro MIDI 1
```

Pick a device interactively (this prompt appears only when there is more than one real device; with just one, midimon says `Only one MIDI device found, using it.` and starts listening):

```
$ ./midimon
  #  Addr   Device / Port
  -  -----  -------------
  0   14:0   Midi Through : Midi Through Port-0
  1   32:0   HaliKey Pro : HaliKey Pro MIDI 1

Select device number(s) (comma-separated, or 'a' for all): 1
```

Monitor a specific port, hiding clock messages:

```
./midimon -p 32:0 -f
```

Monitor by name (uses ALSA's name matching):

```
./midimon -p HaliKey
```

Monitor everything, with wall-clock timestamps, and save a log:

```
./midimon -a -f -w | tee midi.log
```

Watch only the Morse timing from a MoMIDI keyer:

```
./midimon -p 32:0 -m
```

Sample output (default mode):

```
  Seconds        Source  Event
     3.376603   32:0   Note On    ch  1  G#0  ( 20)  vel 127
               >> MoMIDI  left paddle/straight   DOWN  no timing info (vel 127), new epoch
     3.535088   32:0   Poly Press ch  1  G#0  ( 20)  pressure 1
     3.535090   32:0   Note Off   ch  1  G#0  ( 20)  vel 33
               >> MoMIDI  left paddle/straight   UP    +159 ms (1*126 + 33)  epoch+159 ms  held 159 ms  [host +158.487 ms]
```

The same events with `-m`:

```
     3.376603   32:0   >> MoMIDI  left paddle/straight   DOWN  no timing info (vel 127), new epoch
     3.535090   32:0   >> MoMIDI  left paddle/straight   UP    +159 ms (1*126 + 33)  epoch+159 ms  held 159 ms  [host +158.487 ms]
```

Press **Ctrl-C** to quit.

## Decoded events

| Category | Events |
|---|---|
| Channel voice | Note On, Note Off, Polyphonic Key Pressure, Control Change (common controllers named), Program Change, Channel Pressure, Pitch Bend |
| System common | MTC Quarter Frame, Song Position Pointer, Song Select, Tune Request |
| System realtime | Timing Clock, Start, Continue, Stop, Active Sensing, System Reset |
| System exclusive | Full hex dump of the message |

Notes on the output:

- **Channels** are displayed as 1-16.
- **Note names** use the convention that MIDI note 60 is C4. Some manufacturers and DAWs call it C3, so check against your gear's manual if octave numbers look off by one.
- **Note On with velocity 0** is displayed as a Note Off, since that is how many devices send note-offs. It is labelled `(via Note On)` so you can tell the difference.
- **Pitch Bend** is shown both as a signed value (-8192 to +8191, 0 = centre) and as the raw 14-bit value (0 to 16383, 8192 = centre).
- Output is flushed after every event, so it works fine in pipes.

## MoMIDI decoding

MoMIDI is a specification for sending Morse code over MIDI. The keyer measures the time between key events in its own firmware and sends that timing inside the MIDI messages, so the result doesn't depend on USB or MIDI latency. midimon implements **MoMIDI v0.1**; the spec is at <https://github.com/NetKeyer/MoMIDI-Spec>.

midimon decodes MoMIDI on every source it listens to; there is nothing to configure. If a device isn't a MoMIDI device, the only visible effect is that Note On/Off on notes 20, 21, 30 and 31 also produce a `>> MoMIDI` line. Use `-M` to turn it off.

### Keying notes

| Note | Meaning |
|---|---|
| 20 | Left paddle (or straight key) |
| 21 | Right paddle |
| 30 | Straight key |
| 31 | PTT |

Note On is key down, Note Off is key up. Other notes are ignored by the MoMIDI decoder. A Note On with velocity 0 is treated as key up, following normal MIDI convention.

### How timing is decoded

The sender reports the time since its previous keying event (any of the four notes) in two parts:

- The **Note On/Off velocity** carries the low part.
- An optional **Polyphonic Aftertouch** message for the same note, sent just before the Note On/Off, carries the high part in its pressure value. If it is missing, the high part is 0.

```
time_ms = velocity + pressure * 126
```

For example, `Poly Press pressure 5` followed by `Note On vel 73` means 73 + 5×126 = **703 ms** since the previous event. The largest value is 128 × 126 = 16128 ms.

A velocity of **127** (Note On) or **0** (Note Off) means "no timing information". The event is then used as the **epoch** ("zero ms") for the timed events that follow. Devices that never send timing always use these values, and midimon then simply shows each event as a new epoch.

### What the MoMIDI line shows

```
>> MoMIDI  left paddle/straight   UP    +159 ms (1*126 + 33)  epoch+159 ms  held 159 ms  [host +158.487 ms]
```

| Field | Meaning |
|---|---|
| key name and `DOWN` / `UP` | Which key changed, and how |
| `+159 ms (1*126 + 33)` | Device-measured time since the previous keying event, and how it was computed from the aftertouch and velocity. The `(...)` part appears only when an aftertouch message was used |
| `no timing info (vel N), new epoch` | Shown instead of the time for velocity 0/127 events |
| `epoch+159 ms` | Device time since the last epoch: the sum of the reported times. Use it to compare events that are several steps apart |
| `held 159 ms` | On a key up, how long that key was down, according to the device clock. This is the length of the dit or dah. Shown only when the key-down was seen after the current epoch |
| `[host +158.487 ms]` | For comparison: the time between the two events as seen by the computer, measured with the kernel timestamps. The difference from the device time shows the USB/MIDI jitter |

State is tracked separately for each source port, so several MoMIDI devices can be monitored at once without mixing up their timing. If midimon is started in the middle of a keying sequence, the first timed event is measured from an unseen earlier event, so `epoch+` values are relative to that point and `held` appears only once a full down/up pair has been seen.

### Version queries and SysEx

- **Song Select 0** is shown as `version query`.
- **Song Select N** (N ≠ 0) is shown as a version report, e.g. `version v0.1 (0x01)`. The upper 3 bits are the major version and the lower 4 bits the minor version.
- **SysEx** messages whose manufacturer ID is one MoMIDI has assigned (currently 0x01 Lynovations, 0x02 Halibut Electronics, 0x03 Remote Ham Radio) get an extra line with the manufacturer and the number of data bytes. Manufacturer ID 0x00 is not decoded, because in standard MIDI it introduces a 3-byte extended manufacturer ID. Other SysEx messages appear only as the normal hex dump.

midimon is a passive monitor: it **does not answer** version queries or send anything to the device.

### Compatibility

midimon follows v0.1 of the spec. Firmware written for the earlier v0.0 draft, which sent the high timing bits as a Control Change message and used a different meaning for velocity 0, will not decode the same way.

## About the timestamps

The timestamps come from the operating system, not from the program itself. Each incoming event is stamped when it arrives, so the times are accurate even if midimon is briefly delayed in reading or printing. midimon prints them to the microsecond.

- **Linux:** the ALSA sequencer stamps each event in the kernel, using a real-time queue with nanosecond resolution.
- **macOS:** CoreMIDI stamps each packet with the host time at which the system received it (`mach_absolute_time`), which midimon converts to nanoseconds.

- **Default mode:** seconds since midimon started listening.
- **`-w` mode:** wall-clock time, computed by adding the event time to the start time. This has a small fixed offset (typically microseconds) relative to the true wall clock, which is fine for correlating events with logs but not for cross-machine sync.

On Linux, if for some reason the kernel does not stamp an event, midimon falls back to the system clock at the moment it reads the event, and prints this note once on stderr:

```
note: kernel timestamps unavailable, using read-time timestamps instead
```

The timestamps are still valid and increasing, but they then include a little scheduling jitter.

Keep in mind that the *physical* timing of MIDI is limited by the hardware: classic 5-pin DIN MIDI runs at 31.25 kbaud, so a 3-byte message takes about 1 ms to transmit. Timestamps mark when the message was received by the computer, not when the key was pressed. For Morse timing, the MoMIDI device-side times are the more accurate measure, which is the whole point of the protocol.

## Troubleshooting

**"No MIDI input devices found."**
On Linux, check that your device is detected: `aconnect -l` (from `alsa-utils`) or `amidi -l`. On macOS, open Audio MIDI Setup (Window > Show MIDI Studio) and check that the device appears there and is not greyed out. For USB devices, try `lsusb` and `dmesg | tail` to confirm it enumerated. Make sure your user can access the sound devices (usually membership in the `audio` group on older systems).

**"Cannot open ALSA sequencer"** (Linux)
The `snd_seq` kernel module may not be loaded: `sudo modprobe snd-seq`.

**"warning: cannot start queue" or the "kernel timestamps unavailable" note** (Linux)
The timestamp queue could not be started, so midimon is using read-time timestamps. Please check that you are running a current version of midimon (older versions had a bug here) and that the `snd_seq` and `snd_timer` modules are loaded.

**A device is missing from the list**
Only ports that export MIDI output (i.e. things that *send* MIDI) are listed. The list is taken when midimon starts, so devices plugged in afterwards require a restart. Some applications, such as software synths, only expose an input port and so will not appear.

**Nothing prints when I play**
Try `-a` to make sure you picked the right port. Many devices expose several ports, and some controllers send their data on a secondary one. Also check that another program is not exclusively holding the device.

**No `>> MoMIDI` lines**
MoMIDI lines appear only for Note On/Off on notes 20, 21, 30 and 31, and only if `-M` was not given. Check the normal MIDI lines to see which notes your device actually sends.

**MoMIDI times look wrong**
Check the raw messages with `-M -r` (normal MIDI only, with raw bytes). The velocity and the aftertouch pressure just before it should combine as `velocity + pressure × 126`. Also confirm that your device follows v0.1 of the spec (see [Compatibility](#compatibility)).

**Output is flooded with Timing Clock**
Use `-f`. Many sequencers and drum machines send 24 clock messages per beat.

**"input overrun, events lost"**
The kernel's input buffer filled up faster than midimon could drain it. This is rare, but can happen with very heavy SysEx or many devices at once. Redirecting output to a file (rather than a slow terminal) usually helps.

**Virtual ports for testing without hardware (macOS)**
Enable the IAC Driver (Audio MIDI Setup > Window > Show MIDI Studio > double-click IAC Driver > tick "Device is online"). It appears as a MIDI source that other programs can send to.

**Virtual ports for testing without hardware (Linux)**
Load a virtual MIDI device with `sudo modprobe snd-virmidi`, then send data to it, for example:

```
aplaymidi -p 128:0 somefile.mid
```

(Use `aconnect -l` to find the right port number.) You can also use `amidi` or any MIDI software to send events to a virtual port.

## Limitations

- Linux (ALSA) and macOS (CoreMIDI) only. No Windows support yet, and no JACK-MIDI-only setups
- Monitors input only; it does not send MIDI, and does not answer MoMIDI version queries
- The device list is a snapshot taken at startup; hot-plugged devices need a restart
- The macOS backend uses CoreMIDI's classic MIDI 1.0 byte-stream API (deprecated by Apple in favour of the MIDI 2.0 API, but supported on all macOS versions); MIDI 2.0 devices are seen through Apple's MIDI 1.0 translation
- Beyond MoMIDI, it displays events but does not interpret higher-level protocols (e.g. General MIDI, MMC, or manufacturer-specific SysEx content)
- MoMIDI decoding covers v0.1 of the spec only

## Files

| File | Purpose |
|---|---|
| `midimon.c` | Platform-independent part: options, output formatting, MoMIDI decoding |
| `backend.h` | The interface each operating-system backend implements |
| `mm_event.h` | The neutral MIDI event type passed from a backend to the core |
| `backend_alsa.c` | Linux backend (ALSA sequencer) |
| `backend_coremidi.c` | macOS backend (CoreMIDI) |
| `mm_parser.c`, `mm_parser.h` | Turns a raw MIDI byte stream into events (used by the macOS backend; handles running status, interleaved realtime bytes and split SysEx) |
| `Makefile` | Build and install rules; picks the backend for your OS |
| `README.md` | This file |

To add another platform, write a new `backend_xxx.c` that implements `backend.h` and add it to the Makefile. The core does not need to change.

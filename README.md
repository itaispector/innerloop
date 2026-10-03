# innerloop

A minimal, self-built macOS Core Audio driver for routing app output
straight into your DAW.

`InnerLoop` is a Core Audio Server Plug-In (HAL driver) — no kernel
extension — that adds one virtual device with a 2-channel input and a
2-channel output sharing a single ring buffer. Whatever an app plays to
the device's output comes back out its input, so any app's output (e.g.
Spotify) can be captured as a recording input (e.g. in Logic Pro).

```
[App: Spotify] --(CoreAudio output)--> [InnerLoop: Output Stream]
                                               |
                                (shared ring buffer, memcpy per IO cycle)
                                               |
[App: Logic Pro] <--(CoreAudio input)-- [InnerLoop: Input Stream]
```

Feedback-loop rule: a DAW reading InnerLoop as input must send its own
output to real hardware, never back to InnerLoop (directly or via a
Multi-Output Device that contains it). See `docs/ROUTING.md`.

## Repo layout

```
Makefile                  forwards targets to driver/Makefile (run make from the root)
driver/
  Info.plist              plug-in bundle metadata + factory UUID registration
  Makefile                builds/signs/installs the .driver bundle with clang
  include/
    InnerLoopTypes.h       shared IDs, formats, buffer sizing constants
    InnerLoopRingBuffer.h
  src/
    InnerLoopDriver.c      AudioServerPlugIn vtable, object/property model, IO cycle
    InnerLoopRingBuffer.c  mutex-guarded circular buffer shared by both streams
scripts/
  install.sh / uninstall.sh   thin wrappers around `make install` / `make uninstall`
docs/
  ROUTING.md               Multi-Output Device setup, feedback-loop avoidance
  TESTING.md               install/signal-path/robustness checklist
```

## Build & install (macOS only)

This only builds and runs on macOS — it links against `CoreAudio.framework`
and installs into `/Library/Audio/Plug-Ins/HAL`, which loads into
`coreaudiod`.

```sh
make install        # ad-hoc signs, installs, restarts coreaudiod
```

Run this from the repo root (it forwards to `driver/Makefile`) or from
inside `driver/` — both work.

Use a real signing identity for anything beyond local testing:

```sh
SIGN_ID="Developer ID Application: Your Name (TEAMID)" make install
```

Then open **Audio MIDI Setup.app** to confirm `InnerLoop` shows up, set up
routing per `docs/ROUTING.md`, and run through `docs/TESTING.md`.

```sh
make uninstall       # removes the driver, restarts coreaudiod
```

## Current status

Phases 0–6 of the build plan (scaffold, plug-in entry point, device/stream
object model, shared ring buffer, IO cycle callbacks, build/sign/install
tooling, routing docs) are implemented. Phase 7 (testing) and Phase 8
(stretch goals — menu bar helper, multiple devices, configurable channel
count) require running on real macOS hardware and are tracked as
checklists in `docs/TESTING.md`.

Volume/mute controls are intentionally left out for now (noted as optional
in the original plan) — the device exposes only the input/output streams.

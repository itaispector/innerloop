# Testing & validation checklist

Run these on macOS after `make install` (or `scripts/install.sh`).

## Install sanity

- [ ] `sudo killall coreaudiod` does not crash/hang the daemon; it comes
      back up on its own within a second or two.
- [ ] `system_profiler SPAudioDataType` lists **InnerLoop** with a
      2-in/2-out configuration.
- [ ] InnerLoop appears in **Audio MIDI Setup.app**'s device list, and its
      input/output level meters move independently.
- [ ] `log show --predicate 'process == "coreaudiod"' --last 2m` shows no
      crash/exception tied to `com.itai.virtualaudio.loopback` right after
      the restart.

## Signal path

- [ ] Set up routing per `ROUTING.md`.
- [ ] Play audio from Spotify (or any app) → confirm signal appears on
      InnerLoop's input meters in Audio MIDI Setup and in Logic's input
      meter.
- [ ] Confirm you still hear audio through real speakers/headphones
      (validates the Multi-Output Device is actually combining both
      outputs, not just silently swallowing one).
- [ ] Record a short take in Logic and play it back — audio should be
      present, in sync, without obvious added latency beyond the
      configured buffer size.

## Robustness

- [ ] Change the buffer size in Logic's audio preferences (e.g. 128 →
      1024 → 128 frames) while idle, then confirm audio still passes
      after each change without restarting anything.
- [ ] Change the nominal sample rate (44100 ↔ 48000) via Audio MIDI Setup
      and confirm InnerLoop follows without `coreaudiod` needing a manual
      restart.
- [ ] Start/stop the source app and the DAW repeatedly — no crash, no
      stuck audio, no leftover "ghost" playback.
- [ ] Sleep and wake the Mac while the routing is set up; confirm audio
      still flows afterward without reinstalling/rebooting.

## Feedback-loop check (do this deliberately, once)

- [ ] Temporarily set Logic's **output** to InnerLoop (breaking the "never
      loop back" rule) with input monitoring/software monitoring enabled,
      at a low volume, and confirm you hear a runaway feedback loop. This
      proves the mechanism actually works and that InnerLoop is not
      silently deduplicating or filtering its own signal.
- [ ] Immediately revert Logic's output back to real hardware.

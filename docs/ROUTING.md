# Routing setup

Once `InnerLoop.driver` is installed and `coreaudiod` has restarted, it
shows up as a normal device named **InnerLoop** with one 2-channel input
and one 2-channel output, backed by the same shared ring buffer: whatever
is written to its output is what comes back out of its input.

By itself, setting an app's output straight to InnerLoop means you no
longer hear it on your speakers — it only goes into the ring buffer. Use
a **Multi-Output Device** to send it to both places at once.

## 1. Create a Multi-Output Device

1. Open **Audio MIDI Setup.app** (`/Applications/Utilities`).
2. Click the **+** button at the bottom-left → **Create Multi-Output Device**.
3. In the device list on the right, check both:
   - **InnerLoop**
   - your real output device (built-in speakers, headphones, audio interface)
4. Pick your real hardware device as the **primary/clock source** (the
   top-most checked device, or set via the "Clock Source" dropdown) —
   InnerLoop is a virtual device with no real clock, so it should follow
   the hardware's clock, not the other way around.
5. If you hear glitches/clicks, check **Drift Correction** on the
   sub-device that is *not* the clock source (usually InnerLoop).

## 2. Send the source app's audio through it

Pick one:
- Set the app itself (Spotify, etc.) to output to the Multi-Output Device,
  if it has its own output device setting, **or**
- Set the Multi-Output Device as the **system output** (Sound settings →
  Output), so everything routes through it.

## 3. Set up your DAW (Logic Pro, etc.)

- **Input** → InnerLoop
- **Output** → your real hardware device directly (built-in output,
  interface) — **never** InnerLoop, and never the Multi-Output Device
  that contains InnerLoop.

This last rule is the actual feedback-prevention mechanism: InnerLoop's
input only plays back whatever was written to its output. If the DAW's
own output were also InnerLoop (directly or via the Multi-Output Device),
the DAW would be feeding its own input back into itself through the ring
buffer, and any monitoring/loopback in the signal chain turns that into a
runaway feedback loop.

## Recording a specific app only

A Multi-Output Device is *global* — every app using it as output goes
into the mix. If you want to isolate one app, don't set that Multi-Output
Device as the system default; instead, only change the output for the
specific app you want to capture (many apps have a per-app output
device setting; otherwise, quit other apps that might be playing audio
through the same system output while recording).

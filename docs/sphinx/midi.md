# MIDI

`tanh::Midi` (`include/tanh/midi/`, namespace `thl::midi`) defines the form MIDI has
inside tanh-lib and carries it to the audio thread. It depends on Core only.

## One internal format: Universal MIDI Packets

Every message is a Universal MIDI Packet, `thl::midi::Ump` — one to four 32-bit words
whose first nibble is the message type:

| Type | Words | Carries |
|---|---|---|
| 1 System | 1 | system common and real-time (clock, start, stop, song position, ...) |
| 2 MIDI 1.0 channel voice | 1 | notes, controllers, pitch bend at 7 / 14 bits |
| 3 SysEx7 | 2 | up to six bytes of a System Exclusive, with start / continue / end |
| 4 MIDI 2.0 channel voice | 2 | 16-bit velocity, 32-bit controllers, per-note controllers and pitch bend |

MIDI 1.0 input is wrapped as types 1 to 3 without any change of resolution and without
interpretation: a note-on with velocity 0 stays one, RPN and NRPN stay the controller
messages they arrived as. MIDI 2.0 input is type 4. Nothing is lost on the way in, and
MPE needs no special case at this level — it is ordinary channel voice messages on
several channels.

A consumer handles both channel voice types. Code that only wants MIDI 1.0 converts
instead (below).

## Getting packets

`Midi1StreamParser` turns a MIDI 1.0 byte stream into packets. It accepts what a serial
source really delivers: messages split anywhere across calls, several per call, running
status, and system real-time bytes in the middle of another message. Use one parser per
source.

```cpp
thl::midi::Midi1StreamParser parser;
parser.parse(bytes, size, [&](const thl::midi::Ump& ump) {
    queue.push(ump, thl::core::uptime_ns());
});
```

`UmpByteReader` does the same for a UMP stream delivered as bytes in network byte order
(Android's form for MIDI 2.0 devices), and `for_each_ump()` splits a buffer of host-order
words (CoreMIDI's form).

## Plain MIDI 1.0 out

`convert_to_midi1()` turns one packet into MIDI 1.0 short messages:

```cpp
thl::midi::Midi1Messages messages;
const std::size_t count = thl::midi::convert_to_midi1(ump, messages);
for (std::size_t i = 0; i < count; ++i) {
    handle(messages[i].m_bytes.data(), messages[i].m_size);
}
```

Types 1 and 2 convert one to one. Type 4 is scaled down by the MIDI 2.0 translation
rules: values keep their top 7 bits (pitch bend 14), a note-on never ends up with
velocity 0, a program change with a valid bank becomes bank select plus program change,
and a registered or assignable controller becomes the RPN / NRPN sequence. What MIDI 1.0
cannot express — per-note controllers, per-note pitch bend, relative controllers — yields
no message. System Exclusive is not a short message and is not converted.

## Devices

`DeviceManager` lists the MIDI inputs of the machine, opens them, follows hot-plugging and
puts what they send into one `InputQueue`, each packet stamped with its arrival time and
the slot of its input (`DeviceInfo::m_source`).

```cpp
thl::midi::DeviceManager midi;                       // opens every input, now and later
midi.set_device_change_callback([&] { refresh_device_list(); });

for (const thl::midi::DeviceInfo& device : midi.input_devices()) {
    show(device.m_name, device.m_transport, device.m_open);
}

// audio thread
midi.input_queue().drain([&](const thl::midi::TimedUmp& packet) { on_midi(packet); });
```

`DeviceManagerConfig` chooses the protocol to receive (`Protocol::Midi2` by default), the
queue capacity, whether inputs are opened automatically, and whether System Exclusive is
delivered (off by default: a dump would crowd the performance data out of the queue).
`DeviceInfo::m_persistent_key` is the key under which to remember a device across
reconnects; `m_id` is only valid while it is connected.

The backend is CoreMIDI on macOS and iOS and `android.media.midi` on Android. Elsewhere
`DeviceManager::supported()` is false, the list stays empty, and the queue carries what
the application pushes itself.

### macOS and iOS

- The port is opened with the protocol asked for and CoreMIDI converts: with `Midi2`, a
  MIDI 1.0 device arrives as MIDI 2.0 channel voice messages that scale back to exactly
  the bytes it sent; with `Midi1`, a MIDI 2.0 device arrives as MIDI 1.0.
- Packets are pushed from CoreMIDI's own high-priority receive thread, with CoreMIDI's
  timestamp.
- `create_virtual_input()` adds a destination other apps on the device can send to. On
  iOS this needs the `audio` background mode.
- Bluetooth and network MIDI devices appear as ordinary inputs once the system has
  connected them. Connecting them is the application's job (on iOS,
  `CABTMIDICentralViewController`).
- CoreMIDI serves a whole process through the run loop of the thread that created the
  process's first MIDI client. Without that run loop running, device changes are never
  seen — not even by asking for the list again. If the application has no CoreMIDI client
  of its own when the first `DeviceManager` is constructed, the backend starts a thread
  that takes this role and runs until the process ends; a manager therefore works from
  any thread and in a command-line tool. An application that wants CoreMIDI on its main
  run loop creates its own client there before the first manager.

### Android

Receiving from `android.media.midi` needs Java, which a CMake library cannot deliver, so
the app provides three things:

1. **The Java class.** Add tanh-lib's `android/java` folder to the app's sources:

   ```groovy
   android.sourceSets.main.java.srcDirs += "<tanh-lib>/android/java"
   ```

   and keep it from R8 with `android/consumer-rules.pro` (`proguardFiles`).
2. **The JavaVM.** Call `thl::midi::set_android_java_vm(vm)` in `JNI_OnLoad`. It has to
   be there: only on the thread that loads the library can the class be found.
3. **The manifest entry**, if MIDI is optional for the app:

   ```xml
   <uses-feature android:name="android.software.midi" android:required="false"/>
   ```

Until the first two are in place `running()` is false and nothing is listed. USB and
virtual devices need no permission; works from API 23.

- Android does not convert between the protocols. A device arrives in its own: MIDI 1.0
  bytes as type 2 packets, and — with `Protocol::Midi2`, on API 33 and later — devices of
  the Universal MIDI Packet transport as they send. A consumer handles both channel voice
  types, or converts with `convert_to_midi1()`.
- Opening is asynchronous: `open_input()` returns once the request is made, and the
  device change callback fires when `DeviceInfo::m_open` has changed.
- Packets are pushed from the thread Android runs per port, an ordinary thread, with the
  timestamp Android attached (`System.nanoTime()`, the clock of `uptime_ns()`).
- Bluetooth LE devices are listed once something has connected them; scanning and
  connecting is not part of the backend yet. There is no virtual input.

## Testing without hardware

Both backends are tested against virtual MIDI devices, so no keyboard is needed:

- **macOS and iOS**: the `CoreMidi.*` tests create virtual CoreMIDI ports in the test
  process. They run with the desktop tests on macOS and in the iOS simulator test bundle.
- **Android**: `test/android/midi/run.sh` (`just test-android-midi`) builds a small test
  app and runs it on the connected device or emulator. The app declares a virtual MIDI
  1.0 device and a virtual Universal MIDI Packet device (`MidiDeviceService`,
  `MidiUmpDeviceService`; the latter needs Android 15) and drives a `DeviceManager`
  against them; a second app that is only a virtual device is installed and uninstalled
  meanwhile to play a device being plugged in and pulled out.

What this leaves to real hardware: the timing of actual USB and Bluetooth devices,
Bluetooth pairing, and how a USB MIDI 2.0 device presents itself on Android.

## To the audio thread

`InputQueue` is a bounded lock-free queue of `TimedUmp`: the packet, its arrival time in
the time base of `thl::core::uptime_ns()`, and the slot of the input it came from.

- Any number of threads may `push()`; each platform input can have its own receive
  thread. Packets of one thread stay in order.
- One thread drains, normally at the top of the audio callback:

```cpp
queue.drain([&](const thl::midi::TimedUmp& packet) { on_midi(packet); });
```

- Neither end allocates or blocks.
- When the queue is full the new packet is dropped and `dropped()` rises. A consumer that
  sees the count change has missed messages, possibly note-offs, and should release its
  sounding notes.

The arrival time travels with every packet so that it can be placed inside the audio
block it belongs to; the next section does that.

## Audio and MIDI in one callback

`thl::AudioMidiProcessor` is the top of an engine as a host sees it — the shape of a
plugin's process callback:

```cpp
class MyEngine : public thl::AudioMidiProcessor {
    void prepare(double sample_rate, std::size_t max_block_size, std::size_t num_channels) override;
    void process(thl::core::BufferView audio, std::span<const thl::BlockEvent> events) override;
};
```

`process()` gets one block: the audio, in place (input on entry, output on return), and
the events that fall inside it, in ascending `m_sample_offset`. An event is a MIDI packet
today; the type tag leaves room for parameter changes later without changing the
signature. This is the interface of the whole engine, not of the DSP building blocks
(`dsp::BaseProcessor`), which keep taking audio and modulation.

Who calls it is the host:

- **In an app**, `thl::AudioMidiCallback` (`tanh/midi/AudioMidiCallback.h`) is the host.
  It is an `AudioIODeviceCallback`: register it with the `AudioDeviceManager`, give it the
  `DeviceManager`'s input queue, and it runs the engine from the hardware.

  ```cpp
  thl::midi::DeviceManager midi;
  thl::AudioMidiCallback callback(engine, midi.input_queue(), 2);
  audio.add_duplex_callback(&callback);
  ```

- **In a plugin**, a wrapper copies the host's events into `BlockEvent`s with the offsets
  the host gave them and calls `process()` itself.

### Alignment

The audio callback carries no timestamp, so `StreamClock` builds the stream's clock from
the moments the blocks are asked for. Those are poor taken at face value — callbacks come
late, in bursts, or as chunks of one device period — but a block is never asked for
earlier than needed, so the earliest requests of the last half second lie on a straight
line, which also follows the drift between the audio clock and the system clock. Each
block gets the stretch of system time a fixed delay before it; a packet whose timestamp
falls into that stretch gets the matching sample offset, one that is not due yet waits
for its block (`BlockEventCollector`).

What that guarantees:

- Two events keep their distance in the audio to the sample, whatever the block size and
  however irregular the callbacks.
- Every event is late by the same amount: one full-size block
  (`AudioMidiCallback::midi_latency_ns()`). That is the least exact placement allows — a
  block is rendered as a whole, so what arrives while it is asked for can only go into
  the next one — and it is what a plugin host gives a plugin and what JUCE's
  `MidiMessageCollector` does. The way to less delay is a smaller audio buffer, which
  shortens the audio output delay and this one together.
- A stall of more than 100 ms (a device that skipped, an app that was suspended) restarts
  the clock at once; a shorter one is absorbed within half a second.

The delay is measured against the timestamp the platform attached to the message. It
does not include the audio output latency of the device, which is the same for everything
the engine renders.

Two settings in `BlockEventCollectorConfig` change the trade:

- `m_latency_ns` (0 by default) adds slack on top of the block, for a platform whose
  MIDI thread hands messages over later than their timestamps; such a message otherwise
  lands at the start of the next block.
- `m_placement = EventPlacement::Immediate` gives up exact placement for the shortest
  delay: every event goes to the start of the first block after it arrived, so it is late
  by anything between nothing and one block, and timing is quantised to the block. Worth
  it only with very small blocks.

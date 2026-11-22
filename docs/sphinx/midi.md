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

The arrival time travels with every packet so that a consumer can place it inside the
block; the audio callback carries no timestamp of its own yet, so the mapping from
arrival time to a sample offset is up to the consumer.

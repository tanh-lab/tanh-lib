Components
==========

tanh-lib is eight independently buildable library targets. Each is an
``add_library`` of its own, exported as ``tanh::<Component>`` both in-tree and
from the installed package, and enabled by a ``TANH_BUILD_<COMPONENT>`` option.
Everything lives in namespace ``thl``, inside the inline ABI namespace
``thl::v<major>_<minor>`` (``TANH_ABI_NAMESPACE``), so that two copies of
tanh-lib in one process stay apart; code spells ``thl::`` (see
:doc:`symbol_visibility`). The core is defined by ``tanh_add_core_library()``
(``cmake/core-library.cmake``), which an embedder also uses for a private copy.

.. graphviz::
   :align: center

   digraph components {
       rankdir = BT;
       node [shape=box, style="rounded,filled", fillcolor="#eef3fb", fontname="Helvetica", fontsize=11];
       edge [color="#5b6b82", arrowsize=0.7];

       core       [label="tanh::Core\nDispatcher, Logger, RCU,\nLockFreeQueue, Buffer, WavReader"];
       state      [label="tanh::State\nhierarchical parameters,\nRCU reads, JSON"];
       dsp        [label="tanh::DSP\nprocessors, effects, sampler,\nslicing, granular, Rings resonator"];
       modulation [label="tanh::Modulation\nmodulation matrix,\nchange-point sub-blocking"];
       audio_io   [label="tanh::AudioIO\ndevice I/O over miniaudio"];
       net        [label="tanh::Net\nHttpClient, Sha256,\nAssetStore"];
       graph_node [label="tanh::Graph\nprocessor graph,\nlatency compensation"];
       midi       [label="tanh::Midi\nUniversal MIDI Packets,\ndevice input, InputQueue"];
       link       [label="tanh::Link\nLinkSession (Ableton Link)", style="rounded,dashed"];

       state -> core;
       dsp -> core;
       audio_io -> core;
       net -> core;
       midi -> core;
       graph_node -> core;
       graph_node -> dsp;
       modulation -> core;
       modulation -> state;
       modulation -> dsp;
       link -> dsp;
   }

Core
----

``tanh::Core`` (``include/tanh/core/``, ``src/core/``) is the foundation the
other components build on:

- :cpp:class:`thl::Dispatcher` — event messaging between subsystems
- ``thl::Logger`` — leveled logging with pluggable sinks and a
  real-time path, ``thl::Logger::rt``, that never allocates or blocks
- :cpp:class:`thl::RCU` — lock-free read-copy-update for data the audio thread
  reads while another thread replaces it. A writer publishes in one call
  (``update()``, ``replace()``) or in two steps: ``stage_update()`` /
  ``stage_replace()`` do everything that can fail, ``commit()`` cannot fail and
  allocates nothing, so a change that spans several RCUs is staged in full and
  then committed in full
- :cpp:class:`thl::core::LockFreeQueue` — a bounded lock-free MPMC queue
- :cpp:class:`thl::core::Buffer`, :cpp:class:`thl::core::BufferView`,
  :cpp:class:`thl::core::RingBuffer`, :cpp:class:`thl::core::MemoryBlock` —
  header-only containers with no dependency on the logger, so a permissively
  licensed consumer can embed them unchanged
- ``read_wav`` (``WavReader.h``) — a header-only WAV decoder
- :cpp:class:`thl::core::Thread` — an OS thread with a requested scheduling
  class

Core links no platform system library by default; the Linux journald sink is
opt-in through ``TANH_WITH_JOURNALD``. Allocation failure in the containers
throws ``std::bad_alloc``; contract violations ``assert``.

State
-----

``tanh::State`` (``include/tanh/state/``) stores parameters in a hierarchy
addressed by dot-separated paths such as ``"oscillator.frequency"``. Reads are
RCU-protected, so the audio thread can look a value up without taking a lock;
numeric parameter types (``double``, ``float``, ``int``, ``bool``) are fully
real-time safe. Serialization to and from JSON goes through nlohmann_json.
Depends on Core.

DSP
---

``tanh::DSP`` (``include/tanh/dsp/``) holds the processors: synth voices,
effects, filters, analysis helpers and the Rings resonator model, which is also
available as the separate ``tanh::Resonator`` target for consumers that only
need it. Processors inherit :cpp:class:`thl::dsp::BaseProcessor` and follow the
``prepare()`` / ``process()`` contract; ``process_modulated()`` splits a block
at change points for sample-accurate automation.

Sample playback is split into components usable on their own:

- ``thl::dsp::sampler`` — :cpp:class:`thl::dsp::sampler::SamplePlayer`, a
  varispeed player / looper with click-free loop crossfades, zero-crossing
  snap and reverse playback, plus its building blocks (see :doc:`sampler`)
- ``thl::dsp::slicing`` — :cpp:struct:`thl::dsp::slicing::SliceMap` and the
  offline :cpp:class:`thl::dsp::slicing::TransientSlicer` (see :doc:`slicing`)
- ``thl::dsp::pitch`` — :cpp:class:`thl::dsp::pitch::PitchBank`, pitch-shifted
  copies of a sample (see :doc:`pitch`)
- ``thl::dsp::granular`` — :cpp:class:`thl::dsp::granular::GrainEngine`, a
  standalone granulator, and ``GrainProcessorImpl``, a voice that composes the
  engine and the player behind a parameter enum

Depends on Core; Signalsmith Stretch is fetched and linked privately for the
pitch banks.

Modulation
----------

``tanh::Modulation`` (``include/tanh/modulation/``) routes modulation sources
(LFOs, UI-driven input queues and any other
:cpp:class:`thl::modulation::ModulationSource`) to DSP parameters through the
:cpp:class:`thl::modulation::ModulationMatrix`. The matrix records change
points per block and drives the processors' sub-blocking. See :doc:`modulation`
for the input-event design. Depends on Core, State and DSP.

AudioIO
-------

``tanh::AudioIO`` (``include/tanh/audio-io/``) is the cross-platform audio
device layer over miniaudio: device enumeration, playback, capture and duplex
streams through :cpp:class:`thl::AudioDeviceManager`, with
platform-specific code for iOS and Android. See :doc:`audio_io` for the Android
Bluetooth notes. Depends on Core.

Net
---

``tanh::Net`` (``include/tanh/net/``) delivers versioned file sets over HTTPS,
for model or sample packs too large to bundle: :cpp:class:`thl::net::HttpClient`
(GET to a file or string, ``Range`` resume, progress with cancellation) over the
platform HTTP stack, :cpp:class:`thl::net::Sha256` for verification and
:cpp:class:`thl::net::AssetStore` for the verified, atomic install into
``<root>/<id>/<version>/``. Off by default (``TANH_BUILD_NET``) so nothing that
embeds ``tanh::Core`` inherits a network stack. Depends on Core.

Link
----

``tanh::Link`` (``include/tanh/link/``) wraps one Ableton Link session,
:cpp:class:`thl::link::LinkSession`: the Link C++ SDK on macOS, Linux and
Windows, LinkKit on iOS. The audio side,
:cpp:class:`thl::dsp::transport::LinkTransportClock`, lives in DSP and talks to
the session through the ``LinkBackend`` seam, so it is testable without the
SDK. Off by default (``TANH_BUILD_LINK``) because Link brings its own licence
terms (GPLv2+ or proprietary). See :doc:`transport`. Depends on DSP.

Graph
-----

``tanh::Graph`` (``include/tanh/graph/``) runs ``BaseProcessor`` nodes as one
processor: :cpp:class:`thl::graph::Graph` is edited on the control thread and
handed to the audio thread at ``commit()`` with a short fade, nests, mixes each
node's sources and blends its wet signal, and compensates latency: the faster
inputs of every node and a node's dry path are delayed to match each node's
``latency_samples()``, and the graph reports its own. A bypassed node coming
back is ``reset()``. Off by default (``TANH_BUILD_GRAPH``). Depends on Core and
DSP.

Midi
----

``tanh::Midi`` (``include/tanh/midi/``) is the form MIDI has inside tanh-lib:
every message is a Universal MIDI Packet (:cpp:struct:`thl::midi::Ump`), MIDI 1.0
at its own resolution and MIDI 2.0 alike. :cpp:class:`thl::midi::Midi1StreamParser`
turns a MIDI 1.0 byte stream into packets, :cpp:func:`thl::midi::convert_to_midi1`
gives code that only wants MIDI 1.0 plain short messages back, and
:cpp:class:`thl::midi::InputQueue` carries timestamped packets from the receive
threads to the audio thread without locks. :cpp:class:`thl::midi::DeviceManager`
lists and opens the MIDI inputs of the machine and follows hot-plugging (CoreMIDI
on macOS and iOS, ``android.media.midi`` on Android). :cpp:class:`thl::AudioMidiProcessor`
is the engine interface a host calls with a block's audio and its events, and
:cpp:class:`thl::AudioMidiCallback` is that host for an app: it runs an engine from the
audio device and the MIDI inputs, the events aligned with the audio to the sample. See
:doc:`midi`. Depends on Core.

Platforms
---------

macOS 12+, iOS 14+, Android, Linux, Windows and WebAssembly (Emscripten).
``cmake/tanh/platform.cmake`` resolves the target into ``TANH_OPERATING_SYSTEM``
and ``TANH_BINARY_FORMAT`` and defines exactly one ``THL_PLATFORM_*`` macro,
which ``tanh::Core`` carries as a public compile definition.

Licenses
--------

Core is Apache-2.0; every other component is AGPL-3.0, and ``TANH_BUILD_LINK``
adds Ableton Link's own terms. See :doc:`license`.

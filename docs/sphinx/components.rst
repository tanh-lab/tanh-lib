Components
==========

tanh-lib is six independently buildable library targets. Each is an
``add_library`` of its own, exported as ``tanh::<Component>`` both in-tree and
from the installed package, and enabled by a ``TANH_BUILD_<COMPONENT>`` option.
Everything lives in namespace ``thl``.

.. graphviz::
   :align: center

   digraph components {
       rankdir = BT;
       node [shape=box, style="rounded,filled", fillcolor="#eef3fb", fontname="Helvetica", fontsize=11];
       edge [color="#5b6b82", arrowsize=0.7];

       core       [label="tanh::Core\nDispatcher, Logger, RCU,\nLockFreeQueue, Buffer, WavReader"];
       state      [label="tanh::State\nhierarchical parameters,\nRCU reads, JSON"];
       dsp        [label="tanh::DSP\nprocessors, effects,\ngranular, Rings resonator"];
       modulation [label="tanh::Modulation\nmodulation matrix,\nchange-point sub-blocking"];
       audio_io   [label="tanh::AudioIO\ndevice I/O over miniaudio"];
       net        [label="tanh::Net\nHttpClient, Sha256,\nAssetStore"];

       state -> core;
       dsp -> core;
       audio_io -> core;
       net -> core;
       modulation -> core;
       modulation -> state;
       modulation -> dsp;
   }

Core
----

``tanh::Core`` (``include/tanh/core/``, ``src/core/``) is the foundation the
other components build on:

- :cpp:class:`thl::Dispatcher` — event messaging between subsystems
- ``thl::Logger`` — leveled logging with pluggable sinks and a
  real-time path, ``thl::Logger::rt``, that never allocates or blocks
- :cpp:class:`thl::RCU` — lock-free read-copy-update for data the audio thread
  reads while another thread replaces it
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
effects, filters, the granular engine, analysis helpers and the Rings resonator
model, which is also available as the separate ``tanh::Resonator`` target for
consumers that only need it. Every processor inherits
:cpp:class:`thl::dsp::BaseProcessor` and follows the ``prepare()`` /
``process()`` contract; ``process_modulated()`` splits a block at change points
for sample-accurate automation. Depends on Core.

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

Platforms
---------

macOS 12+, iOS 14+, Android, Linux, Windows and WebAssembly (Emscripten).
``cmake/tanh/platform.cmake`` resolves the target into ``TANH_OPERATING_SYSTEM``
and ``TANH_BINARY_FORMAT`` and defines exactly one ``THL_PLATFORM_*`` macro,
which ``tanh::Core`` carries as a public compile definition.

Licenses
--------

Core is Apache-2.0; every other component is AGPL-3.0. See :doc:`license`.

tanh-lib
========

Modular, real-time-safe C++20 audio library. Independently buildable
components, all in namespace ``thl``:

- **tanh::Core** — Dispatcher, Logger (with a real-time path), RCU, lock-free
  queue, generic buffers, header-only WAV decoder
- **tanh::Parameters** — parameter definitions, ranges, modulation scopes and
  lookup errors, shared by State and Modulation
- **tanh::State** — hierarchical parameter storage with RCU lock-free reads and
  JSON serialization
- **tanh::DSP** — processors, effects, filters, sample player / looper,
  slicing, pitch banks, granular engine, Rings resonator model (also available
  alone as ``tanh::Resonator``)
- **tanh::Modulation** — modulation matrix with change-point sub-blocking; reads
  any host's parameters; records and loops motion
- **tanh::AudioIO** — audio device I/O over miniaudio (desktop, iOS, Android)
- **tanh::Net** — verified HTTPS asset delivery (off by default)
- **tanh::Link** — Ableton Link session and Link Audio (``TANH_BUILD_LINK``,
  off by default)
- **tanh::Midi** — MIDI 1.0 and MIDI 2.0 as Universal MIDI Packets, device
  input (macOS, iOS, Android) and a lock-free queue to the audio thread

Platforms: macOS 12+, iOS 14+, Android, Linux, Windows, WebAssembly.

.. toctree::
   :maxdepth: 1
   :hidden:
   :caption: Contents:

   getting_started
   components
   realtime_safety
   symbol_visibility
   change_points
   modulation
   host_parameters
   transport
   motion_recording
   sampler
   slicing
   pitch
   audio_io
   midi
   api/index
   changelog
   contributing
   license

Where to start
--------------

New to tanh-lib? :doc:`getting_started` covers building, consuming the CMake
package and running the tests.

:doc:`components` describes the library targets and how they depend on each
other; :doc:`realtime_safety` lists the rules every ``process()`` path follows.
:doc:`change_points` shows, with an interactive figure, how a processor renders a
block in pieces that start where its modulated parameters change.
:doc:`symbol_visibility`, :doc:`modulation` and :doc:`audio_io` are the design
notes that used to live in the README. :doc:`host_parameters` drives the
matrix from a host's parameter store; :doc:`transport` covers musical time
(``TransportInfo``, host/internal/Link clocks) and Ableton Link;
:doc:`motion_recording` records and loops gestures (a slider, an XY pad). :doc:`sampler`, :doc:`slicing` and :doc:`pitch` describe the sample playback
components, :doc:`midi` the internal MIDI format.

The :doc:`api/index` is generated from the public headers under
``include/tanh/``.

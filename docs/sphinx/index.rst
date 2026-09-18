tanh-lib
========

Modular, real-time-safe C++20 audio library. Six independently buildable
components, all in namespace ``thl``:

- **tanh::Core** — Dispatcher, Logger (with a real-time path), RCU, lock-free
  queue, generic buffers, header-only WAV decoder
- **tanh::State** — hierarchical parameter storage with RCU lock-free reads and
  JSON serialization
- **tanh::DSP** — processors, effects, filters, sample player / looper,
  slicing, pitch banks, granular engine, Rings resonator model (also available
  alone as ``tanh::Resonator``)
- **tanh::Modulation** — modulation matrix with change-point sub-blocking
- **tanh::AudioIO** — audio device I/O over miniaudio (desktop, iOS, Android)
- **tanh::Net** — verified HTTPS asset delivery (off by default)

Platforms: macOS 12+, iOS 14+, Android, Linux, Windows, WebAssembly.

.. toctree::
   :maxdepth: 1
   :hidden:
   :caption: Contents:

   getting_started
   components
   realtime_safety
   symbol_visibility
   modulation
   sampler
   slicing
   pitch
   audio_io
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
:doc:`symbol_visibility`, :doc:`modulation` and :doc:`audio_io` are the design
notes that used to live in the README; :doc:`sampler`, :doc:`slicing` and
:doc:`pitch` describe the sample playback components.

The :doc:`api/index` is generated from the public headers under
``include/tanh/``.

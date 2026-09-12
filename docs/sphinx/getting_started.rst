Getting Started
===============

Prerequisites
-------------

- A C++20 compiler (Clang, GCC or MSVC)
- CMake 3.25 or newer
- Optional: `just <https://github.com/casey/just>`_ for the recipes in the
  ``justfile``; `Doxygen <https://www.doxygen.nl>`_, Graphviz and Python 3 to
  build this documentation

The third-party dependencies (nlohmann_json for State, miniaudio for AudioIO,
googletest and google-benchmark for the tests) are fetched by CMake. The Net
component links the platform HTTP stack (NSURLSession, WinHTTP, libcurl or
``HttpURLConnection`` over JNI) and is off by default.

Build
-----

.. code-block:: bash

   cmake --preset desktop-debug          # or: cmake . -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build --preset desktop-debug --parallel
   ctest --preset desktop-debug

With ``just``:

.. code-block:: bash

   just build            # cmake --preset desktop-debug + build
   just test             # build + ctest
   just build-release

The CMake presets in ``CMakePresets.json`` cover desktop, iOS (simulator and
device), Android, Windows, WebAssembly and the sanitizer configurations.

CMake options
~~~~~~~~~~~~~

.. include:: ../../README.md
   :parser: myst_parser.sphinx_
   :start-after: ### CMake options
   :end-before: ### Consume

Consume
-------

As a subdirectory or through FetchContent:

.. code-block:: cmake

   set(TANH_WITH_TESTS OFF)
   set(TANH_WITH_EXAMPLES OFF)
   add_subdirectory(modules/tanh-lib)

   target_link_libraries(app PRIVATE tanh::Core tanh::DSP)

``tanh::Core`` links no platform-specific system library by default (the
Android log library is the only exception), so it can be embedded by
permissively licensed projects without extra system dependencies. Emscripten
builds are detected as their own platform and use the plain stdout/stderr log
sink.

Install and ``find_package``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   cmake . -B build -DTANH_WITH_TESTS=OFF -DTANH_WITH_EXAMPLES=OFF
   cmake --build build
   cmake --install build --prefix /opt/tanh

.. code-block:: cmake

   find_package(tanh REQUIRED COMPONENTS Core)   # Core State DSP Resonator Modulation AudioIO Net
   target_link_libraries(app PRIVATE tanh::Core)

``test/install`` is a minimal consumer of the installed package; CI builds it
against a fresh install prefix in the merge queue (``just test-install``
locally). The exported target names match the in-tree ``ALIAS`` targets, so
consumers link the same ``tanh::<Component>`` whether they ``add_subdirectory``
or ``find_package``. A parent project that embeds tanh-lib via FetchContent and
installs its own package can leave ``TANH_WITH_INSTALL`` on: the components
join its install prefix and the parent's ``Config.cmake`` re-resolves them with
``find_dependency(tanh COMPONENTS Core)``.

Run the tests
-------------

.. code-block:: bash

   just test                                   # everything through ctest
   just test-filter PATTERN                    # ctest -R PATTERN
   ./build/desktop/Debug/test/dsp/test_dsp     # one suite directly
   ./build/desktop/Debug/test/dsp/test_dsp --gtest_filter="TestSuite.TestName"

``just test-audio`` runs the AudioIO suite; ``just test-hardware`` additionally
runs the tests that need a real audio device.

Rings reference fixtures
~~~~~~~~~~~~~~~~~~~~~~~~

The Rings resonator tests compare output against reference data generated from
the original Mutable Instruments code. Without fixtures, these tests are skipped
(the build still succeeds).

To generate fixtures (requires SSH access to the
``tanh-lab/mutable-instrument-api`` repository):

.. code-block:: bash

   ./test/dsp/generate_reference_fixtures.sh

This clones the upstream repository, builds the reference generators and writes
``.bin`` fixtures to ``test/dsp/fixtures/``. Then rebuild and run:

.. code-block:: bash

   cmake --build build --target test_dsp
   ./build/test/dsp/test_dsp

The fixture files are not checked into version control.

Build this documentation
------------------------

.. code-block:: bash

   just docs
   # or
   cmake -S . -B build/docs -DTANH_WITH_DOCS=ON -DTANH_WITH_TESTS=OFF -DTANH_WITH_EXAMPLES=OFF
   cmake --build build/docs --target sphinx-docs

The HTML lands in ``build/docs/docs/sphinx/html/``. Sphinx and its extensions
are installed into a virtual environment inside the build tree; Doxygen and
Graphviz have to be on the ``PATH``.

Contributing
============

Development setup
-----------------

.. code-block:: bash

   git clone git@github.com:tanh-lab/tanh-lib.git
   cd tanh-lib
   just build      # cmake --preset desktop-debug && build
   just test       # + ctest

``just`` (``just --list``) wraps the everyday recipes: ``format`` /
``format-check``, ``tidy`` / ``tidy-fix``, ``test-filter PATTERN``,
``test-install``, ``docs`` and the iOS configure/build recipes.

Naming conventions
------------------

Enforced by ``.clang-tidy``.

.. include:: ../conventions.md
   :parser: myst_parser.sphinx_
   :start-after: ## C/C++
   :end-before: ## TypeScript

Code style
----------

``.clang-format`` (Google-based: 100-character lines, 4-space indent, K&R
braces) and ``.clang-tidy`` are enforced in the merge queue.

Shared tooling
~~~~~~~~~~~~~~

``just format`` / ``just format-check`` run clang-format, ``just tidy`` /
``just tidy-fix`` run clang-tidy. Their configs — ``.clang-format``,
``.clang-tidy``, ``.clangd`` in the repository root — and the CMake modules
under ``cmake/tanh/`` (platform detection, the symbol-export policy and its
CTest check, git versioning, sanitizers, googletest/benchmark, Apple defaults,
the iOS toolchain, CPack, install RPATHs, binary data) are shared across the
tanh-lab projects and are **not maintained here**: they are installed verbatim
from a pinned release of `tanh-tooling <https://github.com/tanh-lab/tanh-tooling>`_
(canonical copies in its ``clang/`` and ``cmake/`` directories, documented in
its ``cmake/README.md``). Do not edit them by hand and do not add files to
``cmake/tanh/``; the ``tooling-config`` CI job re-downloads the pinned release
and fails if the committed files differ. Changes belong in tanh-tooling.

To move to a newer tanh-tooling release, run its installer with the new tag,
commit the rewritten files, and bump the ``ref`` (and workflow version) in
``.github/workflows/lint.yml`` to the same tag in the same commit. anira pins
the same modules and fetches tanh-lib, so both repositories must move to the
same tag together:

.. code-block:: bash

   curl -fsSL https://raw.githubusercontent.com/tanh-lab/tanh-tooling/vX.Y.Z/install.sh | sh -s -- clang cmake

Tests
-----

``test/`` mirrors the components: ``test/core``, ``test/state``, ``test/dsp``,
``test/modulation`` and ``test/audio-io`` each build one ``test_*`` binary, and
``test/exports`` and ``test/install`` check the packaging contracts. Run one
suite directly:

.. code-block:: bash

   ./build/desktop/Debug/test/dsp/test_dsp --gtest_filter="TestSuite.TestName"

The Rings resonator tests compare against reference fixtures that are not
checked in; without them those tests are skipped (see :doc:`getting_started`).

Every ``process()`` path is covered by the RealtimeSanitizer preset; see
:doc:`realtime_safety`.

CI
--

Pull requests run the fast desktop tier; the merge queue runs every leg:
desktop build and test on Linux, macOS and Windows, iOS and Android, the
WebAssembly build, the install-tree consumer, the sanitizer presets,
clang-format, clang-tidy and the tanh-tooling drift check. All workflows call
the shared actions in ``tanh-lab/ci-actions`` and must pin the same version;
``build_test``'s result job asserts that.

Documentation
-------------

- Document public API with Doxygen comments (``///`` or ``/** */``); the
  :doc:`api/index` is generated from ``include/tanh/`` on every push to
  ``main``.
- Design notes and guides live under ``docs/sphinx/`` (reST or Markdown, both
  render); the README stays a short entry point that links here. The changelog
  page and the CMake options table are included from ``CHANGELOG.md`` and
  ``README.md``, so keep those headings.
- Build locally with ``just docs`` and open
  ``build/docs/docs/sphinx/html/index.html``.

Changelog
---------

``CHANGELOG.md`` follows Keep a Changelog. Add an entry under ``Unreleased``
with every user-visible change, in the same style as the existing entries:
what changed, why, and what a consumer has to do about it.

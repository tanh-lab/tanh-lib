Real-Time Safety
================

The audio thread must never wait on anything with an unbounded worst case: no
heap allocation, no mutex, no system call that can block, no I/O. tanh-lib
encodes that as a handful of rules.

``process()`` is non-blocking
-----------------------------

Every ``process()`` method, and every helper it reaches, is marked
``TANH_NONBLOCKING_FUNCTION``. The macro expands to ``[[clang::nonblocking]]``
when the library is built with ``TANH_WITH_RTSAN`` and to nothing otherwise, so
the annotation documents the contract on every compiler and enforces it under
RTSan.

Register threads before touching State
--------------------------------------

Any thread that reads :cpp:class:`thl::State` or
:cpp:class:`thl::StateGroup` from a real-time context calls
``ensure_thread_registered()`` first. Registration allocates the per-thread RCU
slot once, up front, so the reads themselves stay lock- and allocation-free.

Numeric parameters are safe, strings may not be
-----------------------------------------------

``double``, ``float``, ``int`` and ``bool`` parameters are fully real-time
safe to read. String parameters are backed by ``std::string`` and may allocate
once a value grows beyond the small-string buffer; keep them off the audio
thread.

Logging from the audio thread
-----------------------------

``thl::Logger::log`` / ``logf`` and friends format and dispatch synchronously
and are **not** real-time safe. From real-time code use
``thl::Logger::rt::logf()`` or ``thl::Logger::rt::log()``: the message is
formatted with ``thl::core::rt_snprintf`` (the allocation-free formatter), pushed
onto a lock-free queue and drained by a background thread into the regular
sinks.

Containers
----------

``Buffer``, ``MemoryBlock`` and ``RingBuffer`` allocate only in their
constructors and ``resize``/``prepare`` paths; the accessors used per sample are
allocation-free. An allocation that fails throws ``std::bad_alloc``; a contract
violation such as an out-of-range index is an ``assert``.

Checking it: RealtimeSanitizer
------------------------------

Configure with ``-DTANH_WITH_RTSAN=ON`` (Clang 20 or newer) or use the
``desktop-debug-rtsan`` preset. Every ``TANH_NONBLOCKING_FUNCTION`` then aborts
the test on the first allocation, lock, sleep or blocking syscall it reaches,
with a stack trace. The ``sanitizers`` workflow runs this leg in the merge
queue alongside ASan/UBSan, TSan and LSan.

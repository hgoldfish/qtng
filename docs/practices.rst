Best Practices
==============

Use CoroutineGroup to manage Coroutines
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Always spawn coroutines through ``CoroutineGroup`` and delete the group before other resources are torn down. This ensures coroutines exit cleanly and captured objects remain valid for the lifetime of the work.


The difference between send() and sendall(), recv() and recvall()
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``send()`` and ``recv()`` may transfer fewer bytes than requested in a single call. Use ``sendall()`` and ``recvall()`` when you need the full amount to be sent or received, or when the protocol defines an exact payload size.


Pass std::shared_ptr<T> to coroutine entry
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When a lambda captures objects that must outlive the coroutine, pass them by value or wrap mutable state in ``std::shared_ptr<T>``. Never capture raw pointers to stack or short-lived objects unless you can guarantee they remain valid until the coroutine finishes.


Redirect ngDebug / ngWarning with setLogHandler
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

By default ``ngDebug()`` / ``ngWarning()`` / ``ngCritical()`` / ``ngFatal()`` write to stderr.
Call ``qtng::utils::setLogHandler(handler)`` once at process startup to route every
``LogStream`` flush into your own sink (for example a TUI log pane). Pass ``nullptr`` to
restore the default. Swapping the handler while other threads are flushing is not thread-safe.

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

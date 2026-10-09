Bandwidth
=========

A tiny TCP bandwidth test built on [qtng](https://github.com/hgoldfish/qtng).

`bandwidth-server` streams meaningless random data as fast as the socket allows.
`bandwidth-client` reads it back at a target rate (8 MB/s by default) and prints
the current throughput once per second. The client does the throttling: once its
receive buffer fills up the TCP window closes and the server's `sendall()` blocks,
so the server never needs to know the target rate.


Usage
-----

Start the server:

    $ ./bandwidth-server
    listening on 0.0.0.0:8000, streaming random data

Then run the client, pointing it at the server:

    $ ./bandwidth-client -r 127.0.0.1:8000
    8.00 MB/s  total 8.00 MB
    8.00 MB/s  total 16.00 MB
    ...

Both keep running until interrupted with `Ctrl-C`. The server prints per-connection
statistics and supports multiple concurrent clients.

Options:

    bandwidth-server [-l :8000]        # -l/--listen
    bandwidth-client -r HOST:PORT [-b 8M]

`-b/--rate` accepts a plain byte count or a `K`/`M`/`G` suffix (1024-based), e.g.
`-b 8M`. Use `-h` for help and `-v` for the version.


Requirement
-----------

1. C++11 compiler
2. CMake 3.14+
3. zlib
4. OpenSSL or bundled LibreSSL (same as qtng)


Build Commands
--------------

From the `examples/bandwidth` directory:

    $ mkdir build
    $ cd build
    $ cmake ..
    $ make -j16

The build pulls in qtng from the repository root (`../../`).

### Portable musl-static binaries (Docker / Alpine)

Produce fully static, musl-linked binaries with no glibc dependency (runnable on
any same-arch Linux). Requires Docker.

From the `examples/bandwidth` directory:

    $ ./linux.py
    # optional: ./linux.py --no-cache

Artifacts are copied to `dist/linux/bandwidth-server` and
`dist/linux/bandwidth-client`.

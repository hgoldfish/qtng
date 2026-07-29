kcptun
======

Minimal TCP port-forwarding tunnel over KCP, built on
[qtng](https://github.com/hgoldfish/qtng).

Command-line style is close to the community
[kcptun](https://github.com/xtaci/kcptun), but this example uses qtng
``KcpSocket`` + ``MultiStream`` and is **not** wire-compatible with the
Go kcptun binaries.

Topology
--------

TCP forward mode::

    Application --TCP--> kcptun-client (-l)
                      --KCP/UDP + MultiStream-->
                      kcptun-server (-l) --TCP--> Target (-t)

Built-in httpd mode (server ``-r`` instead of ``-t``)::

    Browser/curl --TCP--> kcptun-client (-l)
                       --KCP/UDP + MultiStream-->
                       kcptun-server (-l) --static files--> webroot (-r)

Connecting to the client's local TCP port is like talking to a normal
HTTP server; only the middle hop uses KCP.

Quick start
-----------

### TCP forward

Server (forwards KCP traffic to a local TCP service; default listen is ``:8000``)::

    $ ./kcptun-server -t "127.0.0.1:8388"

Client (listens locally and dials the KCP server)::

    $ ./kcptun-client -l ":8388" -r "SERVER_IP:8000"

Then connect applications to ``127.0.0.1:8388`` on the client host.

### Built-in httpd

Server (serves static files from a directory over KCP)::

    $ ./kcptun-server -r "./htdocs"

Client (unchanged)::

    $ ./kcptun-client -l ":8388" -r "SERVER_IP:8000"

Then open ``http://127.0.0.1:8388/`` on the client host (or use curl).
Put an ``index.html`` under the webroot for directory requests.

Options
-------

Client::

    kcptun-client -l ":12948" -r "HOST:8000" [-mode fast|normal]

Server (pick one of ``-t`` or ``-r``)::

    kcptun-server [-l ":8000"] -t "127.0.0.1:22" [-mode fast|normal]
    kcptun-server [-l ":8000"] -r "/path/to/webroot" [-mode fast|normal]

- Server ``-l`` defaults to ``0.0.0.0:8000`` (``Any:8000``) when omitted.
- ``-mode fast`` maps to ``KcpSocket::FastInternet`` (default)
- ``-mode normal`` maps to ``KcpSocket::Internet``
- Server ``-t`` and ``-r`` are mutually exclusive. Without ``-r``, the
  default remains TCP forward to ``127.0.0.1:22``.
- Without ``-t``, the TCP target defaults to ``127.0.0.1:22``.
- Server ``-r`` / ``--webroot`` uses qtng ``SimpleHttpRequestHandler``
  (GET/HEAD static files). Client ``-r`` still means remote KCP address.

This minimal demo does **not** include encryption, FEC, compression,
JSON config, or SMUX version flags from upstream kcptun.

Build
-----

From ``examples/kcptun``::

    $ mkdir build && cd build
    $ cmake ..
    $ make -j$(nproc)

Requirements match qtng: C++11, CMake 3.14+, zlib, OpenSSL or bundled
LibreSSL.

### Portable musl-static binaries (Docker / Alpine)

Produce fully static, musl-linked binaries with no glibc dependency
(runnable on any same-arch Linux). Requires Docker.

From ``examples/kcptun``::

    $ ./build.py
    # optional: ./build.py --no-cache

Artifacts are copied to ``dist/kcptun-server`` and ``dist/kcptun-client``.

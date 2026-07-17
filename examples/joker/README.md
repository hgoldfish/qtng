Joker
=====

A socks5 proxy built on [qtng](https://github.com/hgoldfish/qtng).


Usage
-----

Copy the `joker-server` to server.

    $ scp joker-server username@example.com:.
    $ ssh username@example.com

To start:

    $ ./joker-server

If you want to keep the `joker-server` running after logging out, `screen` may be a good choice.

    $ screen -R joker   # connect or reconnect to screen session.
    $ ./joker-server    # in screen window, then press `Ctrl + A, D` to detach

Then start the client:

    $ ./joker-client example.com

Now a socks5 sever listening on `localhost:8085` is running.

Change the settings of Firefox or Chrome to use this proxy.

For more control, you can copy `client-config-example.txt` and `server-config-example.txt` to `client-config.txt` and `server-config.txt` and edit them. Then start both client and server with profile name.

    $ ./joker-client client-config.txt
    $ ./joker-server server-config.txt


Requirement
-----------

1. C++11 compiler
2. CMake 3.14+
3. zlib
4. OpenSSL or bundled LibreSSL (same as qtng)


Build Commands
--------------

From the `examples/joker` directory:

    $ mkdir build
    $ cd build
    $ cmake ..
    $ make -j16

The build pulls in qtng from the repository root (`../../`).

### Portable musl-static binaries (Docker / Alpine)

Produce fully static, musl-linked binaries with no glibc dependency (runnable on
any same-arch Linux). Requires Docker.

From the `examples/joker` directory:

    $ ./build.py
    # optional: ./build.py --no-cache

Artifacts are copied to `dist/joker-server` and `dist/joker-client`.

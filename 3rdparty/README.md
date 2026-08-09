# Third-party git submodules

| Path | Upstream | Purpose |
|------|----------|---------|
| `libressl/` | [libressl/portable](https://github.com/libressl/portable) | Bundled TLS (optional; falls back to system OpenSSL, then `QTNG_NO_CRYPTO`) |
| `libutp/` | [transmission/libutp](https://github.com/transmission/libutp) | uTP interoperability tests only |

Initialize after clone:

```bash
git submodule update --init 3rdparty/libressl   # optional TLS bundle
git submodule update --init 3rdparty/libutp     # optional uTP libutp tests
```

Build works without initializing submodules: LibreSSL → system OpenSSL → `QTNG_NO_CRYPTO`; libutp tests are skipped.

## QUIC interop (picoquic, optional)

QUIC runtime code does **not** link picoquic. The Catch2 target `qtng_test_quic_picoquic`
spawns an external [`picoquicdemo`](https://github.com/private-octopus/picoquic) process
(same idea as libutp tests: third-party only for connectivity checks).

Example local setup (OpenSSL 3.1 works; ngtcp2’s OpenSSL backend needs 3.5+):

```bash
git clone --depth 1 https://github.com/private-octopus/picoquic.git /tmp/picoquic-src
cmake -S /tmp/picoquic-src -B /tmp/picoquic-build -DPICOQUIC_FETCH_PTLS=Y -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/picoquic-build --target picoquicdemo -j"$(nproc)"

mkdir -p /tmp/picoquic-certs
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout /tmp/picoquic-certs/key.pem -out /tmp/picoquic-certs/cert.pem \
  -subj '/CN=localhost'

# From the qtng build directory:
ctest -R picoquic --output-on-failure
# or: ./tests/qtng_test_quic_picoquic
```

Override paths with `QTNG_PICOQUICDEMO` and `QTNG_PICOQUIC_CERTDIR` if needed.
When the binary/certs are missing, the interop test is skipped.

# Alpine + musl toolchain for fully static Linux binaries (no glibc).
FROM alpine:3.20

# Optional: use a China mirror for faster apk
RUN sed -i 's#https\?://dl-cdn.alpinelinux.org#https://mirrors.ustc.edu.cn#g' /etc/apk/repositories

RUN apk add --no-cache \
    build-base \
    cmake \
    linux-headers \
    zlib-dev \
    zlib-static \
    file \
    && printf '%s\n' \
        '#!/bin/sh' \
        'set -eu' \
        'BUILD_DIR=.build-in-docker' \
        'JOBS="$(nproc)"' \
        'cd /qtng/examples/joker' \
        'mkdir -p "$BUILD_DIR"' \
        'ZLIB_A=""' \
        'for candidate in /lib/libz.a /usr/lib/libz.a; do' \
        '  if [ -f "$candidate" ]; then ZLIB_A="$candidate"; break; fi' \
        'done' \
        'if [ -z "$ZLIB_A" ]; then echo "libz.a not found; install zlib-static" >&2; exit 1; fi' \
        'cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_FIND_LIBRARY_SUFFIXES=".a" -DCMAKE_EXE_LINKER_FLAGS="-static" -DZLIB_USE_STATIC_LIBS=ON -DZLIB_LIBRARY="$ZLIB_A" -DZLIB_INCLUDE_DIR=/usr/include' \
        'cmake --build "$BUILD_DIR" -j "$JOBS" --target joker-server joker-client' \
        'for bin in joker-server joker-client; do' \
        '  path="$BUILD_DIR/$bin"' \
        '  if [ ! -f "$path" ]; then echo "missing binary: $path" >&2; exit 1; fi' \
        '  file "$path" || true' \
        '  if ldd "$path" >/dev/null 2>&1; then echo "ERROR: $path is not fully static:" >&2; ldd "$path" >&2 || true; exit 1; fi' \
        '  echo "static ok: $path"' \
        'done' \
        'echo "artifacts: /qtng/examples/joker/$BUILD_DIR/joker-server /qtng/examples/joker/$BUILD_DIR/joker-client"' \
        > /usr/local/bin/joker-build \
    && chmod +x /usr/local/bin/joker-build

WORKDIR /qtng/examples/joker
ENTRYPOINT ["/bin/sh", "/usr/local/bin/joker-build"]

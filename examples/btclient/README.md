# qtng BitTorrent Client

Desktop GUI for the qtng BitTorrent core (`qtng::TorrentSession`).

## Build

```bash
mkdir build && cd build
# If CMake picks a broken Qt, point at the system install, e.g.:
#   cmake .. -DCMAKE_PREFIX_PATH=/usr/lib64/cmake/Qt5
cmake ..
cmake --build .
./btclient
```

Requires Qt5 or Qt6 Widgets. The example builds qtng from the repository root via `add_subdirectory`.

## Usage

1. Choose a `.torrent` file.
2. Choose a download directory.
3. Click **Start**. DHT and µTP are enabled by default.

The download runs on a worker thread with qtng coroutines; the UI polls `TorrentHandle::stats()`.

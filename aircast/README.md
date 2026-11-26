# AirCast Studio Engine

Headless OBS Studio (32.1.0) engine with our in-tree WebRTC phone-camera receiver plugin (`plugins/aircast`). The Electron desktop app (`aircast_desktop`) launches this engine in the background with `--aircast-engine` and drives it over `obs-websocket`.

| What | Where |
|---|---|
| Headless engine mode (`--aircast-engine`: no window, tray, first-run wizard, or safe-mode prompt; forces WebSocket server on) | `aircast/patches/engine-mode.patch` (applied to this tree) |
| Phone receiver plugin (C++20, GPL-2.0-or-later) | `plugins/aircast` (registered in `plugins/CMakeLists.txt`; also standalone-testable via `ctest`) |
| Engine launcher | `aircast/run.sh` |

## Build (Ubuntu 26.04)
```bash
sudo apt install extra-cmake-modules libasound2-dev libasio-dev libcurl4-openssl-dev libavformat-dev libavdevice-dev \
  libavfilter-dev libffmpeg-nvenc-dev libjack-jackd2-dev libjansson-dev libdrm-dev libfdk-aac-dev libpci-dev \
  libspeexdsp-dev libudev-dev libv4l-dev libva-dev libx264-dev libluajit-5.1-dev libmbedtls-dev libpipewire-0.3-dev \
  libpulse-dev libqrcodegencpp-dev qt6-base-dev qt6-base-private-dev qt6-svg-dev libsndio-dev swig uthash-dev \
  libvpl-dev libvulkan-dev libwebsocketpp-dev libx11-xcb-dev libxcb-randr0-dev libxcb-xinerama0-dev \
  libxcb-composite0-dev libxcb-xfixes0-dev libvlc-dev librist-dev libsrt-openssl-dev libxss-dev libcmocka-dev \
  libobs-dev libavcodec-dev libopus-dev libssl-dev nlohmann-json3-dev   # last line: the aircast plugin
```
```bash
cmake --preset ubuntu -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=$PWD/.install -DENABLE_BROWSER=OFF
nice cmake --build build_ubuntu -j4        # -j8 makes the desktop unusable for ~10 min
cmake --install build_ubuntu
./aircast/run.sh --aircast-engine
```
- `run.sh` points the engine at its own `libobs` (dev builds have no rpath). When started from VS Code's snap terminal, it also drops the snap's GTK/GSettings paths, which otherwise crash Qt at startup.
- The browser source and docks are off (`ENABLE_BROWSER=OFF`) until CEF is set up.
- Log lines from the receiver plugin start with `[aircast]`.

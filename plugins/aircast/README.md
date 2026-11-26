# AirCast OBS plugin (B8 receiver)

Receives phone cameras over WebRTC (`plan/04-studio-desktop.md` §2). It ships **inside AirCast Studio**, our own app built from OBS's source; users never run OBS (`docs/beat-prism-desktop.md` §4). Until AirCast Studio builds, stock OBS is only a dev test host.

## Status
| Module | State |
|---|---|
| `rtp_video`: RTP parse, H.264 FU-A/STAP-A depacketiser, jitter buffer, NACK, PLI | ✅ unit-tested |
| `sdp_filter`: answer only H.264 mode 1 + Opus; drop RTX/RED/FEC, transport-cc, CVO | ✅ unit-tested |
| `decoder`: FFmpeg H.264 + Opus | ✅ loopback-tested (x264 → RTP → shuffle → decode). **Software only** (`shortcut:` in decoder.cpp) |
| `peer`: libdatachannel PeerConnection, REMB per role, NACK out, PLI | ✅ end-to-end test over localhost (drop → NACK → resend → all frames) |
| `camera_source`: "AirCast Camera" source (phone + feed: main / game-mode face), program/preview role from OBS activate | ✅ loads in libobs; not yet tried with a real phone |
| Signalling | obs-websocket vendor `aircast` (see `src/plugin-main.cpp`); the Electron app bridges backend ↔ OBS |
| Not done | GPU decode, audio jitter buffer + A/V sync, slates, stats, ICE trickle/restart, Windows/macOS builds |

## Build, test, install (Linux)
```bash
sudo apt install obs-studio libobs-dev libavcodec-dev libavutil-dev libswscale-dev libopus-dev libssl-dev nlohmann-json3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build && ctest --test-dir build --output-on-failure
cmake --install build      # → ~/.config/obs-studio/plugins/aircast/bin/64bit/aircast.so
```
libdatachannel v0.24.7 comes in through `FetchContent`, linked statically with its symbols hidden.

## Try it with a phone (dev, in stock OBS until AirCast Studio exists)
1. Start OBS. In **Tools → WebSocket Server Settings**, enable the server and copy the password.
2. Start the desktop app (`npm start` in the repo root) and sign in. Click **OBS: off**, enter the password, then **Connect**.
3. On the phone, tap Go live. The app shows the phone as **In OBS**.
4. In OBS, add a source: **+ → AirCast Camera**, then pick the phone. For game mode, add a second one with Feed = *Face camera*.
5. Put the scene on Program: the phone switches to 1080p/6 Mbps; off program it drops to 640p/600 kbps.

Logs: OBS log lines start with `[aircast]`.

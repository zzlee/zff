# webrtc_page — Browser publishing demo

Publish camera/mic from Chrome to the `demo_webrtc_page` C program over
WebRTC, signaled through its built-in WebSocket server.

## Build

```bash
# from ~/zff
./scripts/build-docker.sh x86
./scripts/build.sh x86        # demo lands in build-x86/examples/webrtc_page/
```

## Run

```bash
# terminal 1: signaling (8610) + WebRTC receiver
./build-x86/examples/webrtc_page/demo_webrtc_page --port 8610 --room demo

# terminal 2: serve this directory (any static server)
cd examples/webrtc_page && python3 -m http.server 8000
```

Open http://localhost:8000 in Chrome:
1. Signaling URL `ws://localhost:8610`, room `demo` → **Connect signaling**
2. **Start camera + publish** (allow camera/mic) → offer/answer/ICE flow in the log
3. Watch the C console: `frames=... fps=...` proves media arrival
4. **Chat** via the `chat` data channel (C echoes back)
5. **Request keyframe** sends `KEYFRAME 0` → C calls
   `zstr_webrtc_request_keyframe()` (sender-side PLI path)

## Notes

- `localhost` is a secure context, so camera works over plain HTTP.
- For LAN testing, replace `localhost` with the host IP on both ends and
  pass `--stun` later (host candidates usually suffice on one subnet;
  STUN/TURN options are a follow-up).
- Remote `<video>` stays black in v1: the C side has no encoder yet, so it
  only receives. TWCC feedback still runs (watch `bytes=` grow steadily).
- Protocol spoken here is `zstr_signaling` (`JOIN/OFFER/ANSWER/CANDIDATE/MSG`);
  see `include/zff/plugins/zstr_signaling.h`.

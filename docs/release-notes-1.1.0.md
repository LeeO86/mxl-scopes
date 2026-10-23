# mxl-scopes 1.1.0

This release adds a browser preview of the scope, a new web UI, an operator-screen widget, sharper texts, NMOS conformance tests in CI, and deployment files (milestones M3 and M4 of IMPLEMENTATION_PLAN.md). Adrian decided on 2026-10-23 to ship M3 and M4 together. 1.0.0 (milestones M0–M2) is the base; its notes, `docs/release-notes-1.0.0.md`, stay valid except where this file says otherwise. The changes are in CHANGELOG.md under "1.1.0".

## Image

```
ghcr.io/leeo86/mxl-scopes:1.1.0   (also 1.1, 1)
ghcr.io/leeo86/mxl-scopes@sha256:<OCI index digest, filled in after the tag build>
```

- Git tag `v1.1.0` = commit `<sha>`.
- MXL `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, also in the label `io.dmf.mxl.revision`.
- nmos-cpp `fe303849527394b03bdedc8f161f377fe458bb62`.
- The image now also contains:
  - MediaMTX 1.20.1 at `/usr/local/bin/mediamtx` (MIT);
  - Ubuntu's FFmpeg libraries with libx264 (GPL-2.0-or-later as built);
  - Blend2D 0.21.2 and DejaVu Sans, linked in.
  - NOTICE lists every source, including the code taken from the sibling functions.
- Runs as uid 1000. No GPU is needed. When a GPU with NVENC is passed in, the preview encoder uses it.

## What is new

- **WebRTC preview** (the platform preview contract):
  - The scope picture is sent as H.264 over RTSP to a MediaMTX, which serves WHEP and HLS.
  - With `PREVIEW_PUBLISH_URL` set, it goes to that shared MediaMTX at `<PREVIEW_PATH_PREFIX>/scope`. Without it, the image's own MediaMTX runs as a child process; only the scope itself (127.0.0.1) may publish to it, anyone may watch.
  - Default 960×540 at 25 fps, 2 Mbit/s. It is encoded with libx264, or NVENC when available.
  - It costs about 0.15 core. With `SCOPES_PREVIEW_MODE=off` it costs nothing.
- **Web UI** (Vue 3) in the family look:
  - **Scope**: the preview, the live controls and the measurements;
  - **Input**;
  - **NMOS**: ids, the output flow, IS-05 activation and disable;
  - **Status**: probes, engine, preview;
  - **Settings**: every setting with its origin and when it applies, export and import.
- **Operator-screen widget** `scope`:
  - `GET /widgets`; `GET /widget/scope?controls=&measurements=&theme=dark|light|transparent`;
  - `frame-ancestors` comes from `WIDGET_FRAME_ANCESTORS` (host wildcards like `https://*.example.com` also allow CORS on `/widgets`);
  - `WEB_ENABLE=false` turns the widget routes off (404) with the UI;
  - the page posts `widget-ready` and `widget-size` to its parent.
- **Texts with Blend2D and DejaVu Sans.** The graticule labels, caption and state texts are anti-aliased. The picture changes in these texts only; the trace is the same.
- **Snapshot PNG with a key**: `GET /api/v1/snapshot.png` is RGBA with the key as alpha when the output has a key.
- **NMOS fix**: the receiver's IS-04 `caps` now include `media_types`, which was missing in 1.0.0. AMWA IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 run in CI.
- **Deployment and monitoring**:
  - `deploy/k8s/deployment.yaml`;
  - `deploy/grafana/mxl-scopes.json` with provisioning;
  - `docker/docker-compose.demo.yaml`;
  - `docs/cuda-design.md`.

## For the platform

The upgrade from 1.0.0 is in place. The flow ids, NMOS ids, ports, settings and state directory stay the same.

### What changes

**Settings that now take effect.** In 1.0.0 these were validated but did nothing:
- `SCOPES_PREVIEW_MODE`: default `webrtc`, or `off`.
- `SCOPES_PREVIEW_FPS`: default 25, at most the update rate.
- `SCOPES_PREVIEW_HEIGHT`: default 540.
- `PREVIEW_PUBLISH_URL`, `PREVIEW_PATH_PREFIX`, `PREVIEW_WHEP_URL`, `PREVIEW_HLS_URL`: the shared preview of Appendix B.1. Set them as for the other functions; the stream is `<PREVIEW_PATH_PREFIX>/scope`.
- `MEDIAMTX_RTSP_PORT`, `MEDIAMTX_WHEP_PORT`, `MEDIAMTX_HLS_PORT`, `MEDIAMTX_ICE_UDP_PORT` (8574, 8579, 8578, 8279): used only in own mode, when `PREVIEW_PUBLISH_URL` is empty.
- `WIDGET_FRAME_ANCESTORS`: the designer's origin, e.g. `'self' https://designer.<fqdn>`.

**Ports.**
- Unchanged: 8170 (web) and 3322 (+3323) for NMOS.
- In own mode only: 8579 (WHEP), 8578 (HLS) and 8279 UDP and TCP (ICE) on the pod. The RTSP ingest listens on 127.0.0.1:8574 only.
- With the shared MediaMTX, no new ports.

**NMOS.** Same ids and labels. The receiver's `caps.media_types` now lists `video/v210` and `video/v210a`.

**Catalog and designer.**
- Widget `scope`: params `controls` and `measurements` (booleans), `min_size` 320×180.
- `previews`: one stream at `<prefix>/scope`.
- `gpu`: none. An NVENC slot is optional; without one the preview uses x264.

**Metrics.** New:
- Preview: `mxl_scopes_preview_publish_mode{mode}`, `mxl_scopes_preview_publish_state{state}`, `mxl_scopes_preview_encoder{encoder}`, `mxl_scopes_preview_frames_total`, `mxl_scopes_preview_encode_seconds` (histogram). They are absent with `SCOPES_PREVIEW_MODE=off`.
- Process metrics, without the prefix, as the siblings: `process_cpu_seconds_total`, `process_resident_memory_bytes`, `process_start_time_seconds`, `process_open_fds`.
- None renamed or removed. The 7 measurement gauges of 1.0.0 are unchanged.

### Resources

CPU per scope, at 25 Hz:
- Platform (i9, 1.0.0, no preview): `luma` 0.10 core, YCbCr parade 0.16, RGB parade 0.19, RGB overlay 0.26, YCbCr overlay 0.18; `luma` with a key 0.17.
- The x264 preview adds about 0.15 core in the lab (Skylake-SP; expect less on the i9), 0.08 with NVENC.

Memory:
- Platform: 63–116 MB RSS per scope.
- Lab, 1.1.0: 80–160 MB of the process's own memory. On top come the MXL grains it maps (its output ring, 55–140 MB) and about 40 MB for an own MediaMTX (a child process: not in the scope's RSS or `process_resident_memory_bytes`).

`deploy/k8s/deployment.yaml` suggests:
- requests 500m CPU and 384Mi memory;
- limits 2 CPUs and 768Mi (every frame, a key, `SCOPES_THREADS=2`) at 1080p; a UHD output with a key needs about 1Gi or more.

Details in `docs/performance.md` "M3" and "M4".

### Lab numbers (1.1.0)

- **Preview cost:**
  - x264: +0.15 core (luma at 25 Hz: 0.26 → 0.41);
  - NVENC: +0.08 core;
  - its MediaMTX: 0.005 core;
  - quality at 540p: the traces stay readable.
- **Soak:** two instances on one node for 1 h, luma, and RGB parade with a key, both with the preview:
  - RSS flat, no leak;
  - 0 torn reads;
  - late output grains only at start-up and in two short bursts while other work ran on the host.
- **AMWA:** every result Pass, or listed with its reason in `tests/nmos/amwa-expected-off.json`. CI runs without DNS-SD, so IS-04 registration and heartbeats are not covered by AMWA (only against the stub registry in `tests/integration/scope.sh`).

### Platform checks done on 1.0.0 (thank you)

- The keyed scope over a picture works on test-all with multiviewer 1.4.6 (CUDA): compose 1.56 ms, 0 late and 0 missed.
- The CPU and RSS figures per mode above.

## Known limits

- **Multiviewer 1.4.4 CPU backend:** it does not key a v210a tile over the tiles below it. Use the CUDA backend, as on test-all with 1.4.6.
- **RGB parade every frame** stays above the §13 "< 0.4 core" (accepted as deviation 25). `SCOPES_THREADS=2` brings an update under 8 ms, but does not lower the CPU.
- **Two-scope soak:** the scope with a key and the preview used 0.65 core on one core. Give such a scope more than one core: the 2-CPU limit of the example does.
- **Browser support:** headless browsers without H.264 fall back to the snapshot. Chrome, Edge, Firefox and Safari play the stream.

## Please report back

- The digest you pin.
- The preview on the shared MediaMTX:
  - state `publishing` in `/api/v1/status`;
  - the picture in the designer's `scope` widget (the M3 exit check, on an operator screen).
- CPU per scope with the preview on small.
- Late or missed output grains.

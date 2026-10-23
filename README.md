# mxl-scopes

Video scopes for MXL: a media function that reads one MXL video flow and writes a waveform monitor (luma first, then YCbCr and RGB parades) as an ordinary MXL video flow. The multiviewer or any other MXL function can show it, keyed over the picture if wanted. Operators also see it in a browser and on operator screens.

**Status: 1.0.0 (M0–M2).** Every stage-1 mode (luma waveform, YCbCr and RGB parades, RGB and YCbCr overlays) runs end to end: MXL in, MXL out (with a key if wanted), NMOS, REST, metrics with the measurements, a web page with a snapshot; strip workers share the work over several cores. The WebRTC preview and the widget follow in M3. Release notes: [docs/release-notes-1.0.0.md](docs/release-notes-1.0.0.md). CPU per mode: [docs/performance.md](docs/performance.md). What differs from the specification today is listed under "Deviations" in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). Read [SPECIFICATION.md](SPECIFICATION.md) for the design.

## At a glance

- C++20, one process per scope, one video input per instance.
- Input `video/v210` (or `v210a`), BT.709, up to 3840×2160.
- Output `video/v210`, or `video/v210a` with a key for overlays, default 1920×1080 at the input rate.
- Modes: `luma`, `ycbcr-parade`, `rgb-parade`, `rgb-overlay`, `ycbcr-overlay` (live, the output flow id stays). Later: `vectorscope`, `luma-chroma`, HDR.
- Configurable update rate (`frame`, or 1–60 Hz) and reduced sampling, to save CPU.
- CPU engine with AVX-512, AVX2 and a scalar reference, bit-exact; a CUDA backend can be added later.
- NMOS IS-04/IS-05 with BCP-007-03; web UI and REST; `/livez`, `/readyz`, `/metrics`; the `scope` widget; WebRTC preview through the platform's preview contract.

## Ports

| Port | Setting | Default |
| --- | --- | --- |
| Web UI, REST, probes, metrics, widgets | `WEB_PORT` | 8170 |
| NMOS Node and Connection APIs | `NMOS_PORT` | 3322 (+1 reserved) |
| Built-in MediaMTX RTSP / WHEP / HLS / ICE (only without `PREVIEW_PUBLISH_URL`) | `MEDIAMTX_*_PORT` | 8574 / 8579 / 8578 / 8279 |

## Settings (M2)

Environment > `SCOPES_CONFIG_FILE` (flat JSON, else `<SCOPES_STATE_DIR>/config.json`) > defaults. All keys and their meaning: SPECIFICATION §8. In M2:

| Key | Default | M2 |
| --- | --- | --- |
| `SCOPES_MODE` | `luma` | `luma`, `ycbcr-parade`, `rgb-parade`, `rgb-overlay`, `ycbcr-overlay`, live (stage-2 modes exit 78) |
| `SCOPES_UPDATE_RATE` | `25` | `frame` or 1–60 Hz, live; the overlays run at most every n-th input frame, at most 30 Hz (25 Hz from 50p) |
| `SCOPES_ACCUMULATE` | `latest` | `latest` or `all` (every frame between two updates), live |
| `SCOPES_LINE_STEP`, `SCOPES_PIXEL_STEP` | 1, 1 | live; 32-bit histogram cells when 16 bits would not hold an update (UHD on a small output, a parade, `all`) |
| `SCOPES_RANGE`, `SCOPES_GRATICULE_UNITS`, `SCOPES_GRATICULE_LEVEL` | `limited`, `mv`, 60 | live |
| `SCOPES_INTENSITY`, `SCOPES_TRACE_MIN`, `SCOPES_TRACE_COLOR`, `SCOPES_CAPTION` | 100, 96, `green`, `true` | live; the trace colour applies to `luma` (the parades and the overlay colour each component) |
| `SCOPES_OUTPUT_FORMAT`, `SCOPES_OUTPUT_RATE` | `1920x1080`, `input` | live with a new output flow id (`PUT /api/v1/config`; `input` follows the routed flow) |
| `SCOPES_OUTPUT_KEY`, `SCOPES_BG_ALPHA` | `false`, 0 | `true`: `video/v210a`, the scope over a picture in a multiviewer (key: background `SCOPES_BG_ALPHA`, trace at its intensity, graticule at its level, texts opaque); the key live with a new output flow id, `SCOPES_BG_ALPHA` live |
| `SCOPES_HOLD_MS`, `SCOPES_INPUT_OFFSET_GRAINS` | 1000, 0 | live |
| `SCOPES_THREADS` | 1 | 1–8 at start: the renderer and `SCOPES_THREADS` − 1 strip workers share the analyse and trace steps (same picture for any count; `renderer.threads` in the status) |
| `SCOPES_SIMD` | `auto` | `auto`, `avx512`, `avx2`, `scalar` (`auto` = AVX2; AVX-512 only when set to `avx512`, it measured slower so far) |
| `SCOPES_PREVIEW_MODE`, `SCOPES_PREVIEW_FPS`, `SCOPES_PREVIEW_HEIGHT` | `webrtc`, 25, 540 | the WebRTC preview (H.264, x264; NVENC when a GPU has it) of the platform preview contract: `PREVIEW_PUBLISH_URL` set publishes `<PREVIEW_PATH_PREFIX>/scope` to a shared MediaMTX, empty runs the image's MediaMTX on the `MEDIAMTX_*` ports; `off` publishes nothing |
| `SCOPES_STATE_DIR` | `/config` | `config.json`, `routes.json`, `mediamtx.yml` (own preview mode) |
| `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`, `MXL_HISTORY_DURATION_NS`, `MXL_CLEANUP_ON_EXIT` | `/Volumes/mxl`, `/Volumes/mxl/scopes-<id>`, UUIDv5 of the seed, 200 ms, `false` | |
| `NMOS_*`, `WEB_*`, `LOG_LEVEL`, `LOG_FORMAT`, `SHUTDOWN_TIMEOUT_S` | as the sibling functions | |

## API (M2)

| Path | |
| --- | --- |
| `GET /api/v1/info`, `/api/v1/status` (`/statusz`), `/api/v1/input`, `/api/v1/output` | state, SIMD level, ids, counters, the modes built; the output's plot and panels |
| `GET`, `PATCH /api/v1/scope` | display settings, live; 400 on an invalid value |
| `GET /api/v1/snapshot.png` | the latest scope picture |
| `GET /widgets`, `GET /widget/scope?controls=&measurements=&theme=dark\|light\|transparent` | the operator-screen widget: the scope picture (WebRTC, else HLS, else the snapshot), optionally the mode and update rate and one line of measurements; `frame-ancestors` from `WIDGET_FRAME_ANCESTORS` |
| `GET /api/v1/measurements` | luma min, max, mean and legal shares; chroma out of range; R′G′B′ out of gamut (per mode, null where the mode has no plane; also on the WebSocket and as metrics) |
| `GET /api/v1/events` | WebSocket, status and settings five times a second |
| `GET`, `PUT /api/v1/config`; `GET /api/v1/config/export`, `POST /api/v1/config/import`, `GET /api/v1/config/env` | settings with their origin |
| `/livez`, `/readyz`, `/metrics` | probes; Prometheus, prefix `mxl_scopes_` |

## Build

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # scopes-core, unit tests, benchmark
cmake --build build && ./build/unit-tests
./build/mxl-scopes-bench --rasters 1920x1080 --strips 64 --steps 1/1 --modes luma,rgb-parade   # engine time per update
```

With libmxl (and `-DSCOPES_WITH_NMOS=ON -DNMOS_CPP_DIR=...`) the build adds `mxl-scopes`, `mxl-scopes-writer` (test patterns) and `mxl-scopes-sample`; `mxl-scopes` needs the FFmpeg libraries (`libavcodec-dev libavformat-dev libavutil-dev`) for the preview, and builds the web UI (`web/`, Vue 3 and Vite, one embedded HTML file) with npm when npm is there, else embeds a `web/dist/index.html` built before. `docker/Dockerfile` builds everything at the pins (a Node stage for the UI, MediaMTX from its image).

## Lab run

The image `ghcr.io/leeo86/mxl-scopes` (`1.0.0`; `nightly-dev` from `main`) needs no GPU:

```sh
docker run --rm --name scopes --network host --cpuset-cpus 25 \
  -v /Volumes/mxl:/Volumes/mxl -v scopes-config:/config \
  -e NMOS_REGISTRY_ADDRESS=<registry ip> -e NMOS_HOST_ADDRESS=<host ip> \
  -e NMOS_SEED=lab-scopes-1 -e MXL_CLEANUP_ON_EXIT=true \
  ghcr.io/leeo86/mxl-scopes:1.0.0
```

Route the receiver "Scopes In Video" over IS-05, open `http://<host>:8170/`, and route the sender "Scopes Out Video" to a multiviewer tile. Without a controller, `mxl-scopes-writer --domain DIR --flow UUID --pattern bars75` (or `scene`, a colour picture) in the same image writes a test flow, and a `routes.json` in `/config` routes it at start.

## Exit codes

0 `--help`/`--version`, 75 a port or another startup failure, 78 invalid configuration, 143 SIGTERM/SIGINT.

## Licence

MIT (`LICENSE`).

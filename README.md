# mxl-scopes

Video scopes for MXL: a media function that reads one MXL video flow and writes a waveform monitor (luma first, then YCbCr and RGB parades) as an ordinary MXL video flow. The multiviewer or any other MXL function can show it, keyed over the picture if wanted. Operators also see it in a browser and on operator screens.

**Status: design (decisions taken 2026-10-14).** There is no code yet. Read [SPECIFICATION.md](SPECIFICATION.md) and [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).

## At a glance

- C++20, one process per scope, one video input per instance.
- Input `video/v210` (or `v210a`), BT.709, up to 3840×2160.
- Output `video/v210`, or `video/v210a` with a key for overlays, default 1920×1080 at the input rate.
- Modes: `luma`, `ycbcr-parade`, `rgb-parade`, `rgb-overlay`. Later: `vectorscope`, `luma-chroma`, HDR.
- Configurable update rate (`frame`, or 1–60 Hz) and reduced sampling, to save CPU.
- CPU engine with AVX2 and a scalar reference, bit-exact; a CUDA backend can be added later.
- NMOS IS-04/IS-05 with BCP-007-03; web UI and REST; `/livez`, `/readyz`, `/metrics`; the `scope` widget; WebRTC preview through the platform's preview contract.

## Ports (proposed, not final)

| Port | Setting | Default |
| --- | --- | --- |
| Web UI, REST, probes, metrics, widgets | `WEB_PORT` | 8170 |
| NMOS Node and Connection APIs | `NMOS_PORT` | 3322 (+1 reserved) |
| Built-in MediaMTX RTSP / WHEP / HLS / ICE (only without `PREVIEW_PUBLISH_URL`) | `MEDIAMTX_*_PORT` | 8574 / 8579 / 8578 / 8279 |

## Exit codes

0 `--help`/`--version`, 75 a port or another startup failure, 78 invalid configuration, 143 SIGTERM/SIGINT.

## Licence

MIT (`LICENSE`).

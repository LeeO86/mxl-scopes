# mxl-scopes 1.0.0

The first release. It is a waveform monitor for MXL: it reads one MXL video flow and writes the scope as an ordinary MXL video flow. A multiviewer tile, or any MXL reader, can show it. The release covers milestones M0–M2 of IMPLEMENTATION_PLAN.md and the stage-1 scopes of SPECIFICATION.md. The WebRTC preview, the `scope` widget, the AMWA tests and the Kubernetes deployment file come in M3–M4. The changes are listed in CHANGELOG.md under "1.0.0".

## Image

```
ghcr.io/leeo86/mxl-scopes:1.0.0   (also 1.0, 1)
ghcr.io/leeo86/mxl-scopes@sha256:<OCI index digest, filled in after the tag build>
```

- Git tag `v1.0.0` = commit `<sha>`.
- MXL `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, also in the label `io.dmf.mxl.revision`.
- nmos-cpp `fe303849527394b03bdedc8f161f377fe458bb62`.
- Runs as uid 1000. No GPU needed.

## What is in it

- **Modes**, switched live (the output flow id stays):
  - `luma`, `ycbcr-parade`, `rgb-parade`, `rgb-overlay`, `ycbcr-overlay`;
  - graticule in mV, % or code, limited or full range;
  - update rate `frame` or 1–60 Hz (the overlays at most 30 Hz: 25 Hz from 50p);
  - `SCOPES_ACCUMULATE=latest` or `all`;
  - line and pixel steps.
- **Input:** one `video/v210` or `v210a` flow per instance, progressive or interlaced (1080i50 is analysed field pair by field pair), up to 3840×2160, BT.709. The grain is analysed in place in the MXL ring, with a check that it was not overwritten during the read (torn read).
- **Output:** `video/v210`, or `video/v210a` with a key when `SCOPES_OUTPUT_KEY=true`.
  - The key: background `SCOPES_BG_ALPHA`, trace at its intensity, graticule at its level, texts opaque.
  - Raster `SCOPES_OUTPUT_FORMAT` (default 1920×1080). Rate `SCOPES_OUTPUT_RATE` (default `input`).
  - Format, rate and key apply live and get a new flow id.
- **Engine:** integer histograms; scalar, AVX2 and AVX-512 paths that give identical bytes (`auto` = AVX2). 32-bit counters where 16 bits do not suffice. `SCOPES_THREADS` 1–8 splits each update over several cores; the picture is the same for any count.
- **Measurements** per update:
  - luma min, max and mean code, share below 64 and above 940;
  - Cb/Cr share outside 64–960;
  - R′G′B′ share out of gamut (EBU R 103), counted exactly per pixel.
  - A value the mode has no data for is null.
  - Available at `GET /api/v1/measurements`, on the WebSocket and the web page, and as metrics.
- **NMOS:** IS-04 and IS-05, with route persistence. **REST:** info, status, input, output, scope, measurements, snapshot PNG, config with export and import. **Web page:** a snapshot with live controls.

## For the platform

This is a new application, so nothing on the platform changes in place.

### Network and ports

- Pod network.
- `WEB_PORT` 8170: web page, REST, `/livez`, `/readyz`, `/metrics`, WebSocket.
- `NMOS_PORT` 3322, with 3323 reserved for the NMOS WebSocket.
- A port that cannot be bound exits 75. Invalid configuration exits 78. SIGTERM exits 143.

### Environment

Precedence: environment, then the config file (`SCOPES_CONFIG_FILE`, else `<SCOPES_STATE_DIR>/config.json`), then the defaults.

| Variable | Default | Applies |
| --- | --- | --- |
| `SCOPES_MODE` | `luma` | live |
| `SCOPES_UPDATE_RATE` | `25` | live |
| `SCOPES_ACCUMULATE` | `latest` | live |
| `SCOPES_LINE_STEP`, `SCOPES_PIXEL_STEP` | 1, 1 | live |
| `SCOPES_RANGE`, `SCOPES_GRATICULE_UNITS`, `SCOPES_GRATICULE_LEVEL` | `limited`, `mv`, 60 | live |
| `SCOPES_INTENSITY`, `SCOPES_TRACE_MIN`, `SCOPES_TRACE_COLOR`, `SCOPES_CAPTION` | 100, 96, `green`, `true` | live |
| `SCOPES_BG_ALPHA` | 0 | live |
| `SCOPES_HOLD_MS`, `SCOPES_INPUT_OFFSET_GRAINS` | 1000, 0 | live |
| `SCOPES_OUTPUT_FORMAT`, `SCOPES_OUTPUT_RATE`, `SCOPES_OUTPUT_KEY` | `1920x1080`, `input`, `false` | live, with a new output flow id |
| `SCOPES_THREADS` | 1 | at start |
| `SCOPES_SIMD` | `auto` | at start |
| `SCOPES_BACKEND` | `auto` | at start |
| `SCOPES_STATE_DIR`, `SCOPES_CONFIG_FILE` | `/config`, empty | at start |
| `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`, `MXL_HISTORY_DURATION_NS`, `MXL_CLEANUP_ON_EXIT` | `/Volumes/mxl`, `/Volumes/mxl/scopes-<id>`, UUIDv5 of the seed, 200 ms, `false` | at start |
| `NMOS_*`, `WEB_*`, `LOG_LEVEL`, `LOG_FORMAT`, `SHUTDOWN_TIMEOUT_S` | as the sibling functions | at start |

- Set `MXL_CLEANUP_ON_EXIT=true` on the platform.
- `PREVIEW_*`, `MEDIAMTX_*`, `SCOPES_PREVIEW_*` and `WIDGET_FRAME_ANCESTORS` are validated but have no effect yet; the preview is M3. Set `SCOPES_PREVIEW_MODE=off`. With the default `webrtc`, `/api/v1/info` reports the preview as `not_implemented`.

### NMOS

- The node and device take `NMOS_LABEL` and `NMOS_TAGS`.
- One receiver, "Scopes In Video" (group hint `Scopes In:Video`): `video/v210` and `video/v210a`.
- One source, flow and sender, "Scopes Out Video" (group hint `Scopes Out:Video`).
  - The flow's `description` is the mode title, for example "Luma waveform".
  - The flow and the sender carry the tag `urn:x-mxl:tag:scope-mode/v1.0` = `[<mode>]`.
  - Ids are UUIDv5 from `NMOS_SEED`. The flow id is derived from the output format, rate and key (`flow/<width>x<height>p<rate>[a]`), so a mode change keeps it.

### Metrics

All metrics start with `mxl_scopes_`. They are all new.

- `info{version,mxl_revision,backend,simd}`, `simd_level{level}`
- `input_state{state}`, `input_grains_total`, `input_resyncs_total`
- `updates_total{mode}`, `updates_skipped_total`, `updates_torn_total`, `update_rate_hz`
- `analyse_seconds{mode}`, `analyse_cpu_seconds{mode}`, `render_seconds{mode}`
- `accumulate_dropped_total`
- `output_frames_total`, `output_frames_late_total`, `output_frames_missed_total`
- `nmos_registry_up`, `nmos_activations_total`
- Measurements (7 gauges, each absent while its value is null):
  - `luma_min_code`
  - `luma_max_code`
  - `luma_mean_code`
  - `luma_below_legal_ratio`
  - `luma_above_legal_ratio`
  - `chroma_out_of_range_ratio`
  - `rgb_out_of_gamut_ratio`

### Catalog suggestion

See SPECIFICATION §15:
- `default_values` `{mode: luma, updateRate: 25, format: 1920x1080, key: false}`;
- a flow `1080p50-v210`, or `1080p50-v210a` with `key: true`;
- no GPU;
- the requests below.

### CPU

Measured on the lab host: a Skylake-SP Xeon Gold 6136 using AVX2. The platform's i9-9900K is expected to be lower. Input was a camera clip at 1080p50, output 1080p50, preview off. Figures are cores per process (all threads), at 25 Hz / every frame. Details are in `docs/performance.md`.

| Mode | 1 thread | 2 threads (`SCOPES_THREADS=2`) |
| --- | --- | --- |
| `luma` | 0.23–0.24 / 0.36–0.37 | 0.29–0.30 / 0.43–0.44 |
| `ycbcr-parade` | 0.24–0.25 / 0.39–0.42 | 0.29–0.30 / 0.43–0.44 |
| `rgb-parade` | 0.37–0.39 / 0.64–0.68 | 0.46–0.47 / 0.71–0.72 |
| `rgb-overlay` (at most 25 Hz) | 0.55 | 0.62–0.63 |
| `ycbcr-overlay` (at most 25 Hz) | 0.41–0.42 | 0.47–0.48 |

- Every point had 0 late and 0 missed output grains, 0 torn reads and 0 skipped ticks.
- Two threads halve the time an update takes. For example, the RGB parade every frame goes from 11 ms to 7 ms; its benchmark p99 is 7.1 ms. They cost slightly more CPU. Use them for the overlays or for every-frame parades, not to save CPU.
- Suggested requests: `cpu: 500m` for one scope at the default 25 Hz, `memory: 512Mi`. RSS is not measured yet; the §13 target is under 300 MB at 1080p.

## Known limits

- **The key does not work in multiviewer 1.4.4 on the CPU backend.** A v210a scope tile over a picture tile does not show the picture through the key: that backend blends a tile's key into the tile itself and copies the tile opaque onto the canvas. The scope's v210a output is correct (key samples were checked in the integration test and in the lab). This needs a fix in the multiviewer; the CUDA backend was not tried.
- **RGB parade every frame is above the §13 target:** 0.64–0.68 core against "< 0.4". The maintainer accepted this as deviation 25. The default of 25 Hz takes 0.37–0.39 core.
- **No WebRTC preview or `scope` widget yet** (M3). The web page shows a PNG snapshot twice a second.
- **Not yet measured on the desktop class** (the platform's i9).

## Please report back

- The digest you pin.
- CPU per scope on small: `luma` at 25 Hz, and one RGB mode.
- Late or missed output grains (`mxl_scopes_output_frames_late_total`, `…_missed_total`).
- Whether the measurement gauges reach the dashboards.

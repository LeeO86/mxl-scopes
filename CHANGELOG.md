# Changelog

## Unreleased (M2, 2026-10-22)

- 32-bit histogram cells when 16 bits do not hold an update (UHD on a small output, a parade, `all`): every line is analysed; the line step is no longer raised.
- `SCOPES_THREADS` 1–8: strip workers share the analyse and the trace step; the same picture for any thread count. RGB parade every frame at 1080p50: about 6 ms per update on two cores instead of 10 (lab), the CPU in all about the same.
- Measurements (§4.7) per mode: luma min, max, mean and legal shares, chroma out of range, R′G′B′ out of gamut (counted exactly per pixel); `GET /api/v1/measurements`, the WebSocket, the web page, metrics.
- `SCOPES_OUTPUT_KEY=true`: `video/v210a` with a key plane (background `SCOPES_BG_ALPHA`, trace, graticule, texts).
- `SCOPES_OUTPUT_FORMAT`, `SCOPES_OUTPUT_RATE` and `SCOPES_OUTPUT_KEY` apply live with a new output flow id (`PUT /api/v1/config` lists them as `flow_keys`).
- Mode `ycbcr-overlay`: Y′ white, Cb blue, Cr red on one code axis; capped like the RGB overlay.
- `renderer.threads` in the status; `mxl-scopes-bench --threads`; `mxl-scopes-sample --key-height`; a CI job runs the thread tests under ThreadSanitizer.

## Unreleased (M1, 2026-10-16)

- Decisions of 2026-10-22: `rgb-overlay` updates at most every n-th input frame, at most 30 Hz (25 Hz from 50p, 29.97 Hz from 59.94p), instead of skipping ticks at `frame`; the RGB parade CPU every frame is accepted for M1; SPECIFICATION §2 describes the trace computed on the analysing thread.

- Modes `ycbcr-parade`, `rgb-parade` and `rgb-overlay`: scalar reference, AVX2 and AVX-512 analyse steps, byte-exact; R′G′B′ in Q13 (§4.8); three-panel parade layout with a Cb/Cr graticule; the overlay adds R′, G′ and B′; a live mode change keeps the output flow id and updates the NMOS flow description and mode tag; metrics per mode.
- `SCOPES_ACCUMULATE=all`: every frame (or field) between two updates goes into the update.
- The trace is computed on the analysing thread; the writer only patches the groups that differ, so a slow update skips ticks and never delays an output grain.
- The AVX2 analyse step multiplies with `vpmaddwd`: luma 15 % faster, same bytes.
- No cell overflow can end the process: the line step is raised when the counts would not fit 16 bits (a UHD input on a small output, a parade, `all`), shown as `renderer.line_step`; other table errors show `FORMAT`. A settings change is drawn at once. `all` takes every grain in turn, counts what it cannot add (`mxl_scopes_accumulate_dropped_total`) and normalises to the grains of each update.
- `mxl-scopes-bench --modes`, the `scene` pattern, golden pictures of every mode, `docs/performance.md` with the CPU per mode.

## Unreleased (M0, done 2026-10-15)

- Review fixes (2026-10-16): interlaced input (MXL field grains, 1080i50 tff and bff) with grain-size checks; the reader no longer spins on invalid or partial head grains; non-blocking HTTP server that drops a stalled WebSocket client; a taken NMOS port exits 75; counters print as integers; a stricter torn-read check; cleanup removes only our own domain; config import needs JSON and validates routes.

- Measured on the lab class (Skylake-SP, 1080p50 in and out, AVX2): about 0.25 core at 25 Hz and 0.45 core every frame for a real picture, bars 0.1–0.15 and 0.2 core; SPECIFICATION §13 updated. The mostly idle core runs at a lower clock; a performance power profile on the host lowers the figures.
- The render step skips empty stretches and leaves rows without a trace unwritten; `analyse_cpu_seconds` exports the analyse thread's CPU time.

- Luma waveform engine: scalar reference, AVX2 and AVX-512 analyse steps (run-time dispatch AVX-512 → AVX2 → scalar), bit-exact; intensity grows with the hits per cell (softer curve, §4.4); integer intensity and colour tables; graticule in mV, % and code, limited and full range; caption and state texts; packed v210 output written line by line.
- MXL input: domain scan, the routed flow analysed in place in the ring with a torn-read check, `waiting`, `running`, `holding`, `no_signal`, `format_mismatch`, re-open on flow re-creation.
- MXL output: own domain (BCP-007-03 `domain_def.json`), TAI-paced writer that renders straight into the output grain and rewrites only the trace groups that differ per ring slot (no render buffer, no full copy), output rate `input` or fixed; a new rate mints a new flow id.
- Update rate `frame` or 1–60 Hz on TAI ticks; line and pixel steps.
- NMOS node: one receiver, one sender, UUIDv5 ids, route persistence and restore into IS-05 and IS-04.
- REST: info, status, input, output, scope (GET, PATCH), snapshot PNG, config with export, import and `KEY=value`; `/livez`, `/readyz`, `/metrics`; WebSocket events.
- Web page: snapshot with live controls, input, NMOS, status and settings tabs.
- `mxl-scopes-bench`, `mxl-scopes-writer`, `mxl-scopes-sample`; unit and integration tests; CI and the container image.

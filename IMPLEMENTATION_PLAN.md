# mxl-scopes — Implementation plan

This plan comes before any code. It is updated as the work goes on: every decision and every deviation from `SPECIFICATION.md` is recorded here, as in the sibling repos.

## Status (2026-10-14)

Design draft. No repository yet. The open questions in SPECIFICATION §16 need the maintainer's answers before M1.

## Pins

| Component | Pin |
| --- | --- |
| MXL | `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, `-DMXL_ENABLE_FABRICS_OFI=OFF`; one `MXL_REF` in `docker/Dockerfile` and `.github/workflows/ci.yaml` |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` (the siblings' commit) |
| Blend2D, DejaVu Sans | the versions mxl-multiviewer uses (graticule drawing only) |
| FFmpeg | Ubuntu 24.04 system libraries (preview) |
| MediaMTX | the version mxl-multiviewer pins (preview own mode, tests) |
| doctest | vendored |

## Source layout (planned)

```
src/
  main.cpp, version.hpp
  config/        env > file > default, validation
  domain/        MXL root scan, mirror marker, own domain
  scope/         engine interface, settings, LUTs, layout, graticule
  scope/cpu/     scalar reference, avx2 (one TU, -mavx2 -mfma), dispatch
  media/         v210 unpack/pack, measurements, timebase
  mxlio/         reader, renderer, writer threads (needs libmxl)
  nmos/          ids, node (nmos-cpp behind SCOPES_WITH_NMOS)
  preview/       NV12 scale, encoder, RTSP publisher, MediaMTX supervisor
  ops/           HTTP, WebSocket, REST, widgets, metrics
web/             Vue 3 UI and the scope widget
tests/unit/      doctest, no libmxl
tests/integration/scope.sh, preview.sh
tests/tools/     pattern writer (bars, ramp, flat, random), output sampler
tests/nmos/amwa.sh
docker/ deploy/ docs/
```

`scopes-core` (config, scope, media) links neither libmxl nor nmos-cpp, so the bit-exact tests run anywhere.

## Milestones

### M0: spike and skeleton

Goal: prove the cost estimate (SPECIFICATION §4.9) before the rest is built.

- Repository scaffold (only after the maintainer creates or approves the repo), CMake, pins, CI that builds MXL and nmos-cpp, doctest.
- Scalar reference engine for `luma`: unpack, bin, per-column histogram with strips and two sub-histograms, intensity LUT, render, v210 pack.
- AVX2 engine for `luma`; run-time dispatch (`SCOPES_SIMD`).
- Bit-exact unit tests, SIMD vs scalar (random and edge frames, odd widths, steps, thread counts).
- Golden pictures: ramp, 75 % and 100 % bars, flat grey.
- Micro-benchmark tool `mxl-scopes-bench`: analyse and render time per mode, raster, step and SIMD level, from generated frames.
- Measure on both host classes (AVX2 desktop CPU; Skylake-SP with AVX-512): luma at 1080p and 2160p, avx2 and scalar, strip widths 32/64/128, sub-histograms 1/2/4. Record the results here and fix the strip width.
- Decide open question 8 (AVX-512) from these numbers.

Exit: AVX2 luma analyse + render < 4 ms at 1080p on the desktop class, bit-exact with scalar. If not, revise §4.3 before M1.

### M1: luma waveform end to end

- Config (env > file > defaults, exit 78), state directory, exit codes.
- MXL: domain scan, own output domain (G2), reader thread (no copy, torn-grain check), renderer thread with update-rate scheduler (`frame` and Hz, `latest` and `all`), writer thread (TAI paced, triple buffer).
- Graticule layer (mV, %, code; limited and full range) with Blend2D, drawn on settings change.
- NMOS node, receiver and sender (BCP-007-03), UUIDv5 ids, route persistence and restore, IS-04 subscription.
- `/livez`, `/readyz`, `/statusz`, `/metrics` (renderer, writer, input metrics).
- Minimal REST: `/api/v1/info`, `/api/v1/status`, `GET/PATCH /api/v1/scope`, config routes.
- Failure handling of SPECIFICATION §11 for the input (waiting, holding, no signal, format change, resync).
- `tests/integration/scope.sh` (luma, v210).

Exit: a routed 1080p50 bars flow gives a correct luma waveform flow; the multiviewer shows it as a tile; integration test green in CI.

### M2: parades, overlays, key, measurements

- Modes `ycbcr-parade`, `rgb-parade`, `rgb-overlay` (and `ycbcr-overlay` if cheap), scalar first, then AVX2; R′G′B′ in Q13 with the double-reference test.
- `uint32` counters when the bound needs them.
- `SCOPES_OUTPUT_KEY` (v210a key plane), `SCOPES_BG_ALPHA`; output format and rate changes mint a new flow id.
- Measurements (§4.7) and their metrics; `GET /api/v1/measurements`.
- Caption line, state texts on the output.
- `SCOPES_THREADS` > 1 (strip workers), thread-count invariance test.
- Golden pictures for every mode; integration test extended (v210a, mode switch without new flow id).
- Benchmark every mode on both host classes; record here.

Exit: all stage-1 modes bit-exact SIMD = scalar; keyed scope over a picture on the multiviewer in the lab.

### M3: web UI, preview, widget, export

- Preview contract: NV12 scaler, encoder (NVENC, else x264), RTSP publisher with reconnect, MediaMTX supervisor (own mode), shared mode; path `<prefix>/scope`; preview metrics and status.
- Vue 3 UI in the family look: Scope, Input, NMOS, Status, Settings tabs; WebSocket events.
- `scope` widget (`controls`, `measurements`, `theme`), `/widgets` with CORS, CSP, messages.
- `GET /api/v1/snapshot.png` (SHOULD).
- Config export and import, `KEY=value` export.
- `tests/integration/preview.sh`.
- Lab: judge the preview quality of thin traces at 540p (open question 11).

Exit: an operator sees and switches the scope in the widget on an operator screen.

### M4: hardening, platform, release 1.0.0

- Performance runs of SPECIFICATION §13 on both host classes, 1 h each, preview on and off; `docs/performance.md`.
- 1 h soak with 0 late output grains; two instances on one node.
- AMWA IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01 (`tests/nmos/amwa.sh`).
- Container image with OCI and MXL labels, tags, Compose demo, `deploy/k8s/deployment.yaml`, Grafana dashboard.
- README (settings, ports, exit codes, API, widgets, platform notes), CHANGELOG, AGENTS.md.
- Platform hand-off: catalog entry, chart type, productions row and the measured resources (SPECIFICATION §15), as a note to the platform maintainers (they own that repo).
- Release 1.0.0 with the platform release notes (Appendix B.3 of the convention).
- A design note for the CUDA backend (stage 4): histogram kernel with shared-memory atomics per column strip, render and pack on the device, the same integer LUTs, a GPU = CPU byte test.

Exit: 1.0.0 released and deployable by the platform.

## After 1.0

| Release | Work |
| --- | --- |
| 1.1 | vectorscope, `luma-chroma`, multi-pane layouts, persistence |
| 1.2 | HDR (PQ, HLG, BT.2020) |
| 2.0 | CUDA backend; single-line select; false colour |

## Decisions

1. One input per instance (SPECIFICATION §6).
2. No grain copy: analyse in the MXL ring, check for a torn read afterwards.
3. Integer histograms and integer LUTs, so every backend is byte-identical.
4. Column-strip cache blocking with two sub-histograms against store-forwarding stalls.
5. No gathers in AVX2 code; AVX-512 deferred to the M0 numbers.
6. The output runs at full rate; the update rate only changes how often the picture changes.
7. Neutral mode names; the Y-against-chroma display is `luma-chroma`.

## Deviations

None yet.

# mxl-scopes — Implementation plan

This plan comes before any code. It is updated as the work goes on: every decision and every deviation from `SPECIFICATION.md` is recorded here, as in the sibling repos.

## Status (2026-10-15)

M0 done (2026-10-15), PR #1 (`feat/m0`) in review before the merge; M1 next. The open questions of SPECIFICATION §16 are decided (§16 there). Process CPU on the lab class, 1080p50 in and out, AVX2: real picture (camera) about 0.25 core at 25 Hz and about 0.45 core every frame, bars 0.09–0.14 and 0.20–0.22 core; accepted by the maintainer as the §13 figures (§13 updated).

M0 was widened by the maintainer's brief of 2026-10-15 to a working vertical slice (deviation 1): environment config, MXL video in, luma waveform, MXL video out, REST with a snapshot picture, metrics, NMOS, the web page, the image and CI. The spike part of M0 (engine, bit-exact tests, benchmark, numbers) is done; what is left is listed under M0 below.

## Pins

| Component | Pin |
| --- | --- |
| MXL | `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, `-DMXL_ENABLE_FABRICS_OFI=OFF`; one `MXL_REF` in `docker/Dockerfile` and `.github/workflows/ci.yaml` |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` (the siblings' commit) |
| Blend2D, DejaVu Sans | the versions mxl-multiviewer uses (graticule drawing only); not used in M0 (deviation 6) |
| FFmpeg | Ubuntu 24.04 system libraries (preview); not used before M3 |
| MediaMTX | the version mxl-multiviewer pins (preview own mode, tests); not used before M3 |
| doctest | vendored (2.4.12) |
| picojson, stb_image_write, font8x8 | vendored (`NOTICE`) |

## Source layout (planned; M0 state below)

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

M0 as built: `src/config` (schema, store), `src/scope` (`analyse`, `render`, `engine`, `scheduler`, `settings`), `src/scope/cpu` (`avx2.cpp` with `-mavx2 -mfma`, `dispatch`, `group.hpp` the shared reference step), `src/media` (`v210`, `patterns`, `png`, `timebase`), `src/app` (`state`, `triplebuffer`), `src/mxlio/io.cpp` (renderer and writer threads), `src/nmos` (`ids`, `node`), `src/ops` (`api`, `httpserver`, `metrics`), `web/index.html`, `tools/bench.cpp`, `tests/unit`, `tests/tools`, `tests/integration/scope.sh`, `docs/golden`.

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

Progress (2026-10-15):

- [x] Repository scaffold, CMake, pins, CI that builds MXL and nmos-cpp, doctest.
- [x] Scalar reference engine for `luma`: unpack, bin, per-column histogram with strips and two sub-histograms, intensity LUT, render, v210 pack.
- [x] AVX2 analyse step for `luma`; run-time dispatch (`SCOPES_SIMD`, cpuid and XGETBV). Render and pack are shared integer code for both levels (byte-identical by construction).
- [x] Bit-exact unit tests, AVX2 vs scalar: random and edge frames (all codes 0–1023), widths 1–1280 incl. not multiples of 6 or 48, every line and pixel step, column counts 1..width, bins 1..1024, strip widths, padded strides; whole pictures byte for byte at 720p, 1080p, 2160p. Thread counts: M2 (one renderer thread in M0).
- [x] Golden pictures: ramp, 75 % and 100 % bars, flat grey; checked by trace position and by hash; PNGs in `docs/golden` (`mxl-scopes-bench --golden`).
- [x] `mxl-scopes-bench`: analyse and render time per raster, SIMD level, strip width and step.
- [x] Measured on the Skylake-SP class (lab) and an AVX2 server CPU (CI runner); strip width fixed at 64.
- [ ] Measure on the desktop class (8-core AVX2 desktop CPU). Not available tonight.
- [x] Open question 8 (AVX-512): first "not in v1"; the maintainer decided on 2026-10-15 to add it. AVX-512 analyse step with run-time dispatch AVX-512 → AVX2 → scalar, byte-exact in the tests (see "M0 rework").
- [x] Maintainer's answers of 2026-10-15: softer intensity curve (§4.4), AVX-512, the copy-free writer before any M1 work, ports 8170/3322 confirmed.
- [x] Vertical slice (deviation 1): config, MXL reader and writer, REST, probes, metrics, NMOS, web page, Dockerfile, CI, integration test.

M0 is done. Carried to M1: the desktop-class numbers; a lab run of the image (README "Lab run") with a real camera picture and the multiviewer tile.

### M0 numbers

`mxl-scopes-bench`, one thread pinned to one core, 1920×1080 output, median ms (p99 in brackets). Analyse = histogram of one input frame; render = histogram to packed v210 (shared code, so equal for both levels). "bars" is 75 % colour bars (flat areas), "noise" is uniformly random codes (worst case: every sample a random cell).

Lab host class, Skylake-SP (Xeon Gold 6136, 3.0 GHz, AVX-512 present but unused), other agents on the socket:

| Input | Step | Scalar analyse | AVX2 analyse | Render | AVX2 total |
| --- | --- | --- | --- | --- | --- |
| 1080p bars | 1/1 | 6.0 | 1.73 (1.86) | 1.73 | 3.48 (3.82) |
| 1080p bars | 2/1 | 2.5 | 0.87 | 1.66 | 2.55 |
| 1080p bars | 2/2 | 1.4 | 0.63 | 1.58 | 2.21 |
| 1080p noise | 1/1 | 6.5 | 3.37 (3.61) | 4.23 | 7.55 (7.96) |
| 2160p bars | 1/1 | 23.4 | 9.17 (9.42) | 1.79 | 10.97 (11.22) |
| 2160p bars | 2/1 | 12.7 | 5.73 | 1.78 | 7.47 |
| 2160p noise | 1/1 | 27.7 | 14.6 (14.8) | 4.21 | 18.8 (19.0) |

AVX2 server class (CI runner, AMD EPYC 7763), before the render rework (render then 2.9 ms): 1080p bars AVX2 analyse 1.16 ms, scalar 6.2 ms (old byte-wise unpack); 2160p bars AVX2 analyse about 4.6 ms.

Strip width (AVX2 analyse, ms): 1080p bars 1.73 / 1.65 / 1.85 and noise 3.37 / 3.28 / 3.52 at 64 / 128 / 512 columns; 2160p bars 9.17 / 9.21 / 9.39 and noise 14.6 / 15.5 / 20.2. 64 is best or within 6 % of the best everywhere: fixed at 64 (`kDefaultStripColumns`).

Whole process, lab class (2026-10-15, `mxl-scopes` against the pinned libmxl, NMOS off, pinned to one core; `mxl-scopes-writer` 1080p50 75 % bars on another core; 1080p50 output): 25 Hz 0.23 core (renderer thread 0.16, writer thread 0.07), every frame 0.29 core; 0 late, 0 missed, 0 skipped, 0 torn grains over the run. Analyse inside the process averages 3.4 ms against 1.7 ms in the benchmark: the grain was just written from another core, and at 25 Hz the renderer sleeps between ticks.

Findings:

- Exit criterion: AVX2 analyse + render is 3.5 ms (p99 3.8 ms) for a 1080p picture with flat areas on the lab class, below 4 ms already on the slower class; uniform noise takes 7.6 ms. The desktop class is still to be measured. The 2160p target of §13 (< 12 ms) holds for bars (11.0 ms) and not for noise (18.8 ms); line step 2 brings noise to 12.6 ms.
- Scalar 1080p (p99 < 15 ms, §13): met (8.2 ms bars, 10.9 ms noise).
- The hot loop is the scalar increment, as §4.9 expected; the AVX2 unpack, bin and index step makes analyse 2.5–3.5 times faster than scalar. AVX-512 widens only that step (see "M0 rework" for its numbers).
- The column strips of §4.3 read a few cache lines per row at a stride above 4 KiB, which the hardware prefetcher does not follow; the loops prefetch six rows ahead.
- Render cost is independent of the input raster. It was 2.7–3 ms with a planar intermediate picture and a full pack; packing only the 6-pixel groups that carry a trace over the packed still picture brought it to 1.7 ms for typical pictures (4.2 ms for noise, where every group has a trace). An AVX2 pack is the next step if noise matters.
- §4.9's estimate (1.5–2 ms per luma update on a 4.5 GHz desktop) looks reachable on that class; on the 3 GHz lab class it is 3.5 ms.
- §13 process targets (< 0.15 core at 25 Hz, < 0.25 core every frame) are not met on the lab class (0.23 and 0.29). The next savings: render straight into the output grain when an update falls on it and skip the writer's 5.5 MB copy (§2.4 allows it; about 0.07 core), and an AVX2 pack for pictures with many trace groups. To be re-measured on the desktop class.

### M0 rework (2026-10-15, after the maintainer's answers)

**Intensity (§4.4).** `c_ref` is now the samples of a whole column (a flat field's cell) and the gain 20 (`kIntensityGain`, scaled by `SCOPES_INTENSITY`): one hit is dim (`SCOPES_TRACE_MIN`), ten hits about 15 %, 1/20 of a column 63 %, a flat field saturates. Golden picture `camera` (smooth, picture-like) added; the bars, ramp and flat pictures saturate as before, their hashes are unchanged.

**AVX-512.** `src/scope/cpu/avx512.cpp` (AVX-512F only; the lab Xeon has no VBMI2): the AVX2 step on 512-bit registers, four groups per load; strips are now aligned to four groups. `SCOPES_SIMD=avx512` selects it; `auto` stays on `avx2` (below). Byte-exact against scalar in the tests (widths 1–1280 incl. 23, 24, 95, 96, 97, every step, bins, strips, edge codes; whole pictures at 720p, 1080p, 2160p and 1998×50). Per-path timings, lab Skylake-SP, one core on socket 1, median ms of analyse (render is shared):

| Input | Scalar | AVX2 | AVX-512 |
| --- | --- | --- | --- |
| 1080p bars | 4.5 | 1.76 | 1.85 |
| 1080p camera | 5.3 | 2.57 | 2.65 |
| 1080p noise | 6.7 | 3.55 | 3.64 |
| 2160p bars | 25.1 | 9.40 | 9.57 |
| 2160p camera | 26.7 | 12.9 | 13.5 |
| 2160p noise | 31.2 | 14.4 | 16.7 |

AVX-512 is 2–15 % slower than AVX2 here: the scalar increments dominate, and the wider load only moves work that was not the bottleneck. The CI runner of 2026-10-15 (AMD EPYC 9V74, Zen 4) shows the same: 1080p bars analyse AVX2 0.88 ms, AVX-512 1.02 ms, scalar 3.58 ms; 2160p bars 4.85 / 6.26 / 14.4 ms. So `auto` picks AVX2 (deviation 13); AVX-512 stays selectable and tested.

**Copy-free writer (§2.4).** The renderer thread now only analyses and hands the histogram (with its render parameters) to the writer through a lock-free triple slot. The writer renders straight into the output grain: the picture is the still picture plus the trace groups, and each ring slot remembers what it holds, so a grain gets only the groups that differ; a large trace is one copy of the previous grain; a slot that already holds the picture is not written. Bytes equal to the full render (same golden hashes).

Whole process, lab Skylake-SP, `mxl-scopes` pinned to CPU 41 (socket 1), `mxl-scopes-writer` 1080p50 on CPU 43, 1080p50 output, `avx2`, NMOS off, 20 s per point (`pidstat`); renderer and writer threads in brackets. Target §13: 25 Hz < 0.15 core, every frame < 0.25 core.

| Input | 25 Hz | Every frame |
| --- | --- | --- |
| Before (render buffer + copy), bars | 0.23 (0.16 + 0.07) | 0.29 |
| Bars | 0.16 (0.10 + 0.05) | 0.24 (0.14 + 0.09) |
| Camera | 0.23 (0.13 + 0.10) | 0.35 (0.17 + 0.17) |
| Noise | 0.34 (0.14 + 0.20) | 0.51 (0.20 + 0.30) |

0 skipped (2 at a rate change), 0 torn; 2–4 late and 1–2 missed output grains per run, all at start-up. The every-frame target is met for bars; 25 Hz is 0.01 core above it. Inside the process the analyse step takes about 1.5–2 times its benchmark time (the grain comes from another core, and at 25 Hz the core sleeps between ticks); that, not the writer, is now the larger part.

### Analyse in the process (2026-10-15, maintainer's request before M1)

Why the analyse step took 1.5–2 times its benchmark time inside the process, measured with own timers (`perf` is blocked: `perf_event_paranoid` 4, no root):

- **Core clock (main cause).** The benchmark loops busily; the process is idle 80–90 % of the time, and the power management lowers the clock of a lightly loaded core. Sampled clock of CPU 41: median 2.2 GHz during a 25 Hz run, 3.3 GHz under the busy benchmark. The benchmark reproduces it: 1080p bars AVX2 analyse 1.9 ms busy, 4.7 ms with a 38 ms sleep before each update (`--interval-ms 38`), 1.9 ms when the same 38 ms are spun (`--spin 1`); short naps (`--nap-ms 1` or 4) do not help, so it is the clock, not the C-state. No software fix short of spinning a core; it needs a performance power profile on the host.
- **Thread placement.** The new `analyse_cpu_seconds` metric (thread CPU time of the step) equals the wall time for bars: no preemption. With camera and noise every frame, on one core, the writer's render delays the analyse step (wall 7.9 ms, CPU 4.2 ms for noise); two cores (41,45) lower the latency but not the CPU share, which rose (19.8 % against 15.2 % for bars at 25 Hz), since both cores clock down.
- **Cache.** A ring of 12 distinct frames (`--frames 12`) costs only 3–5 % in the benchmark: reading the ring in place is not the problem; no strip copy is needed.
- **Torn-read check:** one runtime-info read per update, not measurable.

Fixed: the render step skips empty stretches eight groups at a time and does not clear rows without a trace (1080p bars 2.0 → 1.6 ms, camera 2.85 → 2.75 ms, noise 5.4 → 5.8 ms; A/B, interleaved runs).

Process CPU after the fix, two runs each (the host's load average was 6–9 during the second), `mxl-scopes` on CPU 41, writer on CPU 43, AVX2, 1080p50 in and out. Targets §13: 25 Hz < 0.15 core, every frame < 0.25 core.

| Input | 25 Hz | Every frame |
| --- | --- | --- |
| Bars | 0.09 / 0.14 | 0.22 / 0.20 |
| Camera | 0.23 / 0.21 | 0.40 / 0.33 |
| Noise | 0.28 / 0.36 | 0.49 / 0.54 |

Bars meet both first targets; camera and noise did not on this host. The maintainer accepted the result (2026-10-15): §13 now gives about 0.25 core at 25 Hz and about 0.45 core every frame for real pictures, bars lower. The work per update is about the benchmark's; the CPU share grows because the idle core runs at two thirds of its clock.

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
5. No gathers or scatters in SIMD code; AVX-512 added on 2026-10-15 (fallback AVX-512 → AVX2 → scalar; `auto` = AVX2, deviation 13).
6. The output runs at full rate; the update rate only changes how often the picture changes.
7. Neutral mode names; the Y-against-chroma display is `luma-chroma`.

## Platform guideline G1–G14

| # | Rule | Status | Evidence | Note |
| --- | --- | --- | --- | --- |
| G1 | Env, then one JSON file, then defaults. Unknown env ignored. Invalid values exit 78. One settings table. State only under one directory, default `/config`. Secrets never logged. | met | `src/config/config.cpp` `loadConfig`, `settingSchema`; `src/config/store.cpp`; `src/main.cpp` | `SCOPES_CONFIG_FILE`, else `<SCOPES_STATE_DIR>/config.json`. No secrets. |
| G2 | Scan `/Volumes/mxl`. Own output domain from `MXL_OUTPUT_DOMAIN_DIR` / `MXL_OUTPUT_DOMAIN_ID`, created if missing. A different id in `domain_def.json` is logged and not overwritten. No writes into other domains. `history_duration` configurable. | met | `src/mxlio/io.cpp` `ensureDomain`; `src/domain/scan.cpp` | Default `/Volumes/mxl/scopes-<first 8 hex of the domain id>`. A mirror or an uncreatable directory exits 78. `options.json` only for a new domain. |
| G3 | `NMOS_SEED` UUIDv5 for node, device, source, flow, sender, receiver and the default domain id. `NMOS_LABEL`, `NMOS_TAGS` on node and device. Group hints. | met | `src/nmos/ids.cpp`, `src/nmos/node.cpp` | Names `mxl-scopes/<seed>/...` as SPECIFICATION §7.1. |
| G4 | Registry and query addresses; query defaults to the registry and port + 1. `NMOS_DNS_SD` false disables browse and advertisement. | met | `src/config/config.cpp`, `src/nmos/node.cpp` (`pri`, `highest_pri`) | As the siblings. |
| G5 | Announced addresses are IP literals from `NMOS_HOST_ADDRESS`, default the first non-loopback IPv4. | met | `src/config/config.cpp` `announceableIpv4`; `src/nmos/node.cpp` | Loopback or `0.0.0.0` exits 78. |
| G6 | Every listen port is a setting, NMOS WebSocket at `NMOS_PORT+1`. Bind failure exits 75. | met | `src/config/config.cpp` `checkPorts`; `src/main.cpp`; CI "Exit codes" | MediaMTX ports count only in own mode (M3). |
| G7 | `/livez`, `/readyz` (serving, and registered when a registry is set), `/metrics` with prefix `mxl_scopes_`. | met | `src/ops/api.cpp` `live`, `ready`; `src/ops/metrics.cpp` | Readiness also needs the output flow (§9.4). |
| G8 | SIGTERM within `SHUTDOWN_TIMEOUT_S`: stop media, DELETE the node, optionally remove only the output domain, exit 143. | met | `src/main.cpp`; `src/nmos/node.cpp`; `src/mxlio/io.cpp` `removeOwnDomain`; `tests/integration/scope.sh` | `MXL_CLEANUP_ON_EXIT` defaults false. |
| G9 | Senders report `mxl_domain_id` and `mxl_flow_id`; receivers take the staged PATCH; `master_enable: false` stops the reader; the route survives a restart. | met | `src/nmos/node.cpp`; `src/app/state.cpp` `saveRoute`, `loadRoute`; `tests/integration/scope.sh` | The restored route is also the IS-05 active state and the IS-04 subscription. |
| G10 | `GET /api/v1/config/export`, `POST /api/v1/config/import`; secrets omitted unless requested. | met | `src/ops/api.cpp`; `tests/unit/test_api.cpp` | No secrets; environment keys skipped on import and listed. |
| G11 | Actions builds and pushes `ghcr.io/leeo86/mxl-scopes`: `main` `git-<sha7>` and `nightly-dev`; `vX.Y.Z` `X.Y.Z`, `X.Y`, `X`. uid 1000. OCI labels. | met (not yet run) | `.github/workflows/container.yaml`, `docker/Dockerfile` | Runs after the merge. The PR builds the image without pushing (`ci.yaml` job `image`). No `latest`. |
| G12 | Pod network, standard env, probes, grace period, MXL hostPath, writable `/config`, no `hostIPC`. | open (M4) | — | `deploy/k8s/deployment.yaml` in M4. |
| G13 | README settings, ports, exit codes, API, platform run. CHANGELOG. SPEC matches the code. | partial | `README.md`, `CHANGELOG.md` | The deviations below are the differences to SPECIFICATION.md until they are folded in. |
| G14 | Unit tests for parsing and the new behaviour. Integration covers start, ready, SIGTERM, deregister and domain removal. CI green. | met | `tests/unit`, `tests/integration/scope.sh`, `.github/workflows/ci.yaml` | |

## Deviations

1. **M0 scope.** The maintainer's brief of 2026-10-15 asked for a working vertical slice in M0: configuration, MXL reader and writer, REST, metrics, NMOS, a web page, Dockerfile and CI, besides the engine spike. Parts of M1 (config, MXL I/O, NMOS, probes, minimal REST, `scope.sh`) and of M3 (a web page, config export) are therefore in M0; their remaining parts stay in M1 and M3.
2. **Reader and analyser are one thread** (SPECIFICATION §2.1 has an input reader and a renderer). The thread waits for the next grain with a timeout up to the next update tick, analyses the grain in place and hands the histogram to the writer, which renders (§2.4, updated); nothing waits for it, and the grain is never copied. Strip workers (M2) can split the analyse step later.
3. **Bin-major histogram** (§4.3 says column-major). The columns of one bin are contiguous: the renderer reads the counts line by line, and a strip touches two cache lines per bin instead of one per column. Counts are equal either way. Cell indices are 32-bit and absolute (§4.9 has 16-bit indices inside a strip); one or two sub-histograms, not four.
4. **Flow keys apply at the restart in M0.** `SCOPES_OUTPUT_FORMAT`, `SCOPES_OUTPUT_RATE` and `SCOPES_OUTPUT_KEY` are saved and reported as `restart_required` (§8 has them live with a new flow id). `SCOPES_OUTPUT_RATE=input` does follow the routed flow live: a route to another rate mints a new output flow id and the NMOS flow and sender follow.
5. **Values not built yet exit 78:** modes other than `luma` (M2), `SCOPES_ACCUMULATE=all` (M1), `SCOPES_OUTPUT_KEY=true` (M2), `SCOPES_THREADS` above 1 (M2). A PATCH with them answers 400.
6. **Graticule text** uses the 8×8 public-domain bitmap font scaled to 16 px at 1080 lines, not Blend2D with DejaVu Sans (§3.1). Blend2D comes with the label polish in M1.
7. **Preview.** No WebRTC preview yet (M3). The web page shows `GET /api/v1/snapshot.png` twice a second; the renderer copies the picture for it every 500 ms. `PREVIEW_*`, `MEDIAMTX_*` and `SCOPES_PREVIEW_*` are validated but unused; `/api/v1/info` reports the preview state `not_implemented`. The image has neither MediaMTX nor FFmpeg yet.
8. **Web page.** One static HTML page in the family look without Vue or a build step; the Vue 3 UI and the `scope` widget (`/widgets`, `/widget/scope`) are M3.
9. **Measurements** (§4.7), their metrics and `GET /api/v1/measurements` are M2 as planned; not in M0.
10. **PATCH of an environment key.** A `PATCH /api/v1/scope` of a key the environment sets applies live until the restart and is listed as `not_persisted` (§9.2 says the PATCH is saved to the file; the file cannot beat the environment, and the platform sets `SCOPES_MODE` and `SCOPES_UPDATE_RATE` in the environment).
11. **A running flow that disappears** (a writer that stops and deletes its flow) holds for `SCOPES_HOLD_MS` and then shows `NO SIGNAL`, like a flow that stops; `waiting` stays for a flow that has not run on this route (§11 lists only the missing flow).
12. **Plot geometry.** The plot starts and ends on 6-pixel v210 group boundaries and has a 2 % right margin (§5.2 gives only the left, top and bottom margins); a picture is the packed still picture plus the groups with a trace.

13. **`SCOPES_SIMD=auto` does not pick AVX-512** (confirmed by the maintainer 2026-10-15). The fallback order is AVX-512 → AVX2 → scalar; `auto` takes AVX2 because the AVX-512 step is slower on both CPUs measured (Skylake-SP, Zen 4). AVX-512 stays selectable with `SCOPES_SIMD=avx512`.

## Questions for the maintainer

Answered 2026-10-15: softer intensity curve (done, §4.4); AVX-512 added (done; slower than AVX2 on Skylake-SP and Zen 4, so `auto` keeps AVX2, deviation 13); copy-free writer first (done, numbers above); ports 8170 and 3322 confirmed.

Open: the analyse step inside the process on Skylake-SP keeps 25 Hz at 0.16 core for bars and above target for busy pictures. Next options, in M1 unless decided otherwise: keep the renderer core awake between ticks (a short spin before a tick), and 2 strip workers (M2 plan) for 2160p.

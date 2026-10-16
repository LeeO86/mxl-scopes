# mxl-scopes — Implementation plan

This plan comes before any code. It is updated as the work goes on: every decision and every deviation from `SPECIFICATION.md` is recorded here, as in the sibling repos.

## Status (2026-10-16)

M1 done on `feat/m1` (PR in review): the stage-1 modes `ycbcr-parade`, `rgb-parade`, `rgb-overlay` end to end (pulled forward from M2, deviation 15), `SCOPES_ACCUMULATE=all`, the trace computed on the analysing thread (deviation 16), and the lab run with a camera picture and the multiviewer tiles; numbers in `docs/performance.md`. What M1 leaves open is listed under M1 below.

M0 done (2026-10-15), PR #1 merged. The open questions of SPECIFICATION §16 are decided (§16 there). Process CPU on the lab class, 1080p50 in and out, AVX2: real picture (camera) about 0.25 core at 25 Hz and about 0.45 core every frame, bars 0.09–0.14 and 0.20–0.22 core; accepted by the maintainer as the §13 figures (§13 updated).

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

M1 adds `src/scope/cpu/multi.hpp` (the three-plane analyse kernels over a vector traits type) and `lanes.hpp` (their increment lists), `tests/unit/test_modes.cpp` and `docs/performance.md`.

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

### M1: luma waveform end to end (and the stage-1 modes)

- Config (env > file > defaults, exit 78), state directory, exit codes.
- MXL: domain scan, own output domain (G2), reader thread (no copy, torn-grain check), renderer thread with update-rate scheduler (`frame` and Hz, `latest` and `all`), writer thread (TAI paced, triple buffer).
- Graticule layer (mV, %, code; limited and full range) with Blend2D, drawn on settings change.
- NMOS node, receiver and sender (BCP-007-03), UUIDv5 ids, route persistence and restore, IS-04 subscription.
- `/livez`, `/readyz`, `/statusz`, `/metrics` (renderer, writer, input metrics).
- Minimal REST: `/api/v1/info`, `/api/v1/status`, `GET/PATCH /api/v1/scope`, config routes.
- Failure handling of SPECIFICATION §11 for the input (waiting, holding, no signal, format change, resync).
- `tests/integration/scope.sh` (luma, v210).

Exit: a routed 1080p50 bars flow gives a correct luma waveform flow; the multiviewer shows it as a tile; integration test green in CI.

Most of this list was built in M0 (deviation 1). Progress (2026-10-16), with the brief of 2026-10-16 (more modes, CPU per mode against §13, a lab run with a real picture and a multiviewer tile):

- [x] Modes `ycbcr-parade`, `rgb-parade`, `rgb-overlay` (M2 items pulled forward, deviation 15): analyse kinds `Luma`, `Ycbcr` (Y′, Cb, Cr planes) and `Rgb` (R′, G′, B′ planes, Q13 matrix of §4.8); scalar reference, AVX2 and AVX-512 steps written once over a vector traits type (`src/scope/cpu/multi.hpp`, `lanes.hpp`); byte-exact against scalar in the unit tests (random and edge frames incl. chroma at the extremes, widths 1–1280, every line and pixel step, column counts, bins, strips; whole pictures at 720p, 1080p, 2160p and 1998×50); R′G′B′ within one code of the double formula. Golden pictures (bars and a colour scene) for each mode, by hash and as PNGs in `docs/golden`.
- [x] Layout: three panels with a 4 % gap for the parades (chroma labels in the gaps, panel names under the panels), one panel for the overlay; Cb/Cr graticule (centre and ∓350 mV) in mV, % or code; per-plane intensity normalisation (a chroma plane has half the samples); the overlay adds pure R′, G′, B′ in R′G′B′, so a neutral trace is white.
- [x] Live mode change (`PATCH /api/v1/scope`, the web page): same output flow id; the NMOS flow description and the `scope-mode` tag of flow and sender follow (deviation 18); `updates_total`, `analyse_seconds`, `render_seconds` per `mode`; `/api/v1/output` lists the panels.
- [x] `SCOPES_ACCUMULATE=all` (§4.3): every grain (every field) between two updates goes into the counts as it arrives, checked for a torn read each; the intensity is normalised to round(input rate / update rate) frames; capped at what fits 16-bit cells (deviation 19).
- [x] The trace groups are computed on the analysing thread; the writer only writes the groups that differ into each grain (deviation 16). A slow update now skips ticks instead of delaying output grains.
- [x] `vpmaddwd` for the bin and cell multiplies (AVX2): luma analyse 15 % faster, bytes unchanged.
- [x] `mxl-scopes-bench --modes`; `scene` test pattern (camera luma with colour) for the writer and the benchmark.
- [x] `scope.sh`: `accumulate=all` at 10 Hz, each mode live on a grey input with the trace row checked in every panel, the flow id kept, the NMOS description followed, a stage-2 mode refused (400).
- [x] Lab run (2026-10-16): the test player 1.2.1 playing a camera clip as 1080p50 and as 1080i50; scopes on both (luma, YCbCr parade, RGB parade); a multiviewer 1.4.3 with the 1080p50 picture and the three scope outputs as tiles (screenshot kept outside the repo). Process CPU per mode in `docs/performance.md`.
- [ ] Desktop-class numbers (no desktop-class host in the lab).
- [ ] The 1080i50 picture as a multiviewer tile: the multiviewer 1.4.3 exits with SIGSEGV (139) when the test player's 1080i50 flow is routed to it (CPU and CUDA backends); the scopes read that flow fine. Reported to the maintainer.
- [ ] RGB parade every frame on camera content is above §13 (0.67–0.74 core against < 0.4; analyse + render p99 11.8 ms against < 8 ms) on the lab class; next steps under "M1 numbers".
- [ ] Blend2D graticule labels (deviation 6) and `ycbcr-overlay` (a SHOULD).

### M1 numbers

See `docs/performance.md` (engine per mode and SIMD level, process CPU per mode and update rate against §13, the A/B of the thread change). In short, lab class, camera picture, 1080p50, AVX2: luma 0.25–0.33 core at 25 Hz and 0.40–0.47 every frame (§13: about 0.25 and 0.45); YCbCr parade 0.34 and 0.44–0.51; RGB parade 0.39–0.46 and 0.67–0.74 (§13 every frame: < 0.4); RGB overlay 0.58–0.63 at 25 Hz, every frame not real time (ticks skipped). Bars: luma 0.08 / 0.18, RGB parade 0.20 / 0.40.

Next for the RGB modes: strip workers (`SCOPES_THREADS`, M2); pack the half-empty B vector of the RGB step (two loads per iteration, about 10 % of the analyse); for the overlay, fewer histogram columns or a vector render step.

### M2: parades, overlays, key, measurements

- ~~Modes `ycbcr-parade`, `rgb-parade`, `rgb-overlay`~~ (done in M1, deviation 15); `ycbcr-overlay` if cheap.
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
| G6 | Every listen port is a setting, NMOS WebSocket at `NMOS_PORT+1`. Bind failure exits 75. | met | `src/config/config.cpp` `checkPorts`; `src/main.cpp`; `src/nmos/node.cpp` (ready only after the listeners opened); CI "Exit codes" (web port and NMOS port taken) | MediaMTX ports count only in own mode (M3). |
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
5. **Values not built yet exit 78:** `SCOPES_MODE=ycbcr-overlay` (a SHOULD of stage 1), `SCOPES_OUTPUT_KEY=true` (M2), `SCOPES_THREADS` above 1 (M2). A PATCH with them answers 400. (M1 built the other stage-1 modes and `SCOPES_ACCUMULATE=all`.)
6. **Graticule text** uses the 8×8 public-domain bitmap font scaled to 16 px at 1080 lines, not Blend2D with DejaVu Sans (§3.1). Blend2D comes with the label polish in M1.
7. **Preview.** No WebRTC preview yet (M3). The web page shows `GET /api/v1/snapshot.png` twice a second; the renderer copies the picture for it every 500 ms. `PREVIEW_*`, `MEDIAMTX_*` and `SCOPES_PREVIEW_*` are validated but unused; `/api/v1/info` reports the preview state `not_implemented`. The image has neither MediaMTX nor FFmpeg yet.
8. **Web page.** One static HTML page in the family look without Vue or a build step; the Vue 3 UI and the `scope` widget (`/widgets`, `/widget/scope`) are M3.
9. **Measurements** (§4.7), their metrics and `GET /api/v1/measurements` are M2 as planned; not in M0.
10. **PATCH of an environment key.** A `PATCH /api/v1/scope` of a key the environment sets applies live until the restart and is listed as `not_persisted` (§9.2 says the PATCH is saved to the file; the file cannot beat the environment, and the platform sets `SCOPES_MODE` and `SCOPES_UPDATE_RATE` in the environment).
11. **A running flow that disappears** (a writer that stops and deletes its flow) holds for `SCOPES_HOLD_MS` and then shows `NO SIGNAL`, like a flow that stops; `waiting` stays for a flow that has not run on this route (§11 lists only the missing flow).
12. **Plot geometry.** The plot starts and ends on 6-pixel v210 group boundaries and has a 2 % right margin (§5.2 gives only the left, top and bottom margins); a picture is the packed still picture plus the groups with a trace.

13. **`SCOPES_SIMD=auto` does not pick AVX-512** (confirmed by the maintainer 2026-10-15). The fallback order is AVX-512 → AVX2 → scalar; `auto` takes AVX2 because the AVX-512 step is slower on both CPUs measured (Skylake-SP, Zen 4). AVX-512 stays selectable with `SCOPES_SIMD=avx512`.

14. **NMOS APIs only on `NMOS_PORT`.** §9.1 said they also answer on `WEB_PORT`; they do not (the spec text is corrected). The web page's NMOS tab shows state and ids; IS-05 activation from the page is M3.

15. **Stage-1 modes in M1** (the plan had them in M2): the brief of 2026-10-16 asked for more modes in M1. `ycbcr-parade`, `rgb-parade` and `rgb-overlay` are built; the key output, measurements, strip workers and `ycbcr-overlay` stay in M2.
16. **The analysing thread computes the trace** (§2.1 and §2.4 have the writer render the histogram into the grain). It now turns the counts into the packed trace groups right after the analyse step and hands them to the writer with the still picture (a shared, never changed buffer); the writer only writes the groups that differ into each grain, still without a render buffer or a full copy. Reason: the RGB overlay's render (10 ms in the process) made the writer late (412 late and 48 missed grains in 20 s every frame); now a slow update skips ticks (§4.6) and the output keeps its time. CPU is the same (A/B in `docs/performance.md`).
17. **Parade layout** (§5.2 says the label margin is "shared at three"): three equal panels with 4 % gaps; the Y′ or R′ labels sit in the left margin, the Cb and Cr labels in the gap left of their panel, the R′G′B′ panels 2 and 3 have none; each parade panel has its name (Y, Cb, Cr or R, G, B) under it when the bottom margin has room. `GET /api/v1/output` lists the panels.
18. **Mode change and the MXL flow definition.** §7.1 has the flow's `description` follow the mode. The NMOS flow's description and the `scope-mode` tag of flow and sender follow a live mode change (same flow id); the MXL `flow_def.json` keeps what it was created with (MXL has no update of a flow definition), until the next new flow id.
19. **`SCOPES_ACCUMULATE=all` details** (§4.3 says only "every input grain between two updates"): grains are analysed as they arrive (each field of an interlaced flow), each checked for a torn read; a torn grain drops the counts of that update. The intensity is normalised to round(input frame rate / update rate) frames, so an update with fewer or more grains than that (start, jitter) is a little dimmer or brighter. The frames per update are capped so that no 16-bit cell can overflow (luma 1080p: 30 frames); grains beyond the cap wait for the next update. `SCOPES_INPUT_OFFSET_GRAINS` does not apply; with `frame` (or a rate at or above the input's), `all` is `latest`.
20. **Chroma with pixel steps** (§4.2 "chroma samples follow their co-sited luma sample"): a Cb/Cr sample (x = 6g + 2k) is counted when its co-sited luma sample is; pixel step 3 or 6 therefore takes one chroma pair per group. R′G′B′ uses each taken luma sample with the pair at k = i / 2 (§4.8).
21. **RGB overlay colours** (§4.4): the three traces add as pure R′, G′, B′ primaries (so a neutral trace is white at full intensity); per pixel the Y′ of the three contributions is summed, the chroma of a pixel pair is the sum over both pixels. The parade panels use lighter tints (red, green, blue; Y′ white, Cb blue, Cr red) so a full-intensity trace stays readable on black. `SCOPES_TRACE_COLOR` applies to `luma` only.
22. **AVX-512 steps of the new modes use `vpmulld`** where AVX2 uses `vpmaddwd` (AVX-512F has no 16-bit multiply-add without AVX-512BW); same bytes, slower. AVX-512 stays selectable only (deviation 13).

## M0 review fixes (2026-10-16)

- Interlaced input (1080i50, the house format): MXL 218ddaa stores an interlaced grain as one field (height/2 lines) at twice the flow_def grain_rate. The reader takes rate and grain count from `mxlFlowInfo.config`, analyses height/2 lines per grain and both fields of a frame per update (twice the samples for the intensity), checks `grainSize` ≥ stride × lines before reading, and stamps grains at the field rate, so hold and NO SIGNAL work. `scope.sh` runs 1080i50 tff and bff writers.
- The reader blocks on the grain after the last one fetched (valid or invalid) and jumps to the head only when behind: an invalid-grain or slice-committing writer no longer makes it spin (`scope.sh` measures the CPU with both).
- HTTP: non-blocking sockets, one output queue per connection, nothing sent under the connection lock, a WebSocket client with more than 1 MiB queued is dropped, request heads above 64 KiB answer 431 (`test_http.cpp`: a client that stops reading, `/livez` still answers).
- NMOS: ready only after the listeners opened, so a taken NMOS port exits 75 (CI check).
- Metrics: counters print as integers.
- Torn check: each analysed grain is read again after the analyse step and must still be the same index.
- The helpers compiled into the SIMD units have internal linkage.
- `MXL_CLEANUP_ON_EXIT` removes only a domain this process created, or one an earlier mxl-scopes run made with this id (label `mxl-scopes`).
- `POST /api/v1/config/import` needs `Content-Type: application/json` (415 otherwise) and validates the routes (UUIDs) before writing `routes.json` in canonical form.
- Re-review (2026-10-16): a half-closed HTTP client is no longer polled for input while its reply drains; HTTP connections without progress close after 30 s (open WebSockets have no idle limit); on an interlaced flow an update is a whole frame (fields 2k, 2k + 1), so `frame` updates 25 times a second at 1080i50 and no field is analysed twice (unit test and `scope.sh`).

## Questions for the maintainer

Answered 2026-10-15: softer intensity curve (done, §4.4); AVX-512 added (done; slower than AVX2 on Skylake-SP and Zen 4, so `auto` keeps AVX2, deviation 13); copy-free writer first (done, numbers above); ports 8170 and 3322 confirmed.

Answered (accepted 2026-10-15): the M0 process figures became §13.

Open (2026-10-16):

1. RGB parade every frame on camera content is 0.67–0.74 core on the lab class (§13: < 0.4). Accept for now (the default 25 Hz is 0.39–0.46), or make strip workers (`SCOPES_THREADS`, two cores) the next step before M2's other items?
2. RGB overlay every frame is not real time at 1080p on the lab class (ticks skipped, output on time). Acceptable as a 25 Hz mode, or should its histogram use fewer columns (half the horizontal resolution, about half the render)?
3. Deviation 16 (the analysing thread computes the trace) changes §2.4 as decided on 2026-10-15: fold it into the specification?
4. The multiviewer 1.4.3 crashes (exit 139) when the test player's 1080i50 flow is routed to it. A ticket for the multiviewer?

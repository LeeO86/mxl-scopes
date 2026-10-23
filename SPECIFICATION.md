# mxl-scopes — Specification

Status: Draft v0.1 (design only; no code exists yet)
Repository: `LeeO86/mxl-scopes` (working name; not created yet)
Aligns with: the media function guideline G1–G14 and the later platform conventions (preview contract, widgets, release notes) as written out in `LeeO86/mxl-audio-mixer` SPECIFICATION Appendices A and B, and the sibling repos `LeeO86/mxl-multiviewer` (closest sibling), `mxl-webrtc-monitor` (preview contract, `channel` widget), `mxl-color-corrector` (run-time SIMD dispatch, bit-exact against a scalar reference), `mxl-browser-source` (one source per process), `mxl-audio-mixer` (spec structure).

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

Names that are guesses and need a check before implementation are marked `TODO_`.

---

## 1. Purpose and scope

`mxl-scopes` is a media function that measures one MXL video flow and draws a video scope (waveform monitor first) into an uncompressed MXL video flow. The multiviewer, a monitor, or any other MXL function can show that flow. Operators also see the scope in a browser through the platform's preview contract and an operator-screen widget.

Design principles:

1. **One input per instance.** One process measures one flow (§6). More flows mean more instances.
2. **The scope is a picture.** The output is an ordinary `video/v210` (or `video/v210a` with a key) flow. Downstream functions need no scope knowledge.
3. **Cheap enough on the CPU.** The v1 engine is CPU only, AVX2 with a scalar reference. It MUST fit well under one core for a 1080p50 input (§13). The engine has a backend interface so a CUDA path can be added later without changing the output.
4. **Bit-exact across code paths.** Every SIMD level (and a later CUDA backend) writes the same bytes as the scalar reference.
5. **Configurable update rate.** The scope can redraw on every input grain or at a lower rate (for example 10 or 25 Hz) to save CPU. The output flow keeps its full rate.
6. **Platform conventions.** NMOS IS-04/IS-05 with BCP-007-03, env > file > defaults, web UI and REST on `WEB_PORT`, `/livez`, `/readyz`, `/metrics`, widgets, the preview contract, exit codes 0/75/78/143, GHCR image with the MXL revision label.

### 1.1 Stages

| Stage | Release | Content |
| --- | --- | --- |
| 1 | 1.0 (this spec, normative) | Luma waveform. YCbCr parade, RGB parade, RGB overlay. Graticule in mV, % and 10-bit code. BT.709 limited range (full range as an option). Update rate and reduced sampling. v210 or v210a output. WebRTC preview and the `scope` widget. Out-of-range measurements as metrics. CPU (AVX2, scalar) |
| 2 | 1.1 (informative) | Vectorscope (Cb/Cr 2D histogram, 75 % and 100 % targets). The luma–chroma display (§5.5). Multi-pane layouts (for example waveform + vectorscope side by side). Optional trace persistence |
| 3 | 1.2 (informative) | HDR: PQ and HLG graticules (cd/m² and %), BT.2020 matrix, colorimetry from `flow_def.json` |
| 4 | 2.0 (informative) | CUDA backend (§3.2). Single-line select. False colour. More inputs per instance only if the one-input rule proves too costly (§6) |

Out of scope for every stage here: audio scopes (the audio mixer and the multiviewer meter audio), recording, gamut correction, compressed outputs, ST 2110 I/O, GStreamer in the media path, IS-07/IS-08/IS-12, authentication.

---

## 2. Architecture

```
 NMOS controller
        │ IS-05 PATCH (mxl_domain_id, mxl_flow_id) on the one receiver
┌───────▼────────────────────────────────────────────────────────────┐
│ mxl-scopes (C++20), one process, one NMOS node                      │
│                                                                     │
│  input reader thread                                                │
│    domain scan → mxlFlowReader → newest complete grain (no copy)    │
│        │ grain index + pointer, published at each update tick       │
│  scope renderer thread (+ optional strip workers)                   │
│    analyse:  v210 → samples → bin → per-column histogram (SIMD)     │
│    trace:    histogram → intensity → trace colour → packed v210     │
│              groups (+ key); still picture: background, graticule   │
│        │ "latest picture" (3 slots, atomic index)                   │
│  output writer thread (TAI paced, output rate)                      │
│    mxlFlowWriter: still + trace groups that differ into each grain  │
│        │                                                            │
│  preview thread (optional)                                          │
│    latest scope frame → NV12 → H.264 (NVENC, else x264) → RTSP      │
│    → MediaMTX (shared, or own supervised child)                     │
│                                                                     │
│  nmos-cpp node · web UI, REST, WebSocket, widgets · /metrics        │
└─────────────────────────────────────────────────────────────────────┘
        │ one video/v210 (or v210a) flow
        ▼
 mxl-multiviewer tile · mxl-webrtc-monitor · any MXL reader
```

### 2.1 Threads

| Thread | Job | Never waits for |
| --- | --- | --- |
| Input reader | Follows the routed flow. Holds the index of the newest complete grain. Handles `waiting`, `no_signal`, resync, flow re-creation, format change (as mxl-multiviewer §5.3) | renderer, writer |
| Scope renderer | At each update tick (§4.6): analyse the newest grain, turn the counts into the packed trace groups, hand them with the still picture to the writer (§2.3) | writer, preview |
| Strip workers | `SCOPES_THREADS` − 1 helpers for the renderer; each owns a set of column strips (§4.3) | — |
| Output writer | One grain per output index from TAI. Writes the newest trace groups straight into the grain, else repeats the picture (§2.4) | renderer |
| Preview | At `SCOPES_PREVIEW_FPS`: scale, encode, publish | renderer, writer |
| MediaMTX supervisor | Own mode only (preview contract) | — |
| HTTP, WebSocket, NMOS | nmos-cpp and the web server threads | media threads |

### 2.2 Input reader

- The reader MUST NOT copy grains. The renderer analyses the grain in the MXL ring in place. MXL keeps a grain for `history_duration` (200 ms on the platform); analysis takes a few ms.
- After the analysis the renderer MUST check that the grain was not overwritten during the read (the writer's head minus the grain index is still inside the ring). If it was, the result is dropped and `mxl_scopes_updates_torn_total` increments. This SHOULD never happen at 200 ms history.
- The reader picks the newest grain whose origin is not newer than `now − SCOPES_INPUT_OFFSET_GRAINS × input frame duration` (default 0: the newest complete grain).
- Slices: the reader waits for full grains (`validSlices = totalSlices`). Slice-progressive analysis is not needed: the scope adds latency of one update interval anyway.
- Invalid or gap grains are skipped; the previous scope frame stays.

### 2.3 Scope renderer

- The renderer owns the engine (§3.2). One call per update: `analyse(grain) → histogram`, then `trace(histogram) → trace groups`: the packed 6-pixel v210 groups that carry a trace, with the intensity, colours and graticule of the settings applied.
- Display settings are an immutable struct. REST and the UI build a new one and swap it atomically. The renderer takes it at the start of an update, so one scope frame never mixes two settings. A change of the settings rebuilds the lookup tables and the graticule layer on the renderer thread before the next update.
- The renderer hands each picture to the writer through three slots (triple buffering: one the writer reads, one being filled, one spare; no locks on the media path). A picture is the trace groups plus the still picture (background, graticule, texts), a shared buffer that never changes once built; a settings change builds a new one.
- Because the renderer computes the trace, a slow update makes it skip ticks (§4.6); it never delays an output grain.

### 2.4 Output writer

- TAI paced like the multiviewer composer (§5.4 there): for each output index `N`, sleep until `indexToTimestamp(rate, N)`, open the grain, write the latest scope picture into it (v210, and the key plane for v210a), commit with `validSlices = totalSlices`.
- No render buffer and no full copy per grain (decided 2026-10-15): the writer writes the newest trace groups from the renderer straight into the output grain. A picture is the still picture (background, graticule, texts) plus the packed 6-pixel groups that carry a trace. Each ring slot remembers which still picture and which trace groups it holds; a grain gets only the groups that differ (old trace groups back to the still bytes, new ones written). A large trace is one copy of the previous grain; a slot that already holds the picture (a still input, `NO SIGNAL`) is not written at all.
- Skipped indexes count as missed; a commit after the next index's time counts as late.
- Before the first scope frame exists, the writer writes the empty graticule (with `NO SIGNAL` while the input is not running).
- The bytes of a grain are those of the full render (the unit tests compare them).

### 2.5 Preview

- The preview follows the platform preview contract exactly as mxl-multiviewer §8.4 and mxl-webrtc-monitor §5.7: `PREVIEW_PUBLISH_URL` set → shared mode, publish there; empty → own mode, a supervised MediaMTX child on the `MEDIAMTX_*` ports. Its config lets only 127.0.0.1 and ::1 publish (anyone may read and play back) and keeps the running publisher (`overridePublisher: false`).
- One stream, path `<PREVIEW_PATH_PREFIX>/scope`.
- The preview thread takes the latest scope frame at `SCOPES_PREVIEW_FPS` (default 25, never more than the update rate), scales it by area average to `SCOPES_PREVIEW_HEIGHT` (default 540, even width keeping the aspect) in 8-bit NV12, and encodes H.264 (NVENC when available, else x264; 2 Mbit/s CBR, one-second GOP, no B-frames, BT.709 limited range).
- The key plane is not in the preview. The preview shows the scope on its background.
- The preview is not part of readiness.
- The picture comes from the output writer, which, after committing the grain, copies the newest output picture (the fill) at the preview rate when it changed, into one of three buffers allocated once; a buffer still held by the encoder or the PNG is skipped, and the old picture is freed outside the state lock. The scale is an area average of integer boxes, 10 to 8 bits rounded, chroma per 2×2 output pixels. The encoder runs on its own thread at `min(SCOPES_PREVIEW_FPS, update rate)`, timestamps from the clock; h264_nvenc is tried first and libx264 used when it does not open (logged once). An output raster change opens a new encoder and connection. The publish state is in `/api/v1/status` (`preview`: `state` connecting, publishing or error, `encoder`, `frames`, `width`, `height`, `fps`, `whep`, `hls`, own mode `mediamtx`).

---

## 3. Technology and build

### 3.1 Toolchain and pins

- C++20, CMake ≥ 3.24, Ninja, GCC ≥ 12 or Clang ≥ 16.
- MXL: the platform pin (`dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`), `-DMXL_ENABLE_FABRICS_OFI=OFF`, one pin variable in the Dockerfile and CI. Public C API only.
- nmos-cpp: the siblings' commit (`fe303849527394b03bdedc8f161f377fe458bb62`).
- Graticule text: Blend2D with DejaVu Sans compiled in, the same versions as mxl-multiviewer. The graticule is drawn only on a settings change, never per frame.
- Preview: system FFmpeg (`libavcodec`, `libavformat`) and the pinned MediaMTX binary, as mxl-multiviewer 1.4.
- Web UI: Vue 3, one embedded HTML file, the family look of the siblings, no CDN.
- Tests: doctest (vendored); shell integration tests.
- Base image Ubuntu 24.04. The image MUST start on any x86-64 CPU without a GPU.
- Repository layout as mxl-multiviewer: `.github/workflows`, `cmake`, `deploy`, `docker`, `src`, `tests`, `third_party`, `web`, `AGENTS.md`, `CHANGELOG.md`, `IMPLEMENTATION_PLAN.md`, `README.md`, `SPECIFICATION.md`, `LICENSE`.

### 3.2 Engine and backends

The engine is behind one interface so that a CUDA backend can come later:

```
struct ScopeEngine {
  void configure(const InputFormat&, const DisplaySettings&);   // LUTs, layout, graticule
  void analyse(const uint8_t* v210, size_t stride, Histograms&); // integer counts only
  void render(const Histograms&, ScopeFrame&);                   // v210 + optional key
};
```

- Histograms are integer counts. Integer sums do not depend on order, so a CUDA backend with shared-memory atomics gives the same counts as the CPU. The render step uses the same integer lookup tables. The output MUST be byte-identical on every backend.
- `SCOPES_BACKEND`: `auto` and `cpu` in v1. `cuda` is reserved: in v1 it exits 78 with `cuda_backend_not_built`.
- CPU code level `SCOPES_SIMD`: `auto`, `avx512`, `avx2`, `scalar` in v1. Chosen at start from cpuid and XGETBV, as mxl-color-corrector. A level the CPU does not have falls back to the next one below (AVX-512 → AVX2 → scalar), with a warning. Any other value exits 78. `auto` picks `avx2` whenever the CPU has it (decided by the maintainer 2026-10-15): the AVX-512 step measured slower than AVX2 on Skylake-SP and on Zen 4 (IMPLEMENTATION_PLAN.md "M0 rework"). AVX-512 stays in the build, tested, and is used only with `SCOPES_SIMD=avx512`. AVX2 stays first-class: the platform's small nodes have no AVX-512.
- AVX-512 is in v1 (open question 8, decided by the maintainer 2026-10-15): the AVX2 analyse step on 512-bit registers, four v210 groups per load, AVX-512F only (the lab Xeon has no VBMI2). The hot loop stays the scalar histogram increment (§4.3), so the gain is small; on Skylake-SP 512-bit code also slows the other hyperthread of the core (the color corrector measured 3.4–4 %), hence the `auto` rule.
- The AVX2 code MUST NOT use gather instructions (slow with the Downfall microcode on the platform's CPUs; see mxl-webrtc-monitor).
- The choice is logged at start, shown in `/api/v1/info` (`simd: {level, requested, cpu}`) and exported as `mxl_scopes_simd_level{level}`.

---

## 4. The waveform algorithm

### 4.1 Input samples

- Input is `video/v210` (or `video/v210a`; the key plane is ignored), 4:2:2, 10 bits, any raster up to 3840×2160 that MXL allows. Row stride is `ceil(width/48) × 128` bytes. One v210 group of 16 bytes carries 6 Y, 3 Cb and 3 Cr samples.
- Interlaced grains are analysed as the whole frame (both fields). In MXL an interlaced grain is one field (height / 2 lines) at twice the flow_def `grain_rate`: an update analyses the newest field and the one before it, and grains are timed at the field rate (`mxlFlowInfo.config`). Field select is a later stage. Interlaced video and its ANC follow the shared MXL interlaced convention: https://github.com/LeeO86/mxl-multiviewer/blob/main/docs/mxl-interlaced.md
- Codes 0–3 and 1020–1023 are measured like any other code. They are counted as out of range (§4.7).
- v1 assumes BT.709. The flow's colorimetry in `flow_def.json` is read; anything other than BT.709 shows a warning in the UI and in `/api/v1/input` (`colorimetry_warning`). It is not an error.
- `SCOPES_RANGE=limited` (default) uses BT.709 narrow range: Y 64–940, Cb/Cr 64–960 centred on 512. `full` uses 0–1023 for every component (§4.5).

### 4.2 Sampling and reduced sampling

- `SCOPES_LINE_STEP` (1–8, default 1) analyses every n-th line, starting at line 0.
- `SCOPES_PIXEL_STEP` (1, 2, 3 or 6; default 1) analyses every n-th luma sample of each v210 group. The steps divide 6 so a group always gives the same sample positions. Chroma samples follow their co-sited luma sample.
- Intensity normalisation (§4.4) divides by the number of samples actually taken, so the trace keeps its brightness when the steps change.
- The steps cut the analyse cost about in proportion (§4.8). They do not change the plot geometry.
- Recommended presets in the UI: **full** (1/1), **half** (line step 2), **quarter** (line step 2, pixel step 2).

### 4.3 Accumulation: one histogram per column

The plot area has `C` columns and `B` bins (rows). Defaults: the plot's width and height in output pixels (§5.1).

- Column of an input sample at `x`: `col = floor(x × C / width)`, from a per-x lookup table built at configure time. In a parade each component has its own panel and its own `C`.
- Bin of a code value `v` (0–1023): `bin = floor((v − v_lo) × B / (v_hi − v_lo + 1))`, clamped to `[0, B−1]`, from a 1024-entry lookup table. `v_lo`/`v_hi` are the visible code range (default 0–1023, so super-black and super-white stay visible).
- `hist[component][col][bin] += 1`. Layout: column-major, the `B` bins of one column contiguous.
- Counter width: `uint16_t` when the largest possible count per cell (`ceil(width / C) / pixel_step × ceil(height / line_step) × frames accumulated`) fits in 65535, else `uint32_t`. The choice is made at configure time and is the same on every backend; the picture does not depend on it.
- **Cache blocking.** A 1080p luma histogram of 1840 × 1000 cells is about 3.7 MB, larger than L2. The engine therefore walks the picture in **column strips**: a strip of input columns that maps to at most 64 plot columns (about 128 KB of counters at `B` = 1000), for all analysed lines, then the next strip. The input of one strip is 64 × ⌈width/C⌉ samples per line, so it streams with hardware prefetch. With `SCOPES_THREADS` > 1, each worker takes a range of whole strips of about equal size. Where a range ends inside a strip, that strip is split at the next input x where a new histogram column starts, so no column spans two ranges and the workers never share a counter; the result is the same for any thread count. The trace step splits the plot rows the same way, at bin boundaries.
- **Store-forwarding hazard.** Flat picture areas send neighbouring samples of one column to the same bin, which chains load-add-store at about 5 cycles each. When `width / C ≥ 2`, the engine keeps two sub-histograms per strip (even and odd input `x`) and adds them when the strip is done. The sum is exact, so the result does not depend on the trick.
- **Accumulation over time.** `SCOPES_ACCUMULATE=latest` (default) analyses only the newest grain at each update. `all` analyses every input grain between two updates into the same histogram (better statistics on noisy pictures, full analyse cost at any update rate).

### 4.4 Intensity mapping

- Each cell's count `c` maps to a trace intensity `I` (0–1023) through a lookup table `I = LUT[min(c, c_max)]`.
- The table is built once per settings change in double precision and rounded to integers. Every backend uses the same table, so the mapping is exact.
- Default curve: `I = 1023 × (1 − exp(−g × c / c_ref))`, where `c_ref` = samples taken per column (the count of a cell that holds a whole flat column) and `g` = `SCOPES_INTENSITY` / 100 × 20. Brightness grows with the hits per cell, as on broadcast waveform monitors: a cell with 1/20 of its column's samples is at 63 %, one with a few hits is dim (about 1–2 % at 1080 lines, so `SCOPES_TRACE_MIN` keeps it visible), a flat field saturates. `SCOPES_INTENSITY` is 1–400 %, default 100. (Changed 2026-10-15 by the maintainer: the first draft used `c_ref` / `B` and gain 4, which lit almost every cell at full brightness.)
- `c = 0` always maps to the background. Any `c ≥ 1` maps to at least `SCOPES_TRACE_MIN` (default 96 of 1023), so a single sample stays visible.
- Trace colour: `I` scales a colour in Y′CbCr. Luma waveform: `SCOPES_TRACE_COLOR` (`green` default, `white`, `amber`). Parades and overlay: each component in its own colour (Y white, Cb blue, Cr red; R red, G green, B blue). In the RGB overlay, colours of the three components in one cell add in R′G′B′ and are clamped, so a neutral grey trace is white.
- Persistence (decay over updates) is a stage 2 option.

### 4.5 Graticule

The graticule is drawn once per settings change into a layer of 10-bit Y′CbCr with alpha, then blended over the trace on each render. Only its non-transparent spans are stored, so the blend touches a few percent of the pixels.

Luma and R′G′B′ panels, BT.709 limited range (10 bits):

| Code | mV | % (IRE) | Line |
| --- | --- | --- | --- |
| 1019 | 763.1 | 109.0 | dotted, "max" |
| 940 | 700 | 100 | solid, legal white |
| 64 + 87.6 × k, k = 1…9 | 70 × k | 10 × k | thin, labelled at 0, 50, 100 % or 0, 350, 700 mV and at every 100 mV |
| 64 | 0 | 0 | solid, legal black |
| 4 | −48.0 | −6.8 | dotted, "min" |

`mV = (code − 64) × 700 / 876`. `% = (code − 64) / 876 × 100`. In BT.709 there is no set-up, so % equals IRE units; the UI says "%" and offers "IRE" as a label variant.

Cb and Cr panels: centre line at 512 (0 mV), solid lines at 64 and 960 (∓350 mV), labels in mV or code. `mV = (code − 512) × 700 / 896`.

`SCOPES_RANGE=full`: luma lines at 0 and 1023 as 0 % and 100 %; chroma 0–1023 around 512. Labels in % and code; mV is shown as mapped (0–700 mV).

`SCOPES_GRATICULE_UNITS`: `mv` (default), `percent`, `code`. `SCOPES_GRATICULE_LEVEL`: brightness of the graticule, 0–100 %, default 60. 0 hides it. Labels sit in a margin to the left of each panel; their size scales with the output height (16 px at 1080).

### 4.6 Update rate

- `SCOPES_UPDATE_RATE`: `frame` (every input grain) or a rate in Hz, 1–60 (`10`, `25`, `12.5`, …). Default `25` (open question 1).
- Ticks run on TAI: tick `k` is at `k / rate`. At each tick the renderer analyses the newest grain (or, with `ACCUMULATE=all`, every grain since the last tick). A grain is analysed at most once.
- With `frame`, the renderer waits for each new grain. A rate at or above the input rate behaves like `frame`.
- If an update is still running when the next tick comes, that tick is skipped and counted (`mxl_scopes_updates_skipped_total`). The writer keeps writing the last frame.
- `rgb-overlay` and `ycbcr-overlay` update at most every n-th input frame, n the smallest that keeps it at or below 30 Hz: 25 Hz from 50p, 29.97 Hz from 59.94p, every frame from 25p, 29.97p and 1080i50 (decided 2026-10-22). `frame` and higher rates run at that rate; lower rates stay. The overlay draws three planes over the full plot width; on the lab class one update takes 18–20 ms at 1080p50, close to the 20 ms frame time, so every frame takes most of a core and skips ticks. The cap keeps the full horizontal resolution and an even cadence at about half a core. `/api/v1/status` (`renderer.update_rate_hz`) and the caption show the rate in use.
- The output flow always runs at its own full rate (§7). Between updates it repeats the last scope frame.

### 4.7 Measurements

From the same histograms, at no extra pass over the picture, each update computes (decided 2026-10-22):

- luma min, max and mean code; share of samples below 64 and above 940 — in `luma` and the YCbCr modes (plane Y′);
- Cb/Cr share outside 64–960 — in the YCbCr modes;
- in the RGB modes: share of pixels with any R′G′B′ component outside the EBU R 103 tolerance (−5 % to 105 %, codes 20–984 rounded), from the unclamped values of §4.8.

A value the mode has no plane for is null (and its metric is not exported). The luma and chroma values come from the bin totals of each plane, summed by the trace step while it reads the counts: a bin counts as its centre code, so they are exact to one bin, `codes_per_bin` = 1024 / bins (about 1 code at a 1080-line output, about 6 at 180 lines). The R′G′B′ share is counted exactly per pixel in the RGB analyse step (a compare per component, on every SIMD path). No per-code histogram is kept. The thresholds are the BT.709 limits whatever `SCOPES_RANGE` is. While a state text shows (no input), every value is null.

They are exported as metrics, at `GET /api/v1/measurements` and on the WebSocket (§9, §10). They raise no alarms in v1 (open question 12).

### 4.8 R′G′B′ from Y′CbCr

- Each luma sample uses the co-sited chroma pair (v210 chroma sits with the first of two luma samples); no chroma interpolation, the same choice as mxl-color-corrector.
- BT.709 limited range, R′G′B′ in the same 10-bit code scale as Y (64 = 0 %, 940 = 100 %):

| | Y | Cb − 512 | Cr − 512 |
| --- | --- | --- | --- |
| R′ | 1 | 0 | 1.539648 |
| G′ | 1 | −0.183143 | −0.457675 |
| B′ | 1 | 1.814180 | 0 |

- Integer maths: coefficients in Q13 (×8192, rounded: 12613, 1500, 3749, 14862), `(Y << 13) + Σ coef × (C − 512) + 4096 >> 13`, then clamped to 0–1023 for binning. The unclamped value decides the gamut measurement. Scalar and AVX2 use the same formula, so they are bit-exact. The unit tests also check it against the double formula (≤ 1 LSB).

### 4.9 Cost estimate (1080p50 input, 1920×1080 output)

Estimates for an 8-core AVX2 desktop CPU (Coffee Lake class, ~4.5 GHz), one thread. Milestone M0 measures them; the numbers are a design budget, not a result.

| Step | Work per update | Estimate |
| --- | --- | --- |
| Unpack v210 + bin index (AVX2) | 2.07 M luma samples, ~0.25 cycle each | 0.1 ms |
| Histogram increments (scalar) | 2.07 M increments, ~1.5–2 cycles each (L2 resident strips) | 0.7–0.9 ms |
| Render: intensity LUT, colour, zero fast path | ~1.8 M plot cells, most empty | 0.3–0.5 ms |
| Graticule blend (spans) | a few % of pixels | < 0.1 ms |
| Clear histograms (fused with render) | 3.7 MB | 0.1 ms |
| v210 pack (AVX2) | 5.5 MB | 0.2–0.3 ms |
| **Luma waveform update** | | **~1.5–2 ms** |
| YCbCr parade update | 4.15 M increments | ~2.5–3 ms |
| RGB parade or overlay update | 6.2 M increments + matrix | ~3.5–4.5 ms |
| Writer copy, per output grain | 5.5 MB | 0.3–0.5 ms |

CPU per instance (luma, 1080p50 output): every frame ≈ 50 × (2 + 0.4) ms ≈ 0.12 core; 25 Hz ≈ 0.07 core; 10 Hz ≈ 0.04 core. RGB parade every frame ≈ 0.25 core. The scalar reference is expected at 2–3× the AVX2 time (the increments do not vectorise; the unpack, binning, matrix and pack do). A 2160p50 input has 4× the samples: luma every frame ≈ 6–8 ms per update, or half that with line step 2. Preview encode is extra: x264 at 960×540, 25 fps, about 0.1–0.2 core; NVENC near zero.

How it maps to AVX2:

- **Unpack:** one 256-bit load = 2 v210 groups = 12 luma samples. `vpshufb` + `vpsrlvd` + `vpand` extract the 10-bit fields into 16-bit lanes; Y, Cb and Cr go to separate registers.
- **Bin:** `bin = (v × k) >> 16` with `vpmulhuw` (k from `B` and the range), clamp with `vpminuw`/`vpmaxuw`. Column offsets come from the per-group table added with `vpaddw`. The 16-bit cell index (`col × B + bin` within the strip) is stored to a small stack buffer.
- **Increment:** a scalar loop over that buffer, `++hist[idx]`, two sub-histograms (§4.3). No gather, no scatter, no `vpconflictd`.
- **R′G′B′:** `vpmaddwd` on interleaved (Y, C) pairs with Q13 coefficients, `vpsrad`, `vpackusdw`.
- **Render:** compare 16 counters to zero (`vpcmpeqw`, `vpmovmskb`); an all-zero block writes the background directly. Non-zero cells use the scalar LUT, then SIMD colour multiply and blend.
- **Pack:** the reverse of the unpack, as in the siblings' v210 code.

---

## 5. Display modes

### 5.1 Modes

`SCOPES_MODE` sets the display. Names are neutral, descriptive, and not vendor names.

| Mode | Stage | Shows | Panels |
| --- | --- | --- | --- |
| `luma` | 1 (first) | Y′ waveform | one, full width |
| `ycbcr-parade` | 1 | Y′, Cb, Cr waveforms side by side | three, each 1/3 width |
| `rgb-parade` | 1 | R′, G′, B′ waveforms side by side | three |
| `rgb-overlay` | 1 | R′, G′, B′ waveforms in one panel, each in its colour | one |
| `ycbcr-overlay` | 1 (SHOULD) | Y′, Cb, Cr in one panel: the raw codes on the shared code axis (neutral chroma on the 50 % line), Y′ white, Cb blue, Cr red | one |
| `vectorscope` | 2 | Cb/Cr 2D histogram, 75 % and 100 % colour targets, skin-tone line | one, square |
| `luma-chroma` | 2 | Y′ against Cb (upper half) and Y′ against Cr (lower half), the combined display used to check colour bars, timing and gamut | one, two halves |

The combined Y-against-chroma display is called **`luma-chroma`** in the API and the UI ("Luma/Chroma" as the title). Vendor trade names MUST NOT be used for it, nor for any other mode.

### 5.2 Layout of the output raster

- The scope fills the whole output raster. Positioning on a wall is the multiviewer's job (a tile rect, with the key for an overlay).
- Panels have a left margin for the graticule labels (8 % of the width at one panel, shared at three) and 4 % margins top and bottom. The plot sizes `C` and `B` follow from that; `B` is at most 1024.
- A thin header line (SHOULD, `SCOPES_CAPTION=true`) shows the mode, the input label (IS-04 sender label of the routed flow, else the MXL flow label), the input format and the update rate.
- While the input is not `running`, the panels show the graticule and the state (`NO SIGNAL`, `WAITING`, `NOT ROUTED`, `FORMAT`) in the centre.
- Multi-pane layouts (for example waveform left, vectorscope right) are stage 2: `SCOPES_LAYOUT=single` is the only v1 value.

---

## 6. Inputs

**One video input per instance** (one IS-05 receiver). Several flows mean several instances. Rationale:

1. **Same rule as mxl-browser-source** (one page per process): a crash, a stuck reader or a bad input takes down one scope, not a set of them.
2. **Kubernetes sizing works per instance.** The platform sets CPU requests and limits per function; one scope = one predictable budget (§13). A many-input process would need its own internal scheduling and fair share.
3. **A simple NMOS model.** One receiver, one sender, one node. A controller routes a scope like any other function. The multiviewer already combines several scope outputs on one wall, so a multi-input scope adds nothing there.
4. **The update rate is per instance.** A programme scope at every frame and a quiet camera scope at 10 Hz are two settings, not a per-input table.
5. **Low fixed cost.** The reader does not copy grains. The per-process overhead (nmos-cpp node, web server, about 50–100 MB RAM; measured 2026-10-23: 63–116 MB RSS on the platform) is small next to a 53 MB output flow on the tmpfs.

The cost is one NMOS node and one web endpoint per scope. If productions need many scopes and the per-instance overhead turns out to matter, a later stage can add `SCOPES_INPUTS` (inputs × modes into a mosaic). The engine interface (§3.2) does not prevent it.

- Receiver capabilities (BCP-004-01): `video/v210` and `video/v210a`, progressive and interlaced, width 1–3840, height 1–2160, the grain rates of the multiviewer, YCbCr-4:2:2, 10 bits.
- No audio receiver.
- IS-05 behaviour as mxl-multiviewer §4.2: an activation with UUIDs is accepted even if the flow is not there yet (`waiting`, backoff 250 ms → 5 s); `master_enable: false` stops the reader (`not_routed`); the active route is saved in `<SCOPES_STATE_DIR>/routes.json` and restored at start, into the reader and into the receiver's IS-05 active state and IS-04 subscription.
- A format change in the input's `flow_def.json` reconfigures the engine (new lookup tables and strips) at the next update. The output flow does not change.

---

## 7. Output

- One video flow in this process's own output domain, as the siblings (§11 G2).
- Format: `SCOPES_OUTPUT_FORMAT`, default `1920x1080`; width and height even, at most 3840×2160, progressive only. The output rate is `SCOPES_OUTPUT_RATE`: `input` (default: the routed input's rate, `50/1` before the first route) or a fixed rate token (`25`, `50`, `5994`, `N/D`). A change of the output format or rate mints a new flow id (flow token, as the multiviewer); the sender follows. With `input`, a re-route to a flow of another rate changes the output flow too (open question 3).
- `SCOPES_OUTPUT_KEY=false` (default): `video/v210`, the background is limited-range black.
- `SCOPES_OUTPUT_KEY=true`: `video/v210a`. The key plane is straight alpha, 0 transparent, 1023 opaque, the MXL layout used by mxl-multiviewer (§5.5 there): after the v210 fill, three 10-bit samples per little-endian 32-bit word, ceil(width / 3) words per line. Key values:
  - background: `SCOPES_BG_ALPHA` (0–1023, default 0, fully transparent);
  - trace: `max(SCOPES_BG_ALPHA, I)`, so a faint trace is faint over the picture (the overlays: the largest `I` of the three planes);
  - graticule: its own alpha × `SCOPES_GRATICULE_LEVEL` (the larger of it and what is under it); the labels and units at the level;
  - caption, panel names and state texts: opaque.
  The fill under transparent areas stays limited-range black, so a downstream that ignores the key still shows a correct scope.
- The multiviewer uses a v210a tile's key as straight alpha. A scope tile with a higher z over the picture tile of the same input gives an overlay scope with no change to the multiviewer.
- History: `MXL_HISTORY_DURATION_NS` (default 200 ms) for a new domain only.

### 7.1 NMOS resources and labels

| Resource | Label | Group hint |
| --- | --- | --- |
| Node | `NMOS_LABEL`, else `HOST_ID` | — |
| Device | `NMOS_LABEL` + ` scopes`, else `MXL Scopes` | — |
| Video receiver | `Scopes In Video` | `Scopes In:Video` |
| Video source, flow, sender | `Scopes Out Video` | `Scopes Out:Video` |

- `NMOS_TAGS` on the node and the device. The flow carries `description` = the mode title (for example `Luma waveform`), updated when the mode changes (same flow id).
- The output flow and sender carry the tag `urn:x-mxl:tag:scope-mode/v1.0` = `[<mode>]` (`TODO_` confirm the tag namespace with the platform).
- UUIDv5 ids from `NMOS_SEED` (RFC 4122 URL namespace) with the names `mxl-scopes/<seed>/node`, `/device`, `/domain`, `/in/video`, `/out/video/source`, `/out/video/sender`, `/out/video/flow/<format-token>`; the token is `<width>x<height>p<rate>`, with `a` appended for a v210a output, so format, rate and key each give their own flow id.
- Senders report `mxl_domain_id` and the current `mxl_flow_id`; `master_enable` is true while the writer writes.
- No audio flows. No output receivers.

---

## 8. Configuration

Precedence: environment > `SCOPES_CONFIG_FILE` (flat JSON, keys are the variable names, string values) > defaults. Invalid configuration exits 78. The file is written atomically. "Live" keys apply at the next update without a restart; "flow" keys apply live and mint a new output flow id; "restart" keys are saved and reported as `restart_required`.

### 8.1 Function settings

| Key | Default | Apply | Meaning |
| --- | --- | --- | --- |
| `SCOPES_MODE` | `luma` | live | §5.1 (stage 2 values exit 78 in v1) |
| `SCOPES_UPDATE_RATE` | `25` | live | `frame` or 1–60 Hz (§4.6) |
| `SCOPES_ACCUMULATE` | `latest` | live | `latest` or `all` (§4.3) |
| `SCOPES_LINE_STEP` | 1 | live | 1–8 (§4.2) |
| `SCOPES_PIXEL_STEP` | 1 | live | 1, 2, 3, 6 (§4.2) |
| `SCOPES_RANGE` | `limited` | live | `limited` (BT.709 narrow) or `full` |
| `SCOPES_GRATICULE_UNITS` | `mv` | live | `mv`, `percent`, `code` |
| `SCOPES_GRATICULE_LEVEL` | 60 | live | 0–100 % |
| `SCOPES_INTENSITY` | 100 | live | 1–400 % (§4.4) |
| `SCOPES_TRACE_MIN` | 96 | live | 0–1023, the dimmest visible trace |
| `SCOPES_TRACE_COLOR` | `green` | live | `green`, `white`, `amber` (luma mode) |
| `SCOPES_CAPTION` | `true` | live | header line (§5.2) |
| `SCOPES_LAYOUT` | `single` | live | `single` only in v1 |
| `SCOPES_OUTPUT_FORMAT` | `1920x1080` | flow | output raster |
| `SCOPES_OUTPUT_RATE` | `input` | flow | `input` or a rate token |
| `SCOPES_OUTPUT_KEY` | `false` | flow | `true` writes `video/v210a` (§7) |
| `SCOPES_BG_ALPHA` | 0 | live | key of the background, 0–1023 |
| `SCOPES_INPUT_OFFSET_GRAINS` | 0 | live | 0–30 (§2.2) |
| `SCOPES_HOLD_MS` | 1000 | live | `no_signal` after this long without a new grain |
| `SCOPES_THREADS` | 1 | restart | renderer threads, 1–8 (§4.3) |
| `SCOPES_BACKEND` | `auto` | restart | `auto`, `cpu`; `cuda` reserved (§3.2) |
| `SCOPES_SIMD` | `auto` | restart | `auto`, `avx512`, `avx2`, `scalar` |
| `SCOPES_PREVIEW_MODE` | `webrtc` | restart | `webrtc` or `off` |
| `SCOPES_PREVIEW_FPS` | 25 | live | 1–30, capped at the update rate |
| `SCOPES_PREVIEW_HEIGHT` | 540 | restart | 270–1080, even |
| `SCOPES_STATE_DIR` | `/config` | restart | `config.json`, `routes.json`, `mediamtx.yml` |
| `SCOPES_CONFIG_FILE` | empty | restart | flat JSON; empty uses `<SCOPES_STATE_DIR>/config.json` |

### 8.2 Platform settings (same names and meaning as the siblings)

| Key | Default | Apply |
| --- | --- | --- |
| `HOST_ID` | hostname | restart |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | restart |
| `MXL_OUTPUT_DOMAIN_DIR` | `/Volumes/mxl/scopes-<seed-short>` | restart |
| `MXL_OUTPUT_DOMAIN_ID` | UUIDv5 of the seed | restart |
| `MXL_HISTORY_DURATION_NS` | 200000000 | restart (new domain only) |
| `MXL_CLEANUP_ON_EXIT` | `false` | restart |
| `NMOS_ENABLE` | `true` | restart |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / 3210 | restart |
| `NMOS_QUERY_ADDRESS` / `NMOS_QUERY_PORT` | registry address / registry port + 1 | restart |
| `NMOS_DNS_SD` | `false` | restart |
| `NMOS_PORT` | 3322 (§15) | restart |
| `NMOS_SEED` | `<HOST_ID>-scopes` | restart |
| `NMOS_LABEL` | empty (`HOST_ID`) | restart |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | restart |
| `NMOS_TAGS` | `{}` | restart |
| `WEB_ENABLE` | `true` | restart |
| `WEB_PORT` | 8170 (§15) | restart |
| `WIDGET_FRAME_ANCESTORS` | `'self'` | restart |
| `PREVIEW_PUBLISH_URL` | empty (own MediaMTX) | restart |
| `PREVIEW_PATH_PREFIX` | `mxl-scopes` | restart |
| `PREVIEW_WHEP_URL` / `PREVIEW_HLS_URL` | empty (own MediaMTX) | restart |
| `MEDIAMTX_RTSP_PORT` | 8574 | restart |
| `MEDIAMTX_WHEP_PORT` | 8579 | restart |
| `MEDIAMTX_HLS_PORT` | 8578 | restart |
| `MEDIAMTX_ICE_UDP_PORT` | 8279 | restart |
| `LOG_LEVEL` | `info` | live |
| `LOG_FORMAT` | `json` | restart |
| `SHUTDOWN_TIMEOUT_S` | 10 | restart |

Ports MUST NOT collide with each other (the `MEDIAMTX_*` ports count only in own mode). A port that cannot be bound exits 75.

---

## 9. Web UI, REST and widgets

Unauthenticated, protected networks only, as the siblings. `WEB_ENABLE=false` answers 404 for the UI, the widgets and every mutating route; GET APIs and the probes stay.

### 9.1 Web UI

The family look of the siblings: header with label, state and registration pills and the version; tabs with their own address.

- **Scope** (`#scope`): the live preview (WHEP, HLS fallback) and the live controls: mode, update rate, sampling preset, intensity, graticule units and level, trace colour, range. The measurements (§4.7) under the picture. Changes are sent at most 10 times per second.
- **Input** (`#input`): state, routed sender and flow, format, colorimetry warning, grains, resyncs, update counters.
- **NMOS** (`#nmos`): node, registration, receiver and sender, IS-05 activation and disable (the NMOS Node and Connection APIs answer on `NMOS_PORT` only; the tab shows their state and ids, and IS-05 activation goes to `NMOS_PORT`).
- **Status** (`#status`): probes, SIMD level, backend, preview state, the counters of `/metrics`.
- **Settings** (`#settings`): every setting with its origin; env-set keys read-only; export and import.

### 9.2 REST

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/info` | version, MXL revision, backend, `simd`, label, `preview` (`mode`, `path`, `whep`, `hls`, `public`) |
| GET | `/api/v1/status` (also `/statusz`) | input, output, renderer, preview, NMOS summary |
| GET | `/api/v1/scope` | the display settings |
| PATCH | `/api/v1/scope` | any subset of the live display settings; 400 on an invalid value, nothing changes |
| GET | `/api/v1/input` | state, route, format, colorimetry, counters |
| GET | `/api/v1/output` | flow id, format, key, frames, late, missed |
| GET | `/api/v1/measurements` | the latest measurements (§4.7) with the update index: `{"update", "mode", "codes_per_bin", "luma": {"min_code", "max_code", "mean_code", "below_64_ratio", "above_940_ratio"} \| null, "chroma": {"out_of_range_ratio"} \| null, "rgb": {"out_of_gamut_ratio"} \| null}` |
| GET | `/api/v1/snapshot.png` | SHOULD: the latest scope frame as PNG (full raster, key as PNG alpha when on) |
| GET | `/api/v1/events` | WebSocket: state, settings changes, measurements at most 10 per second |
| GET | `/widgets`, `/widget/{id}` | §9.3 |
| GET/PUT | `/api/v1/config` | flat key update; `restart_required` when a restart key changes; `flow_keys` lists the output flow keys that changed (they apply at once with a new flow id, §7) |
| GET | `/api/v1/config/export` | one JSON document: `version`, `secrets: false`, `settings`, `routes` |
| POST | `/api/v1/config/import` | restore it; env-set keys are skipped and listed; `restart_required`, `keys` and `flow_keys` as for `PUT /api/v1/config` |
| GET | `/api/v1/config/env` | `KEY=value` text |

A `PATCH /api/v1/scope` is saved to the config file, so the scope comes back the same after a restart.

### 9.3 Widgets

The widget contract of the siblings (platform Appendix B.2; mxl-webrtc-monitor 1.3 is the reference for a video widget).

- `GET /widgets` answers `[{id, title, params, min_size: {w, h}, version}]`; GET only (405 with `Allow: GET`); `Access-Control-Allow-Origin` for an origin listed in `WIDGET_FRAME_ANCESTORS` (or `*`; a host wildcard such as `https://*.example.com` matches the subdomains, as in `frame-ancestors`), `Vary: Origin`, never the API's CORS headers on top.
- **`scope`**: `params` `controls` (boolean, default false), `measurements` (boolean, default false); `min_size` 320×180. The scope's WHEP picture (HLS fallback), filling the frame. With `controls`, a slim bar with the mode and the update rate. With `measurements`, one line of luma min/max and out-of-range share. One instance has one scope, so there is no channel parameter.
- `GET /widget/scope[?controls=][&measurements=][&theme=dark|light|transparent]`; an invalid parameter answers 400, an unknown widget 404.
- `/widget` routes carry `Content-Security-Policy: frame-ancestors <WIDGET_FRAME_ANCESTORS>` (default `'self'`) and no `X-Frame-Options`. `;`, `,` or control characters in the value exit 78.
- The page posts `{type: "widget-ready"}` at its first picture and `{type: "widget-size", w, h}` then and on every resize, to `window.parent`.
- With `SCOPES_PREVIEW_MODE=off` the widget shows the PNG snapshot once per second (SHOULD) and says "preview off".

### 9.4 Probes

- `/livez`: 200 while the renderer and writer heartbeats are younger than 5 s. Never depends on the registry.
- `/readyz`: 200 when the writer heartbeat is fresh, the output flow exists and, with a registry address set and `NMOS_ENABLE=true`, the Query API returns the node. An input in `waiting` or `no_signal` does not fail readiness (the output carries the state). The preview does not count.

---

## 10. Metrics

Prefix `mxl_scopes_`.

| Metric | Type | Labels |
| --- | --- | --- |
| `info` | gauge 1 | `version`, `mxl_revision`, `backend`, `simd` |
| `simd_level` | gauge 1 | `level` |
| `input_state` | gauge 1 for the state | `state` (`not_routed`, `waiting`, `running`, `holding`, `no_signal`, `format_mismatch`) |
| `input_grains_total` | counter | |
| `input_resyncs_total` | counter | |
| `updates_total` | counter | `mode` |
| `updates_skipped_total` | counter | |
| `updates_torn_total` | counter | |
| `accumulate_dropped_total` | counter | grains `SCOPES_ACCUMULATE=all` could not add (behind the ring, or more than the update was sized for) |
| `update_rate_hz` | gauge | effective rate |
| `analyse_seconds` | histogram | `mode` |
| `analyse_cpu_seconds` | histogram | `mode` (CPU time of the renderer thread in the analyse step, strip workers not included; below `analyse_seconds` when the thread waited for its core) |
| `render_seconds` | histogram | `mode` (building the trace groups on the renderer thread, §2.3; the writer's patching is not in it) |
| `output_frames_total` | counter | |
| `output_frames_late_total` | counter | |
| `output_frames_missed_total` | counter | |
| `luma_min_code`, `luma_max_code`, `luma_mean_code` | gauge | the measurements of §4.7; absent while null |
| `luma_below_legal_ratio`, `luma_above_legal_ratio` | gauge | |
| `chroma_out_of_range_ratio` | gauge | |
| `rgb_out_of_gamut_ratio` | gauge (RGB modes) | |
| `nmos_registry_up` | gauge 0/1 | |
| `nmos_activations_total` | counter | |
| `preview_publish_mode`, `preview_publish_state`, `preview_encoder` | gauge 1 | as mxl-multiviewer |
| `preview_frames_total` | counter | |
| `preview_encode_seconds` | histogram | |

Buckets for `analyse_seconds` and `render_seconds`: 0.5, 1, 2, 4, 8, 16, 32 ms. `deploy/grafana/mxl-scopes.json` graphs updates, skipped, analyse and render time, output late and missed, and the measurements.

---

## 11. Process lifecycle and failure handling

Startup: validate config (else 78) → state directory → SIMD level → own preview mode: `mediamtx.yml` and the child → output domain (78 if a mirror or not creatable; `domain_id_mismatch` logged and the file's id kept) → bind web and NMOS (else 75) → restore the route → start reader, renderer, writer → register.

| Failure | Behaviour |
| --- | --- |
| Input flow missing | `waiting`, retry with backoff; the output shows the graticule and `WAITING` |
| Input stops | the last scope frame holds for `SCOPES_HOLD_MS`, then `NO SIGNAL` |
| Writer restarts the input flow | re-open (as mxl-multiviewer §5.3); `input_resyncs_total` |
| Input not v210/v210a, or outside the caps | `format_mismatch`; output shows `FORMAT`; nothing is analysed |
| Input format change | reconfigure at the next update; output unchanged |
| Renderer slower than the update rate | ticks skipped and counted; output keeps its rate |
| Grain overwritten while analysed | result dropped, `updates_torn_total` |
| Writer late | counted; it never waits for the renderer |
| MediaMTX or publish fails | preview `error`, reconnect every 2 s; own child restarts with backoff; not part of readiness |
| NVENC unavailable | x264, logged once |
| Requested SIMD level missing | next level below, warning |
| Registry down | keeps running; `/readyz` 503 with the reason |

SIGTERM or SIGINT, within `SHUTDOWN_TIMEOUT_S`: stop HTTP, stop the preview (RTSP TEARDOWN), stop the media threads and release the MXL reader and writer, delete the IS-04 resources (node last), stop the own MediaMTX, remove the own output domain when `MXL_CLEANUP_ON_EXIT=true`, exit 143.

| Code | Meaning |
| --- | --- |
| 0 | `--help`, `--version` |
| 75 | a port could not be bound, or another startup failure |
| 78 | invalid configuration, or the output domain is a mirror |
| 143 | SIGTERM or SIGINT, also when the budget ran out |

The container runs as uid/gid 1000, needs no GPU, no `hostIPC`, no extra capabilities, and the pod network.

---

## 12. Deployment and CI

- Image `ghcr.io/leeo86/mxl-scopes`, public. Tags on `vX.Y.Z`: `X.Y.Z`, `X.Y`, `X`, never moved. `main`: `nightly-dev` and `git-<sha7>`. No `latest`. OCI labels: `org.opencontainers.image.source`, `.revision`, `.licenses`, `.version` (set explicitly), and `io.dmf.mxl.revision` = the MXL pin.
- The image carries MediaMTX (pinned, with its licence) and Ubuntu's FFmpeg libraries, and sets `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` so NVENC is injected when a GPU is assigned. The scopes need no GPU.
- CI (`ci.yaml`): build MXL and nmos-cpp at the pins, build, unit tests (scalar and AVX2 on the runner; the bit-exact tests skip AVX2 only if the runner lacks it, and say so), integration tests on the CPU (`scope.sh`, `preview.sh` with the MediaMTX release binary), container build. `tests/nmos/amwa.sh` runs IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 outside the default job.
- `docker/docker-compose.demo.yaml`: registry stand-in, a bars/ramp writer, the scopes, and a note to attach mxl-webrtc-monitor or the multiviewer. `deploy/k8s/deployment.yaml`: pod network, uid 1000 with `supplementalGroups: [1000]`, MXL root `hostPath`, writable `/config`, probes, `terminationGracePeriodSeconds` > `SHUTDOWN_TIMEOUT_S`, `NMOS_HOST_ADDRESS` from `status.podIP`, `MXL_CLEANUP_ON_EXIT=true`.
- Release notes per platform Appendix B.3: tag and digest, new or changed settings, ports and volumes, widget ids and params, what was tested on hardware.

---

## 13. Performance targets

Measured on hardware, not in CI; recorded in `docs/performance.md`. Hosts: the platform class (8-core AVX2 desktop CPU, no AVX-512, Ampere workstation GPU) and the lab class (Skylake-SP server CPU with AVX-512, Ampere multi-GPU board).

| Case | Target |
| --- | --- |
| Luma, 1080p50 in, 1080p50 out, update every frame, AVX2 | analyse + render p99 < 4 ms (bars); process about 0.45 core for a real picture, about 0.2 core for bars (preview off); 0 late output grains in 1 h |
| Luma, same, update 25 Hz | process about 0.25 core for a real picture, about 0.1–0.15 core for bars |
| RGB parade, 1080p50, every frame, AVX2 | analyse + render p99 < 8 ms; process < 0.4 core |
| Luma, 2160p50 in, 1080p50 out, every frame, AVX2 | analyse + render p99 < 12 ms (line step 1) |
| Scalar reference, luma 1080p50, every frame | still real time (p99 < 15 ms) |
| Preview on (x264, 540p25) | adds < 0.25 core |
| Scope latency | input grain origin → first output grain that shows it ≤ one update interval + 2 output frames |
| Memory | < 300 MB RSS at 1080p (measured 2026-10-23: 63–116 MB on the platform; in the lab 80–160 MB anonymous plus the shared MXL grains the process maps, docs/performance.md "M4") |

Accepted deviation (the maintainer, 2026-10-22): the RGB parade every frame on camera content takes 0.67–0.74 core on the lab class (analyse + render p99 11.8 ms), above the targets of its row; bars meet them (0.40 core). At the default 25 Hz it takes 0.39–0.46 core. With `SCOPES_THREADS=2` (M2) an update takes about 7 ms (p99 below 8 ms), the CPU stays the same; IMPLEMENTATION_PLAN.md deviation 25.

The process figures are the M0 measurements on the lab class (Skylake-SP, 3 GHz; accepted by the maintainer 2026-10-15; IMPLEMENTATION_PLAN.md "Analyse in the process"). They include an idle-clock effect: the process is idle most of the time, so the host's power management runs its core at about two thirds of the clock (2.2 against 3.3 GHz measured), and the same work takes up to twice as long as in the busy benchmark. A performance power profile on the host lowers the figures; the desktop class is expected lower as well.

The Kubernetes default for the catalog (§15) comes from these numbers.

---

## 14. Testing

### 14.1 Unit (doctest, no libmxl)

- **Bit-exact SIMD against scalar** for each mode: random v210 frames (all codes 0–1023, including the reserved 0–3 and 1020–1023), edge frames (all 0, all 1023, flat 64/940/512), rasters whose width is not a multiple of 6 or 48 (1280, 720, 1) and the short last group, every line and pixel step, every column count from 1 to the input width at a few points, `uint16` and `uint32` counters, 1 and N threads. Histograms equal count for count; rendered v210 (and key plane) equal byte for byte.
- v210 unpack and pack round trip, bit-exact.
- R′G′B′: SIMD = scalar exactly; scalar vs the double BT.709 formula ≤ 1 LSB at every Y, Cb, Cr corner; gamut counting from unclamped values.
- Bin mapping: code 64 lands on the 0 % graticule row and 940 on the 100 % row for every supported `B`; columns cover the plot without gaps.
- Intensity LUT: monotonic, `c = 0` is background, `c ≥ 1` is at least `SCOPES_TRACE_MIN`; normalisation keeps a flat field's intensity within ±1 code across line and pixel steps.
- Graticule: lines at the computed rows for mV, %, code and full range; labels inside the margin.
- Golden pictures: a luma ramp gives a diagonal; 75 % and 100 % colour bars give horizontal segments at the known codes in every mode; a flat grey gives one line at its code. Stored as hashes of the output plus PNGs for review.
- Update scheduler: 25 Hz from 50p takes every second grain; 10 Hz from 59.94p never analyses a grain twice and never drifts; `frame` analyses each grain once; a slow update skips a tick; `rgb-overlay` at `frame` takes every second grain from 50p and 59.94p.
- Measurements from known histograms.
- Config precedence, validation (78), UUIDv5 ids, ports, widget list and pages (params, CSP, CORS, no `X-Frame-Options`), preview settings.

### 14.2 Integration (CI, CPU, real MXL in a temp root)

`tests/integration/scope.sh`: pattern writer (bars and ramp, 1080p50) and registry stand-in; IS-05 activation; `/readyz` 200; output `flow_def.json` matches the raster and media type (v210, then v210a with `SCOPES_OUTPUT_KEY=true`); sampled output pixels show trace at the expected rows and background elsewhere; key plane 0 on the background; a `PATCH /api/v1/scope` changes the mode at a later grain without a new flow id; `SCOPES_OUTPUT_FORMAT` change mints a new flow id and the sender follows; update counter matches the update rate over 10 s; routing to a missing flow → `waiting` → writer starts → `running`; a persisted route is restored at start; `/metrics` exposes `mxl_scopes_updates_total`; config export and import; SIGTERM exits 143, the node leaves the Query API and the domain is removed with `MXL_CLEANUP_ON_EXIT=true`.

`tests/integration/preview.sh`: as mxl-multiviewer's (own mode, shared mode, MediaMTX killed and restarted, `/widgets` and the widget page).

### 14.3 Lab (hardware, not CI)

- §13 on both host classes, `SCOPES_SIMD=avx2` and `scalar`, preview on and off, 1 h each.
- The multiviewer shows the scope as a tile, and with `SCOPES_OUTPUT_KEY=true` as an overlay tile over its input's picture.
- The `scope` widget on an operator screen in the production designer.
- Two instances on one node with distinct ports.
- Production up and down on the platform: ready, routed, deregistered, domain removed.

---

## 15. What the platform needs

### 15.1 Ports

Defaults in use by the siblings (from their READMEs): decklink 8080/3212, st2110-gateway 8090, fabrics-agent 8095/3232, webrtc-monitor 8100/3242 (MediaMTX 8554, 8889, 8888, 8189, 9997, 9998; TSL 8912/8913), multiviewer 8110/3262 (MediaMTX 8754, 8789, 8788, 8389; TSL 8910/8911), srt-gateway 8120/3272, test-player 8130/3282, color-corrector 8140/3292, replay 8150/3302 (MediaMTX 8854, 8689, 8688, 8489), audio-mixer 8160/3312 (MediaMTX 8954, 8599, 8199), browser-source 8160/3312, FlowXer 9620 (engine 8654, 8989, 8988, 8289, 9897), registry 3210/3211.

For mxl-scopes (the next free step of the pattern; confirmed by the maintainer 2026-10-15):

| Port | Setting | Default |
| --- | --- | --- |
| Web, REST, probes, metrics, widgets | `WEB_PORT` | 8170 |
| NMOS Node and Connection APIs (+1 reserved) | `NMOS_PORT` | 3322 (3323) |
| Own MediaMTX RTSP (127.0.0.1) | `MEDIAMTX_RTSP_PORT` | 8574 |
| Own MediaMTX WHEP | `MEDIAMTX_WHEP_PORT` | 8579 |
| Own MediaMTX HLS | `MEDIAMTX_HLS_PORT` | 8578 |
| Own MediaMTX ICE (UDP and TCP) | `MEDIAMTX_ICE_UDP_PORT` | 8279 |

On the platform the shared MediaMTX is used, so only the web and NMOS ports are Service ports. Finding: mxl-browser-source and mxl-audio-mixer both default to 8160/3312; that does not block this function but the platform should give one of them new defaults.

### 15.2 Catalog entry

In `media_function_catalog` (`inventories/shared/group_vars/all/apps.yml`):

```yaml
  scopes:
    app: mxl-scopes
    short: scopes
    layers: [production]
    network: pod
    web_port: 8170
    websocket: true
    ports: {web: 8170, nmos: 3322}
    gpu: none                        # optional later (NVENC preview, CUDA engine)
    config_dir: /config
    default_values: {mode: luma, updateRate: 25, format: 1920x1080, key: false}
    export: [{path: /api/v1/config/export, file: config.json}]
    default_resources:
      requests: {cpu: "500m", memory: 384Mi}   # M4: docs/performance.md, deploy/k8s/deployment.yaml
      limits: {cpu: "2", memory: 1Gi}
    default_flows:
      - {format: 1080p50-v210, count: 1, history_duration_ms: 200}   # 1080p50-v210a with key: true
```

### 15.3 Chart type

A `mxl-common.type.scopes` template in the common chart, in the style of the color-corrector type:

- env: `SCOPES_MODE`, `SCOPES_UPDATE_RATE`, `SCOPES_OUTPUT_FORMAT`, `SCOPES_OUTPUT_KEY` from `values`; `SCOPES_STATE_DIR=/config`; `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`, `MXL_CLEANUP_ON_EXIT=true`; `WEB_PORT`, `NMOS_PORT`; the common NMOS env (registry, `NMOS_SEED=<production>-<function>`, label, tags, `NMOS_HOST_ADDRESS` = pod IP); the shared preview (`PREVIEW_PUBLISH_URL`, `PREVIEW_PATH_PREFIX=<production>/<function>`, `PREVIEW_WHEP_URL`, `PREVIEW_HLS_URL`); `WIDGET_FRAME_ANCESTORS='self' https://designer.<fqdn>`;
- Service ports `http` and `nmos`; metrics on `http`; probes `/livez`, `/readyz`; MXL mount and config mount; `terminationGracePeriodSeconds` 20.
- `docs/productions.md` types row: `scopes` | `LeeO86/mxl-scopes` | none (AVX2) | `mode`, `updateRate`, `format`, `key` | config | one input per instance; about 0.1 core per 1080p50 luma scope at 25 Hz (measured on the platform 2026-10-23: 0.10 luma, 0.16–0.26 the other modes, 0.17 with a key).

### 15.4 Designer

The `scope` widget (§9.3) in the production designer's widget list. The multiviewer needs nothing: it routes the scope sender to a tile like any flow.

---

## 16. Decisions (the user, 2026-10-14)

The open questions of the draft, now decided: question 7 is **MIT** (not Apache-2.0); the update rate is **25 Hz** (question 1); the combined display is **`luma-chroma`** (question 6); every other question takes its recommendation as written below.

### The questions as drafted

1. **Default update rate.** `frame` gives the smoothest scope; 25 Hz halves the analyse cost at 50p and looks the same to the eye. *Recommendation:* default `25`, `frame` per instance for critical scopes.
2. **Output raster.** 1920×1080 (sharp on a large tile, 53 MB on the tmpfs) or 960×540 (a quarter of the memory and render time, soft when shown large). *Recommendation:* 1920×1080 default; 960×540 for walls with many small scopes.
3. **Output rate.** Follow the input rate (a re-route to another rate changes the flow id) or a fixed rate. *Recommendation:* `input`; the platform may pin `50` per production to keep the flow id stable.
4. **Key output default.** *Recommendation:* off (`video/v210`); on per instance when the scope is keyed over a picture.
5. **One input per instance.** *Recommendation:* yes for v1 (§6); revisit `SCOPES_INPUTS` only if productions need more than about 8 scopes per node.
6. **Name of the combined Y-against-chroma display.** *Recommendation:* `luma-chroma` ("Luma/Chroma"). Alternatives: `yc-split`, `component-xy`.
7. **Repository and licence.** *Recommendation:* `LeeO86/mxl-scopes` (decided: **MIT**, like mxl-browser-source and mxl-audio-mixer); check Blend2D (Zlib), MediaMTX (MIT) and FFmpeg (LGPL, dynamically linked) against it.
8. **AVX-512.** *Recommendation:* not in v1. The histogram increment is scalar, so the gain is small, and on Skylake-SP 512-bit code slows the core's other hyperthread. Re-check after M0 on the lab host. **Decided 2026-10-15: in v1** with run-time dispatch AVX-512 → AVX2 → scalar (§3.2).
9. **Graticule units default.** *Recommendation:* `mv`, with `percent` one click away in the UI.
10. **Trace colour default for luma.** *Recommendation:* `green`; `white` for operators who prefer it.
11. **Preview quality (judged in the lab 2026-10-23: at 540p the traces of a camera picture stay readable; the area average dims one-pixel lines, the MXL output keeps the full picture).** H.264 4:2:0 at 540p softens thin coloured traces. A browser-drawn scope (histogram over the WebSocket, drawn on a canvas) would be sharper and cheaper, but it is a second renderer. *Recommendation:* WebRTC for v1 as decided; judge it in the lab (M3); keep the canvas as a stage-2 option.
12. **Alarms.** Out-of-range shares are measured. Should they raise alarms (for example to the multiviewer as a tally or label)? *Recommendation:* metrics and UI only in v1; alarms after the operators have seen real numbers.
13. **Persistence.** *Recommendation:* stage 2, off by default.
14. **Ports.** *Recommendation:* 8170/3322 and the MediaMTX block 8574/8579/8578/8279, after a check against the platform's current port table (`ports_media`) and FlowXer; and resolve the 8160/3312 clash of browser-source and audio-mixer at the same time.

---

## 17. References

- MXL — https://github.com/dmf-mxl/mxl (pin `218ddaa`)
- AMWA IS-04 v1.3, IS-05 v1.2, BCP-007-03, BCP-004-01, BCP-002-01
- ITU-R BT.709-6 (matrix, narrow range); SMPTE ST 274; SMPTE RP 219 (colour bars)
- EBU R 103 (video signal tolerance)
- Sibling specifications: `LeeO86/mxl-multiviewer`, `mxl-webrtc-monitor`, `mxl-color-corrector`, `mxl-browser-source`, `mxl-audio-mixer`

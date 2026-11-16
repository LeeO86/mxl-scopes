# Performance

Measured on hardware, not in CI (SPECIFICATION §13). M2 figures at the end. Lab class: Skylake-SP server CPU (Xeon Gold 6136, 3.0 GHz, AVX-512), one socket for the scopes and their sources, other work on the host. The desktop class (8-core AVX2 desktop CPU) is still to be measured.

The figures move with the host: the scope is idle most of the time, so the power management lowers its core's clock and the same work takes longer (IMPLEMENTATION_PLAN.md "Analyse in the process"). Runs of the same point on different cores or at different times differ by up to a quarter; compare figures from one session only.

## Engine (M1, 2026-10-16)

`mxl-scopes-bench`, one thread pinned to one core, 1920×1080 output, strip width 64, every line and sample, median ms (p99 of analyse + render in brackets). Analyse = the counts of one input frame; render = counts to the packed trace groups (shared code for every SIMD level). Patterns: `bars75` (flat areas), `scene` (smooth picture with colour, closest to camera content), `random` (every sample a random code, the worst case).

1080p input:

| Mode | Pattern | Scalar analyse | AVX2 analyse | AVX-512 analyse | Render | AVX2 total |
| --- | --- | --- | --- | --- | --- | --- |
| `luma` | bars75 | 4.5 | 1.50 | 1.87 | 1.25 | 2.75 (3.5) |
| `luma` | scene | 5.3 | 2.01 | 2.33 | 2.40 | 4.5 (5.6) |
| `luma` | random | 6.8 | 3.29 | 3.85 | 5.58 | 8.8 (9.6) |
| `ycbcr-parade` | bars75 | 8.7 | 3.67 | 4.25 | 1.31 | 5.0 (6.1) |
| `ycbcr-parade` | scene | 9.2 | 4.31 | 5.17 | 2.20 | 6.8 (7.5) |
| `ycbcr-parade` | random | 14.5 | 7.17 | 8.18 | 5.48 | 12.6 (13.2) |
| `rgb-parade` | bars75 | 16.2 | 6.13 | 7.23 | 1.40 | 7.6 (9.9) |
| `rgb-parade` | scene | 19.5 | 8.22 | 9.95 | 2.81 | 11.0 (11.8) |
| `rgb-parade` | random | 25.7 | 9.66 | 11.1 | 5.45 | 15.0 (15.7) |
| `rgb-overlay` | bars75 | 14.9 | 4.91 | 5.95 | 2.29 | 7.2 (7.9) |
| `rgb-overlay` | scene | 19.4 | 8.53 | 9.65 | 7.75 | 16.3 (16.8) |
| `rgb-overlay` | random | 28.3 | 14.1 | 16.4 | 13.3 | 27.4 (28.4) |

2160p input (`scene`, AVX2, total): `luma` 14.1 ms (line step 2: 9.7), `ycbcr-parade` 19.4 (11.3), `rgb-parade` 33.4 (18.8), `rgb-overlay` 43.5 (30.7).

- The analyse step of the luma waveform got 15 % faster in M1 (1.75 → 1.50 ms, bars): the bin and cell multiplies use `vpmaddwd` (one µop) instead of `vpmulld` (two); the products are exact because every factor fits 16 bits.
- The parades count two (YCbCr) or three (RGB) samples per luma sample. The scalar increments dominate as in M0; the RGB step adds the Q13 matrix in 32-bit lanes (§4.8), about 3.3 ms of its 8.2 ms on `scene`.
- The overlay draws three planes over the full plot width: three times the cells of a parade panel set, so its render is the largest.
- AVX-512 stays slower than AVX2 in every mode (`auto` picks AVX2; AVX-512 has no `vpmaddwd` without AVX-512BW, so its new steps use `vpmulld`).

## Process (M1, 2026-10-16)

`mxl-scopes` image pinned to one socket-1 core, 1080p50 output, AVX2, NMOS on without a registry, preview off. Sources: the test player 1.2.1 playing a 12 s camera clip (720p50 interview, upscaled by the player) as 1080p50 and as 1080i50, and `mxl-scopes-writer` 75 % bars 1080p50. 20 s per point after 5 s settling; CPU of all threads from `/proc`; analyse and render are the process's own averages. 0 late and 0 missed output grains and 0 torn reads in every point below.

| Input | Mode | 25 Hz: core (analyse / render ms) | Every frame: core (analyse / render ms) |
| --- | --- | --- | --- |
| Camera 1080p50 | `luma` | 0.25–0.33 (2.8 / 2.5) | 0.40–0.47 (2.8 / 2.6) |
| Camera 1080p50 | `ycbcr-parade` | 0.34 (5.0 / 1.5) | 0.44–0.51 (5.1 / 1.5) |
| Camera 1080p50 | `rgb-parade` | 0.39–0.46 (8.3 / 2.8) | 0.67–0.74 (8.3 / 2.6) |
| Camera 1080p50 | `rgb-overlay` | 0.58–0.63 (9.4 / 10.5) | 0.99, 2–4 % of the ticks skipped (9 / 10.5); since 2026-10-22 at most 30 Hz (25 Hz from 50p), see below |
| Camera 1080p50 | `luma`, `SCOPES_ACCUMULATE=all` | 0.32 (5.4 for two frames / 2.6) | — |
| Camera 1080i50 | `luma` | 0.26 (3.4 / 2.5) | 0.26 (`frame` = 25 updates/s on 1080i50) |
| Camera 1080i50 | `ycbcr-parade` | 0.30 (6.3 / 1.6) | — |
| Camera 1080i50 | `rgb-parade` | 0.44 (9.9 / 2.6) | 0.43 |
| Bars 1080p50 | `luma` | 0.08 (1.6 / 0.6) | 0.18 (2.1 / 0.6) |
| Bars 1080p50 | `ycbcr-parade` | 0.14 (4.5 / 0.4) | 0.31 (4.9 / 0.4) |
| Bars 1080p50 | `rgb-parade` | 0.20 (6.8 / 0.4) | 0.40 (6.9 / 0.4) |
| Bars 1080p50 | `rgb-overlay` | 0.19 (5.4 / 1.3) | 0.38 (5.5 / 1.3) |

Against §13 (lab class): the luma waveform meets "about 0.25 core at 25 Hz and about 0.45 core every frame for a real picture" within the host's spread, and stays at 0.1–0.2 core for bars. The RGB parade every frame meets "< 0.4 core" for bars (0.40) and not for camera content (0.67–0.74; accepted for M1 by the maintainer on 2026-10-22, IMPLEMENTATION_PLAN.md deviation 25); its analyse + render p99 is 9.9 ms (bars) and 11.8 ms (scene) against < 8 ms. At the default 25 Hz the RGB parade takes 0.39–0.46 core on camera content. Options, in order of expected gain: strip workers (`SCOPES_THREADS`, M2) to spread the analyse over two cores; packing the half-empty second vector of the RGB step (about 10 % of the analyse); the desktop class, expected about 1.5 times faster per update.

A/B of the M1 thread change (the trace groups computed by the analysing thread, not the writer; same session, cores swapped between runs): M0 image 0.29 core at 25 Hz and 0.45 every frame, M1 image 0.29 and 0.40. The total work is the same; the writer no longer runs late when an update takes long (`rgb-overlay` every frame: 412 late and 48 missed output grains in 20 s before, 0 now; the analysing thread skips ticks instead, §4.6).

## RGB overlay at most 30 Hz: 25 Hz from 50p (2026-10-22)

Decided by the maintainer: when the overlay cannot update every frame, it updates at 25 Hz from 50p with all its columns instead of skipping ticks. Built as a fixed cap, not a setting (SPECIFICATION §4.6): `rgb-overlay` updates at most every n-th input frame, n the smallest that keeps it at or below 30 Hz (25 Hz from 50p, 29.97 Hz from 59.94p, every frame from 25p, 29.97p and 1080i50). `frame` and higher rates run at that rate, lower rates stay; `accumulate=all` adds both frames of an update.

Same session, same host and cores (test player 1.2.1 on two cores, the scope on one core, camera clip 1080p50, 1080p50 out, AVX2, 20 s per point after 5 s settling; `measure.sh` of the M1 lab). Before = PR head 2ea01f4, after = this change. 0 late and 0 missed output grains and 0 torn reads in every point.

| `rgb-overlay`, update rate | Before: core, updates/s, skipped in 20 s | After: core, updates/s, skipped in 20 s |
| --- | --- | --- |
| `frame` | 0.85 and 0.92, 50.0, 2 | 0.51, 25.1, 0 |
| 30 Hz | 0.59, 30.1, 0 | 0.52, 25.1, 0 |
| 25 Hz | 0.51 and 0.52, 25.1, 0 | 0.51, 25.1, 0 |
| 10 Hz | — | 0.26, 10.0, 0 |
| `frame`, `accumulate=all` (10 s) | — | 0.68, 25.1, 0 (both frames analysed, 17.5 ms analyse per update) |

Analyse about 7.5 ms and render about 10.8 ms per update in every point. In this session, before the cap, every frame skipped only 2 ticks in 20 s (the core ran at a higher clock than on 2026-10-16, when 2–4 % were skipped) but took 0.85–0.92 core; with the cap the overlay takes the 25 Hz figure (about half a core) and never skips. `luma` at `frame` still updates 50 times a second (0.31 core).

## M2 (2026-10-22)

### Engine

`mxl-scopes-bench`, 1080p `scene`, 1920×1080 output, AVX2, strip width 64, median ms (p99 of analyse + render in brackets); two cores of the lab host with idle hyperthread siblings.

| Mode | 1 thread: analyse / render / total | 2 threads: analyse / render / total |
| --- | --- | --- |
| `luma` | 1.70 / 2.28 / 3.99 (4.27) | 0.89 / 1.73 / 2.62 (2.87) |
| `ycbcr-parade` | 4.04 / 1.93 / 5.98 (6.41) | 2.18 / 1.51 / 3.69 (4.30) |
| `rgb-parade` | 7.70 / 2.45 / 10.1 (10.8) | 4.20 / 1.96 / 6.17 (7.08) |
| `rgb-overlay` | 6.85 / 7.13 / 14.0 (14.7) | 3.51 / 4.88 / 8.44 (9.29) |
| `ycbcr-overlay` | 3.31 / 6.55 / 9.86 (10.6) | 1.70 / 4.07 / 5.78 (6.42) |

- Two strip workers halve the analyse step; the trace step gains less (its parts are joined in order, about 0.4 ms for the overlay's 130 000 groups). The RGB parade with two threads meets the §13 time target (p99 7.1 ms < 8 ms); its CPU does not change (below).
- Against M1 on one thread (same session, interleaved): luma and YCbCr parade analyse unchanged; RGB analyse about +8 % (the exact gamut count of §4.7); the trace step +0.25–0.45 ms (the bin totals of the measurements, the key bytes of the trace groups). The RGB modes skip the bin totals (they measure only the gamut).
- First M2 builds were slower on one thread: strips cut at every column boundary (now only at worker boundaries), a gamut count by movemask and popcount (now counted in vector lanes) and bin totals summed in 64 bits (now 32). Two workers' trace vectors on one cache line made two threads slower than one (false sharing; now each on its own line).

### Process, before and after

Lab host as in M1; the test player 1.2.1 playing the camera clip as 1080p50 on its own core; the scope on one core (one thread) or two cores (`SCOPES_THREADS=2`); 1080p50 out, AVX2; `measure.sh` of M1, 20 s per point after 5 s. Two rounds of main (M1), M2 one thread, M2 two threads, interleaved; the ranges cover both rounds (main `luma` from the second round only: in the first the player had just started). Core = CPU of all threads.

| Mode | M1, 1 core: 25 Hz / every frame | M2, 1 thread | M2, 2 threads |
| --- | --- | --- | --- |
| `luma` | 0.20 / 0.31 | 0.23–0.24 / 0.36–0.37 | 0.29–0.30 / 0.43–0.44 |
| `ycbcr-parade` | 0.22 / 0.35–0.36 | 0.24–0.25 / 0.39–0.42 | 0.29–0.30 / 0.43–0.44 |
| `rgb-parade` | 0.34–0.35 / 0.58–0.62 | 0.37–0.39 / 0.64–0.68 | 0.46–0.47 / 0.71–0.72 |
| `rgb-overlay` (at most 25 Hz) | 0.52 / 0.53 | 0.55 / 0.55 | 0.62 / 0.63 |
| `ycbcr-overlay` (at most 25 Hz) | — | 0.41–0.42 / 0.42 | 0.48 / 0.47–0.48 |

Analyse + render per update every frame (the process's own averages, ms): RGB parade M1 7.2–7.8 + 2.6, M2 one thread 7.8–8.4 + 3.0–3.1, two threads 4.4–4.6 + 2.4–2.5; RGB overlay M1 7.6–8.3 + 10.7–10.9, M2 one thread 8.7–9.1 + 10.4–10.5, two threads 5.5–5.8 + 7.0–7.1. Every point: 0 late, 0 missed, 0 torn; 0 skipped ticks.

- M2 on one thread costs 0.02–0.06 core more than M1 (the measurements, the key-capable trace groups, the gamut count in the RGB modes).
- Two threads cost 0.05–0.1 core more than one (the second thread's wake-ups and the split trace) and halve the time an update takes. They are for latency and the overlays' render time, not for CPU: the RGB parade every frame stays above the §13 "< 0.4 core" (deviation 25).

## M3 (2026-10-23): the WebRTC preview

Lab host as in M2, the camera clip as 1080p50, the scope on one core with its own MediaMTX in the same container, 1080p50 out, AVX2, preview 960×540 at 25 fps, 2 Mbit/s, no WebRTC viewer (MediaMTX ingests only). Process CPU of `mxl-scopes` (all threads), 20 s per point.

| Mode, update rate | Preview off | Preview on (x264) | Preview on (NVENC, lab GPU) |
| --- | --- | --- | --- |
| `luma`, 25 Hz | 0.26 | 0.41 (0.40 over 60 s) | 0.34 |
| `rgb-parade`, 25 Hz | 0.39 | 0.54 | — |
| `luma`, every frame | 0.40 | 0.54 | — |

- The preview adds about 0.15 core with x264 (§13: < 0.25), 0.08 with NVENC. MediaMTX ingesting the stream: 0.005 core.
- A first build added 0.27 core and made one output grain late on the shared core; the scaler took 6.6 ms per picture. A straight-loop path for exactly half the raster and a faster v210 unpack bring it to 2.5 ms, and the publisher thread runs at nice 10 so the output writer never waits for it: 0 late and 0 missed output grains in every point above.
- Quality (SPECIFICATION open question 11): at 540p the luma waveform of the camera clip keeps its traces readable; the area average dims one-pixel lines and the 4:2:0 chroma softens coloured traces. The MXL output keeps the full picture.

## M4 (2026-10-23)

### The test platform (i9-9900K hosts, AVX2), scopes 1.0.0

Measured by the platform on test-all, 1080p50 input, 25 Hz, no preview (1.0.0 has none):

| Mode | CPU (core) | RSS |
| --- | --- | --- |
| `luma` | 0.10 | 63–116 MB |
| `ycbcr-parade` | 0.16 | 87 MB |
| `rgb-parade` | 0.19 | 103 MB |
| `rgb-overlay` | 0.26 | 114 MB |
| `ycbcr-overlay` | 0.18 | 114 MB |
| `luma` with `SCOPES_OUTPUT_KEY=true` (v210a) | 0.17 | — |

Keyed scope over a picture (the M2 exit check), 06:24–06:29Z: multiviewer 1.4.6 on CUDA, the scope's v210a tile (scale fill, on top) over a player tile. The picture shows through the key, with the trace and graticule over it (seen by the maintainer). Multiviewer compose 1.56 ms (1.54 unkeyed); 0 late and 0 missed on both.

The platform host is about twice as fast per update as the lab's Skylake-SP at its idle clock (luma 0.10 against 0.23 core).

### Memory per mode (lab, 1.1.0)

`/proc/<pid>/status` of `mxl-scopes` after 20 s per mode, camera clip 1080p50, 25 Hz, 1080p50 out, MB:

| Mode | Preview off: RSS (anonymous / shared) | Preview on | Preview on, 2 threads | Preview on, key |
| --- | --- | --- | --- | --- |
| `luma` | 319 (83 / 202) | 352 (97 / 217) | 342 (113 / 191) | 507 (126 / 343) |
| `ycbcr-parade` | 319 (78 / 207) | 353 (92 / 222) | 336 (107 / 191) | 502 (121 / 343) |
| `rgb-parade` | 339 (88 / 217) | 415 (102 / 275) | 365 (131 / 196) | 514 (134 / 343) |
| `rgb-overlay` | 409 (105 / 269) | 447 (118 / 290) | 388 (148 / 202) | 539 (158 / 343) |
| `ycbcr-overlay` | 409 (105 / 269) | 468 (118 / 311) | 393 (148 / 207) | 543 (162 / 343) |

- "Shared" is RssShmem: the MXL grains the process has mapped and touched. That is the writer's input ring, which the input writer owns and is charged for, plus the scope's own output ring (5.5 MB per 1080p v210 grain, 8.3 MB with a key). How much of it is the scope's depends on the ring sizes; it is not memory the scope allocates. The process's own memory is the anonymous part: 80–160 MB. A further 33–37 MB is mapped files (libraries).
- The built-in MediaMTX (own mode) adds about 41 MB RSS. It is a child process: the RSS above and `process_resident_memory_bytes` do not include it; the container's memory does.
- The platform measures 63–116 MB per scope.
- `deploy/k8s/deployment.yaml` requests 384Mi and limits 768Mi: room for the anonymous memory, the scope's own output ring and the key at 1080p. A UHD output with a key (33 MB per grain, for the output ring and the three preview snapshot buffers) needs a higher limit, about 1Gi or more.

### Soak: two instances on one node, 1 h (lab, 1.1.0)

The test player on its own core plays the camera clip at 1080p50. Two scopes, each on its own core, both with the preview (own MediaMTX each), read it:
- A: `luma` at 25 Hz;
- B: `rgb-parade` at 25 Hz with a key (v210a).

Sampled every 60 s for 60 min:

| Instance | Output grains | Late | Missed | Skipped ticks | Torn | CPU (mean) | RSS start → end |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | 181 213 | 1 (in the first minute) | 0 | 1 (first minute) | 0 | 0.42 | 442 → 453 MB |
| B | 181 105 | 16 (6 in the first minute, then 7 and 4 in two bursts before minute 17, none after) | 1 (first minute) | 2 | 0 | 0.65 | 513 → 512 MB |

Re-run after the review fixes (snapshot copied after the commit into three fixed buffers, x264 with one thread), same set-up, the player on its own core, the other cores of the host busy only with pinned work:

| Instance | Output grains | Late | Missed | Skipped ticks | Torn | CPU (mean) | RSS minute 1 → 60 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | 178 219 | 0 | 0 | 0 | 0 | 0.41 | 341 → 357 MB |
| B | 178 108 | 1 (at start-up) | 0 | 0 | 0 | 0.65 | 514 → 515 MB |

- RSS stays flat over the hour; there is no leak. (A grew 16 MB in a few steps, B not at all; the steps are not broken down into anonymous and shared memory.)
- The late grains of B come at start-up (MediaMTX starting and the encoder opening on the same core) and in two bursts while other work (the CI tools of this session) ran unpinned on the host; the last 43 minutes had none.
- One core per scope is enough for these configurations, but not with headroom for B (0.65 core with a key and the preview). The platform's 2-CPU limit leaves that headroom.
- An earlier attempt was discarded because a build ran on the same cores. In it, `rgb-overlay` with a key and the preview on one core (0.89 core) skipped ticks and made output grains late: that configuration needs more than one core.

### AMWA (CI, 1.1.0)

IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 against the node with an nmos-cpp registry. Every test passes, except:
- the tool's own set-up and tear-down steps;
- IS-10 authorization (out of scope; the tool runs those only with `ENABLE_AUTH`);
- the DNS-SD tests, disabled by `ENABLE_DNS_SD = False` (no mDNS or unicast DNS-SD in CI). So **IS-04 registration and heartbeats are not covered by AMWA**; `tests/integration/scope.sh` runs registration, heartbeats and deregistration against a stub registry only;
- the RTP-only parts of IS-05 (the receiver and sender are MXL, and BCP-007-03-01 tests them);
- the optional BCP-004-01 and BCP-002-02 details;
- two manual checks.

`tests/nmos/amwa-expected-off.json` lists each with its reason. The first run found one real failure: the receiver's `caps.media_types` was missing (BCP-007-03-01 test_07). It is fixed.


## 1.3.0 (2026-11-16): vectorscope and picture colour

`mxl-scopes-bench`, lab class (Xeon Gold 6136), one thread pinned to one core, nothing else on the host's MXL, 1920×1080 output, strip width 64, every line and sample, median ms (p99 of analyse + render in brackets); `--trace-color picture` for the picture colour, `--v210 FILE` for `camera`: a real frame (the lab's 720p50 interview clip, scaled to 1080 lines). The vectorscope analyses with the scalar step on every level (deviation 27), so its AVX2 column is the scalar one. With the logarithmic intensity curve and the faster picture colour (2026-11-16).

1080p input:

| Mode | Pattern | Scalar analyse | AVX2 analyse | Render | AVX2 total |
| --- | --- | --- | --- | --- | --- |
| `luma` | camera | 4.95 | 1.82 | 3.52 | 5.35 (5.68) |
| `luma` | bars75 | 4.69 | 1.43 | 0.88 | 2.31 (2.61) |
| `luma` | scene | 4.97 | 1.71 | 2.52 | 4.24 (4.51) |
| `luma` | random | 6.04 | 2.65 | 6.69 | 9.32 (9.63) |
| `luma` picture | camera | 9.25 | 3.97 | 7.37 | 11.3 (12.3) |
| `luma` picture | bars75 | 7.83 | 3.02 | 1.53 | 4.55 (4.86) |
| `luma` picture | scene | 9.32 | 4.07 | 5.37 | 9.45 (10.2) |
| `luma` picture | random | 15.2 | 7.71 | 15.3 | 23.0 (23.7) |
| `vectorscope` | camera | 3.39 | = scalar | 1.63 | 5.02 (5.82) |
| `vectorscope` | bars75 | 3.19 | = scalar | 1.57 | 4.76 (5.01) |
| `vectorscope` | scene | 3.23 | = scalar | 1.85 | 5.09 (5.46) |
| `vectorscope` | random | 3.71 | = scalar | 4.11 | 7.85 (8.36) |
| `vectorscope` picture | camera | 4.29 | = scalar | 2.15 | 6.44 (6.74) |
| `vectorscope` picture | bars75 | 4.09 | = scalar | 2.10 | 6.20 (6.87) |
| `vectorscope` picture | scene | 4.16 | = scalar | 2.65 | 6.81 (7.11) |
| `vectorscope` picture | random | 7.26 | = scalar | 7.71 | 15.0 (15.9) |

2160p input (`scene`, total at the AVX2 level; line step 2 in brackets): `luma` 13.1 (8.5), `luma` picture 24.7 (15.8), `vectorscope` 15.6 (8.6), `vectorscope` picture 20.1 (11.5).

The picture colour before and after the optimisation of 2026-11-16 (same host and session, AVX2, total; analyse / render): `luma` camera 13.9 → 11.3 ms (4.50 / 9.36 → 3.97 / 7.37), scene 11.5 → 9.45, random 32.8 → 23.0; `vectorscope` camera 5.76 → 6.44 (4.01 / 1.74 → 4.29 / 2.15; within the run-to-run spread of this mode, see below). What it took, found by timing the steps on the camera frame:

- One colour per output pixel pair (4:2:2 has one chroma per pair) from the pair's sums, the luma per pixel: half the colour maths.
- The pairs of a plot row are gathered and coloured in one loop the compiler vectorises: per group, each pair's two divisions formed a dependency chain that did not overlap (31 ns per group alone). Three things kept the loop scalar: `max(1, count)` (GCC branched around the division), a float clamp, possible aliasing.
- The luma sums are one 64-bit word per two histogram columns (6.9 instead of 13.7 MB at a 1920×1080 output), built in vector registers by the AVX2 step; the vectorscope keeps a word per cell (its pixel pairs are one cell).
- Not taken: two sub-histograms or a pair-wise sum for the vectorscope (slower), 16-bit sums (they overflow or lose the chroma's precision).

The vectorscope's figures move with the host more than the other modes (its 2D histogram of 1 MB and the store-forwarding chains): the same binaries in the two lab images, run back to back on one core, gave 4.96 (before) and 4.76 ms (after) for `vectorscope` on bars.

- The vectorscope costs about what the luma waveform costs on a picture. It counts half the samples (one per chroma pair), but neighbouring pairs of a flat or smooth colour hit the same cell, so the increments wait on each other (store forwarding, §4.3). At zoom 5 the render takes longer (the cells spread over more pixels).
- `SCOPES_THREADS=2` (1080p, `scene`, AVX2, before the optimisation): `luma` picture 8.1 ms, `vectorscope` picture 5.8 ms (the vectorscope splits only its trace step).
- Against §13 (targets for `luma`): at 1080p every frame the vectorscope is at p99 5.0–5.5 ms on bars and scene, above the 4 ms bars target; the luma picture colour is at 4.9 ms on bars and 12.3 ms on the camera frame (2.1 times the green trace), and at the scalar level 17.4 ms on the camera frame, above the 15 ms scalar target. At 2160p the vectorscope needs line step 2 for the 12 ms target (8.9 ms p99); the luma picture colour does not meet it even with line step 2 (16.4 ms), so it fits 2160p at 25 Hz, not every frame.

### Process on a real flow (lab, 2026-11-16)

The test player 1.3.1 (CPUs 31, 33) playing the camera clip as 1080p50 into an MXL domain under `/dev/shm`, routed by IS-05 straight to `mxl-scopes` (CPU 37, AVX2, preview off, no registry), 1920×1080 output. 20 s per point after 5 s settling; core = `process_cpu_seconds_total` over the 20 s; analyse and render ms are the process's averages. Before: the branch before the optimisation and the brightness change (`2b7c34e`), after: with both (`0fe19de`). 0 late and 0 missed output grains in every point.

| Mode | Trace colour | 25 Hz before → after: core (analyse / render ms) | Every frame before → after: core (analyse / render ms) |
| --- | --- | --- | --- |
| `luma` | green | 0.26 → 0.27 (3.2 / 3.7 → 2.8 / 3.3) | 0.38 → 0.39 (2.4 / 4.0 → 2.0 / 2.8) |
| `luma` | picture | 0.55 → 0.45 (9.5 / 12.8 → 7.9 / 8.6) | 0.94 → 0.75 (6.5 / 13.3 → 5.8 / 9.4); before 16 updates skipped in 20 s, after none |
| `vectorscope` | green | 0.17 → 0.19 (5.4 / 2.0 → 4.5 / 1.4) | 0.24 → 0.26 (4.0 / 1.3 → 3.3 / 1.2) |
| `vectorscope` | picture | 0.21 → 0.23 (7.1 / 2.7 → 7.7 / 2.1) | 0.29 → 0.32 (4.0 / 1.8 → 4.4 / 2.0) |

The idle-clock effect of §13 shows: the same work takes longer at 25 Hz than every frame. The vectorscope figures differ by about a tenth between the two runs in both directions (green and picture alike), within the spread of single 20 s points.

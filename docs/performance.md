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

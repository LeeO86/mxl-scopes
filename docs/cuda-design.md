# CUDA backend: design note (stage 4)

SPECIFICATION §3.2 and §1.1 stage 4. This is a design note, not code: v1 runs on the CPU, and `SCOPES_BACKEND=cuda` exits 78 (`cuda_backend_not_built`). It records how a CUDA backend fits the engine and gives the same bytes as the CPU.

## Where it plugs in

The engine is three calls (§3.2):
- `configure(input, settings, raster)`: builds the integer tables;
- `analyse(grain)`: counts into the histograms;
- `trace(histogram)`: draws the trace groups.

The CUDA backend keeps that interface. Everything around it stays on the CPU and unchanged:
- the update scheduler;
- the torn-read check;
- the still picture (graticule and texts, drawn once per settings change);
- the copy-free writer;
- the preview.

## Data path

1. **Upload.** The grain is pinned for the upload, not copied first. Its row stride is `ceil(width / 48) × 128` bytes.
   - One `cudaMemcpy2DAsync` per grain into a device ring of two frames, on the backend's own stream, the way mxl-multiviewer 1.4.2 uploads its inputs.
   - Interlaced flows upload the two fields into alternate lines, as the multiviewer's `cudaUploadFields`.
   - The torn-read check stays on the CPU and runs after the upload, so a torn grain drops the counts as on the CPU (§2.2).
2. **Analyse kernel.**
   - One thread block per column strip (the strips of `AnalysePlan`) and per band of lines.
   - Each block keeps its strip's cells in shared memory: 64 columns × B bins × 16 or 32 bits fits 48 KB up to B = 192 with 32-bit cells. Larger B splits the bins into passes or uses global atomics on the strip's slice.
   - Threads unpack v210 groups and look up `binOf` and `columnOf` from constant or texture memory (the same tables as the CPU). Per plane they `atomicAdd` into the shared cell, then add the block's cells into the global histogram with one atomic per non-zero cell.
   - Integer sums do not depend on order, so the counts equal the CPU's.
   - The R′G′B′ kinds use the same Q13 integer formula (§4.8). The gamut count is one `atomicAdd` per block of the per-thread counts.
3. **Trace kernel.**
   - One thread per 6-pixel group of a plot row. It reads the cells of its group from the device histogram, maps them through the same integer intensity and colour tables (copied to the device after each `setIntensity`), blends the graticule pixels of the row, packs the v210 group and the key words, and appends the group with its offset to a compacted list (a prefix sum over a "has trace" flag). Rows without a trace write nothing.
   - The bin totals of the measurements are a reduction per bin in the same kernel.
4. **Download.** The compacted trace groups and the bin totals come back to the host: tens of kB to a few MB, not the picture. The writer patches them into the output grain as today; the output never needs device memory.

## Byte-exactness

- Same tables, same integer formulas, same rounding: every lookup table is built once on the CPU and copied, never recomputed on the device.
- Test: `test_cuda.cpp`, built only with CUDA. Random, edge and scene frames, every mode, pixel and line step, 16- and 32-bit cells. The counts, bin totals, gamut count and trace groups must equal the scalar reference byte for byte (as the SIMD tests do now). It exits 77 without a GPU, like `mxl-mv-cuda-alarm-check`.
- The golden hashes hold, since the still picture and the texts stay on the CPU.

## Expected gain

- On the lab class the CPU spends 2–9 ms per update analysing and 2–11 ms tracing (`docs/performance.md`). On a time-sliced A4000 the analyse and trace kernels should take well under 1 ms each at 1080p. The process would drop to the scheduler, the writer and the preview, about 0.1 core.
- The upload of a 1080p grain (5.5 MB) costs about 1 ms of PCIe time per analysed grain, so `accumulate=all` at 50p uploads every grain.
- Worth it for many scopes per host, or for UHD inputs every frame. Not needed for the platform's one 1080p scope at 25 Hz (0.23 core).

## Open points

- The platform's GPU slots are time-sliced (4 per GPU). A scope would hold one slot for a small load; sharing a CUDA context with the preview's NVENC session in the same process keeps it to one.
- `SCOPES_BACKEND=auto` would pick CUDA only when a device and a context exist, and log the fallback otherwise (as mxl-multiviewer `MV_BACKEND`).

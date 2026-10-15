# Changelog

## Unreleased (M0, done 2026-10-15)

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

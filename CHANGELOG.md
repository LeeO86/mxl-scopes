# Changelog

## Unreleased (M0)

- Luma waveform engine: scalar reference and AVX2 analyse step, bit-exact; integer intensity and colour tables; graticule in mV, % and code, limited and full range; caption and state texts; packed v210 output written line by line.
- MXL input: domain scan, the routed flow analysed in place in the ring with a torn-read check, `waiting`, `running`, `holding`, `no_signal`, `format_mismatch`, re-open on flow re-creation.
- MXL output: own domain (BCP-007-03 `domain_def.json`), TAI-paced writer from a triple buffer, output rate `input` or fixed; a new rate mints a new flow id.
- Update rate `frame` or 1–60 Hz on TAI ticks; line and pixel steps.
- NMOS node: one receiver, one sender, UUIDv5 ids, route persistence and restore into IS-05 and IS-04.
- REST: info, status, input, output, scope (GET, PATCH), snapshot PNG, config with export, import and `KEY=value`; `/livez`, `/readyz`, `/metrics`; WebSocket events.
- Web page: snapshot with live controls, input, NMOS, status and settings tabs.
- `mxl-scopes-bench`, `mxl-scopes-writer`, `mxl-scopes-sample`; unit and integration tests; CI and the container image.

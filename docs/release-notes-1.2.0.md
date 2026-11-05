# mxl-scopes 1.2.0

Every input grain is read again, a skipped grain is counted, and the graticule lines can be thicker. Ports, NMOS resources and the other settings are unchanged from 1.1.0.

## Input reader

- **Why.** When a wait for the next grain ran out, MXL answers "too early", not "timeout". The reader took that for an answer that comes back at once and paused 10 ms, at every update tick. With the update after it, the next grain was often missed and the reader jumped to the newest one without counting the gap. In the field `input_grains_total` grew at 47.9 instead of 50 per second (1080p50, 25 Hz updates); in the lab the RGB parade read 33 and the RGB overlay 25 of 50.
- **What changed.** No pause after "too early" (other unexpected answers still pause). In the lab every mode now reads 50.1–50.2 grains per second at 1080p50.
- **New metric** `mxl_scopes_input_grains_skipped_total`: grains the reader jumps over when it is behind. Read plus skipped is the source's rate. `accumulate_dropped_total` is now counted once per gap (it could count a gap twice).

## Graticule width

- New setting `SCOPES_GRATICULE_WIDTH` (1–4 px, default 1, live): `graticule_width` in `GET`/`PATCH /api/v1/scope`, a slider in the Scope tab. The default draws exactly the same pixels as 1.1.0.

## What changes for the platform

- In-place upgrade; no setting must change. The new setting is optional (catalog: integer 1–4, default 1).
- Dashboards: add `input_grains_skipped_total` next to `input_grains_total`; after the upgrade read plus skipped should be about the source's rate.

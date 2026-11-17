# mxl-scopes 1.3.0

A vectorscope, a picture-colour trace, brighter traces on real pictures, intensity up to 1000 %, and the graticule behind the trace. Ports, NMOS resources, metrics and the existing settings are unchanged from 1.2.1.

## Graticule behind the trace

- **Why.** The graticule was blended over the trace. A black picture lies on the 0 % line, so its trace turned grey there and could not be seen.
- **What changed.** The graticule is drawn behind the trace. A trace on a line keeps its colour; a faint trace still lets the line show through.

## Vectorscope

- New mode `vectorscope` ("Vectorscope"): the Cb/Cr picture of the colours in a square plot in the middle of the output. With a hue ring, rings, vectors to the six colours (R, Mg, B, Cy, G, Yl), the skin-tone line and BT.709 target boxes at 75 % and 100 %.
- New setting `SCOPES_VECTOR_ZOOM`: 1, 2 or 5 (default 1, live). `vector_zoom` in `GET`/`PATCH /api/v1/scope`, a select in the Scope tab. The zoom shows as "2×" right of the plot.
- The graticule level and width apply. The key output, the NMOS `scope-mode` tag, the UI and the widget work as in the other modes.
- No measurements in this mode: every measurement is null and its metric is not exported.

## Picture colour

- New trace colour `picture` ("picture colour") for `luma` and `vectorscope`: each trace point shows the colour of the picture samples in it, so you can read which colour sits where. Its brightness still comes from the intensity.
- It costs more CPU than green, white or amber (those keep their speed). On a real 1080p50 flow at 25 updates per second: luma 0.45 core (green 0.27), vectorscope 0.23 core (green 0.19). Details in `docs/performance.md` ("1.3.0").
- At 2160p the luma picture colour fits 25 Hz updates, not every frame. The vectorscope at 2160p every frame needs line step 2.

## Brighter traces

- **Why.** On a real picture most trace points get only a few hits, and 80 % of them sat at the minimum brightness (about 9 %).
- **What changed.** A logarithmic intensity curve: one hit is about 21 % bright, ten hits about 62 %. Colour bars and flat fields look the same as before.
- `SCOPES_INTENSITY` now goes up to 1000 % (was 400 %), also in `PATCH /api/v1/scope` and the Scope tab slider.

## What changes for the platform

- In-place upgrade; no setting must change. The output looks brighter on real pictures.
- Catalog: mode `vectorscope` and trace colour `picture` are new values; `SCOPES_VECTOR_ZOOM` is new (integer, one of 1, 2, 5, default 1); `SCOPES_INTENSITY` maximum is 1000.
- Dashboards: in `vectorscope` mode the measurement metrics are not exported (no data, not an error).

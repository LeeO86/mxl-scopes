# Agent notes

- Read `SPECIFICATION.md` and `IMPLEMENTATION_PLAN.md` before changing behaviour. Record every deviation in the plan, numbered.
- C++20, CMake, no GStreamer in the media path. One process, one video input.
- MXL pin is `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` with `-DMXL_ENABLE_FABRICS_OFI=OFF`. One `MXL_REF` in `docker/Dockerfile` and `.github/workflows/ci.yaml`, and `SCOPES_MXL_REVISION` in `src/version.hpp`.
- nmos-cpp pin matches the siblings: `fe303849527394b03bdedc8f161f377fe458bb62`.
- `scopes-core` links neither libmxl nor nmos-cpp: the unit tests and `mxl-scopes-bench` build and run anywhere. `mxl-scopes` and the test tools need libmxl; `tests/integration/scope.sh` needs the nmos-enabled binary.
- Every engine path is bit-exact with the scalar reference: integer counts, integer tables, the same packed bytes. A change to the picture changes the golden hashes in `tests/unit/test_engine.cpp`; regenerate the PNGs with `mxl-scopes-bench --golden docs/golden` and say why in the commit.
- The SIMD code lives in `src/scope/cpu/avx2.cpp` (`-mavx2 -mfma`) and `src/scope/cpu/avx512.cpp` (`-mavx512f`) only; the rest stays baseline x86-64. No gathers or scatters. AVX2 stays first-class: the platform's small nodes have no AVX-512.
- Do not write into a mirror domain (`x-mxl-fabrics-agent.mirror`).
- Config precedence is environment > `SCOPES_CONFIG_FILE` (else `<SCOPES_STATE_DIR>/config.json`) > defaults. Invalid config exits 78; a port or another startup failure 75; SIGTERM 143.
- Metrics prefix `mxl_scopes_`. Ports 8170 (web) and 3322 (+1) NMOS.
- Public repository: no internal host names, addresses or organisation references.

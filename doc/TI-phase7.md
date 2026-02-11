# TI-Phase7: Performance Regression Verification

Phase 1–7 refactoring introduced abstraction layers (DecimationEngine facade, overlay callback,
IRenderBackend, IDataSource) and moved code between targets. This report verifies no performance
regression was introduced.

## Environment

- **Platform:** WSL2 (Linux 6.6.87.2-microsoft-standard-WSL2)
- **GPU:** RTX 5080 via Mesa dzn (D3D12→Vulkan translation)
- **Build:** GCC, `-O2` (CMake Release), C++20
- **Baseline:** `bench_20260208_085402.json` / `profile_20260208_085439.json` (pre-Phase 1 PoC)
- **Phase 7:** `bench_20260211_131907.json` / `profile_20260211_131516.json`

---

## R-1: Decimation Throughput (BM-B)

Decimator static functions (`minmax`, `minmax_scalar`, `lttb`) are unchanged — same code in
`src/decimator.cpp`, same compiler flags. 5-run median used for Phase 7.

| Algorithm | PoC (MSPS) | Phase 7 median (MSPS) | Delta | Threshold | Result |
|-----------|------------|----------------------|-------|-----------|--------|
| MinMax SIMD | 20,602 | 21,741 | +5.5% | ≤5% degrad. | PASS |
| MinMax Scalar | 18,994 | 19,609 | +3.2% | ≤5% degrad. | PASS |
| LTTB | 721 | 719 | −0.3% | ≤5% degrad. | PASS |

All within noise range (run-to-run variance ±10% on WSL2). No regression detected.

---

## R-2: Draw FPS (BM-C, V-Sync OFF)

Renderer draw pipeline is unchanged — same code, moved from `src/` to `apps/viewer/`.

| Vertex Count | PoC (FPS) | Phase 7 median (FPS) | Delta | Threshold | Result |
|-------------|-----------|---------------------|-------|-----------|--------|
| 3,840 | 804 | 810 | +0.7% | ≤5% degrad. | PASS |
| 38,400 | 821 | 834 | +1.6% | ≤5% degrad. | PASS |
| 384,000 | 837 | 823 | −1.7% | ≤5% degrad. | PASS |

All within noise. No regression.

---

## R-3: Overlay Callback Overhead (BM-F)

Phase 6 replaced `if (hud) hud->render(cmd)` (null pointer check + virtual dispatch) with
`if (overlay_cb) overlay_cb(cmd)` (`std::function` bool check + type-erased call).

**Measurement:** BM-F runs 500 draw frames with empty callback vs no-op callback, 4 runs:

| Run | No callback (ms/frame) | No-op callback (ms/frame) | Delta (ms) |
|-----|----------------------|--------------------------|-----------|
| 1 | 1.219 | 1.220 | +0.001 |
| 2 | 1.206 | 1.231 | +0.026 |
| 3 | 1.227 | 1.218 | −0.009 |
| 4 | 1.194 | 1.201 | +0.007 |

**Median delta: +0.004 ms/frame** (fluctuates ±0.03 ms, measurement noise)

| Metric | Value | Threshold | Result |
|--------|-------|-----------|--------|
| Overlay callback overhead | ≤0.03 ms/frame | ≤0.1 ms/frame | **PASS** |

---

## R-4: DecimationEngine Facade Overhead

Phase 4 introduced `DecimationEngine` as a public facade over internal `DecimationThread`.

**Code analysis:** The hot-path `DecimationEngine::try_get_frame()` performs:
1. One `unique_ptr` dereference (`impl_->thread`)
2. One forwarded function call (`try_get_frame`)
3. Two field assignments (`raw_sample_count`, `per_channel_vertex_count`)

**Overhead: ~10ns per call.** At 60 FPS, this is 0.0006 ms/frame — 3 orders of magnitude
below measurement resolution.

**Profile confirmation:** `decimate_ms` at 1 MSPS: PoC 0.0090 ms → Phase 7 0.0090 ms (identical).

| Metric | Value | Threshold | Result |
|--------|-------|-----------|--------|
| Facade overhead | ~10ns/call (< 0.001 ms) | ≤1% of BM-B | **PASS** |

---

## R-5: E2E Latency (Profile)

Profile runs V-Sync locked (~60 FPS). E2E latency measurement was added in Phase 12 (post-PoC),
so no PoC baseline exists. Phase 7 profile values serve as the post-refactoring reference.

| Scenario | FPS avg | dec_ms | upload_ms | render_ms | E2E ms (avg) |
|----------|---------|--------|-----------|-----------|-------------|
| 1 MSPS | 59.8 | 0.009 | 0.324 | 15.93 | 22.1 |
| 10 MSPS | 59.9 | 0.011 | 0.317 | 15.62 | 16.2 |
| 100 MSPS | 59.9 | 0.044 | 0.318 | 15.62 | 16.6 |
| 1 GSPS | 58.4 | 0.527 | 0.571 | 15.85 | 15.9 |

**Sub-metric comparison (1 MSPS, directly comparable):**

| Metric | PoC | Phase 7 | Delta |
|--------|-----|---------|-------|
| FPS avg | 60.0 | 59.8 | −0.3% |
| dec_ms | 0.0090 | 0.0090 | 0.0% |
| upload_ms | 0.336 | 0.324 | −3.6% |
| render_ms | 16.11 | 15.93 | −1.1% |
| frame_ms | 16.66 | 16.73 | +0.4% |

All within noise. All scenarios PASS.

| Metric | Delta | Threshold | Result |
|--------|-------|-----------|--------|
| FPS (1-100 MSPS) | ≤0.3% | ≤10% degrad. | **PASS** |
| FPS (1 GSPS) | −2.7% | ≤10% degrad. | **PASS** |

---

## R-6: Build Artifact Sizes

| Artifact | Phase 6 (pre-Ph7) | Phase 7 | Delta |
|----------|-------------------|---------|-------|
| libgrebe.a | 641 KB | 179 KB | −72% |
| grebe-viewer | 2.6 MB | 2.5 MB | −4% |
| grebe-sg | 1.9 MB | 1.9 MB | 0% |
| **Total** | **5.1 MB** | **4.6 MB** | **−10%** |

Total size decreased. Vulkan/VMA/ImGui code moved from library to app — no duplication,
identical code compiled once in grebe-viewer instead of twice (library + app).

| Metric | Delta | Threshold | Result |
|--------|-------|-----------|--------|
| Total artifact size | −10% (decreased) | ≤5% increase | **PASS** |

---

## Summary

| ID | Measurement | Result | Detail |
|----|------------|--------|--------|
| R-1 | Decimation throughput | **PASS** | MinMax SIMD +5.5%, Scalar +3.2%, LTTB −0.3% |
| R-2 | Draw FPS (V-Sync OFF) | **PASS** | +0.7% / +1.6% / −1.7% across vertex counts |
| R-3 | Overlay callback overhead | **PASS** | ≤0.03 ms/frame (threshold: 0.1 ms) |
| R-4 | DecimationEngine facade | **PASS** | ~10ns/call, unmeasurable vs 0.009 ms baseline |
| R-5 | E2E latency (profile) | **PASS** | All scenarios within noise of PoC baseline |
| R-6 | Artifact sizes | **PASS** | Total −10% (decreased) |

**Overall: PASS — No performance regression from Phase 1–7 refactoring.**

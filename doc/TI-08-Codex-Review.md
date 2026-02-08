# TI-08/TI-09 Codex Review (Revised with Trigger/Timing Assurance)

Date: 2026-02-08  
Reviewed baseline: `origin/master` at `5cad5e4` (`Fix doc inconsistencies and add header versioning task`)

## Scope
- Re-reviewed `doc/technical_judgment.md` (TI-08 + TI-09).
- Refreshed recommendations based on latest conclusions and tracker/TODO updates.

## What Changed Since Previous Review

The previously raised three concerns are now reflected in `master`:

1. R-11 tracker wording consistency
- Updated to align TI-08 and TI-09: viewer-side bottleneck was resolved in Phase 10-3, while IPC transport can still limit SG-side drops in IPC mode.

2. TI-09 quality conclusion wording
- Strengthened with proper caveat: current PoC metrics show no significant degradation, but waveform fidelity metrics are still an open product-phase task.

3. IPC header compatibility/versioning follow-up
- Added as an explicit TODO item (Phase 12) for `FrameHeaderV2` compatibility/versioning policy and robust parsing rules.

## Current Technical Position (as of master)

1. Viewer pipeline objective is achieved.
- Embedded 4ch/8ch × 1GSPS reached 0-drops after Phase 10-3 consumer-side fixes.

2. SG-side drops in IPC mode are real and quantified.
- TI-09 telemetry now propagates `sg_drops_total` through IPC and profiler/HUD, enabling viewer-drop vs SG-drop separation.

3. For current PoC acceptance, SG-side drops are treated as acceptable.
- Rationale: core PoC goal (high-rate rendering path validation) is demonstrated on embedded reference path.
- IPC SG-side loss is documented as a transport-bandwidth behavior, not hidden.

## Revised Recommendations

Priority 1: Keep embedded path as canonical performance baseline.
- Continue to evaluate rendering/decimation limits primarily on embedded mode.

Priority 2: Keep IPC quality transparent with end-to-end telemetry.
- Maintain separate reporting for viewer-side drops and SG-side drops in all profile artifacts.

Priority 3: Execute low-risk IPC robustness work before any shm decision.
- Follow Phase 12 items first:
  - `header_crc32c` verification
  - `FrameHeaderV2` compatibility/versioning policy (`header_bytes`-aware parsing, explicit mismatch handling)
  - long-run stability checks

Priority 4: If IPC quality needs to improve, prefer non-shm mitigations first.
- Backpressure / adaptive rate limiting
- Copy-path reduction
- Optional SG-side pre-decimation only when product requirements justify added design complexity

Priority 5: Add trigger/timing assurance as the next quality-control layer.
- Add SG-side trigger modes (internal/external/timer fallback) as capture authority.
- Add Main-side frame validity gating (sequence/CRC/window coverage).
- Add trigger-aware quality metrics to profile reports.

Priority 6: Keep shm as trigger-based escalation, not default next step.
- Reconsider only when measured requirements exceed what pipe + flow control + robustness improvements can satisfy.

## Suggested Validation Matrix (next)

1. 4ch×1G IPC (`block=16K`, `64K`) with SG-drop and viewer-drop separation.
2. 8ch×1G IPC same settings, compare SG-drop ratio stability.
3. Trigger mode comparison (`internal`, `external` stub, `timer`) with fixed capture windows.
4. Repeat after each Phase 12 hardening item.
5. Repeat after Phase 14 trigger/validity implementation.

Success criterion:
- No regression in viewer-side 0-drops targets, and improved/controlled SG-side drop behavior with explicit observability.

## Waveform Quality and Integrity (added)

Current status:
- TI-09 appropriately states that current PoC metrics do not show significant degradation, while also acknowledging that this is not a full waveform-fidelity proof.

Why this matters:
- Constant vertex count and acceptable FPS are necessary, but not sufficient, to guarantee waveform integrity.
- SG-side drops can shorten effective time window and may hide narrow peaks/high-frequency details depending on signal characteristics.

Recommended quality validation gates:
1. Embedded-reference envelope comparison.
- Compare IPC output vs embedded output over the same scenario/time window.
- Track min/max envelope mismatch rate per frame.

2. Peak integrity metrics.
- Track peak miss rate (events present in embedded reference but absent in IPC path).
- Track extremum amplitude error distribution (p50/p95/p99).

3. Time-window coverage metric.
- Report effective covered sample window ratio (IPC vs embedded baseline).
- Add threshold per use case (e.g., warning when coverage drops below agreed limit).

Decision rule:
- If integrity metrics remain within agreed thresholds, SG-side drops remain acceptable for current scope.
- If thresholds are violated, prioritize non-shm mitigations first (flow control/rate matching/copy-path reductions), then reconsider shm only with evidence.

## Trigger/Timing Assurance Impact Review

The new trigger/timing assurance features (as added in RDD + TODO Phase 14) are a strong mitigation for current observed risks.

Current issue -> mitigation effect:
1. SG-side drops can silently alter effective time window.
- Mitigated by capture-window semantics + window coverage tracking.
- Invalid/incomplete windows become explicit instead of implicit quality loss.

2. FPS/vertex count alone cannot prove waveform integrity.
- Mitigated by trigger-aligned frame assembly and validity gating.
- Adds deterministic sampling windows for fair IPC vs embedded fidelity comparison.

3. Potentially corrupted or discontinuous frames can look “normal.”
- Mitigated by frame validity checks (sequence continuity, CRC, coverage) and explicit invalid-frame reporting.

4. Future external device ingest (PCIe/USB3) may introduce clock/domain mismatch.
- Mitigated by SG-side trigger authority and external-trigger support path.
- Provides a clear synchronization contract before adding device-specific transports.

Net effect:
- These features do not remove IPC bandwidth limits directly.
- They significantly reduce quality-assurance ambiguity and prevent silent data-integrity regressions.
- They derisk future development by enforcing explicit timing/validity contracts early.

## Derisking Strategy for Future Development

1. Implement Phase 12 IPC hardening first (header validation/versioning, long-run robustness).
2. Implement Phase 14 trigger/validity pipeline with timer fallback always available.
3. Gate acceptance on both performance and integrity:
- Performance: FPS, viewer drops, SG drops.
- Integrity: valid frame ratio, window coverage, envelope mismatch, peak miss rate.
4. Only after these gates are stable, evaluate whether transport escalation (including shm) is still needed.

## Architecture Direction (unchanged, still recommended)

- Canonical data plane: single-process embedded path for core performance truth.
- IPC: adapter path for process separation and integration testing.
- Source boundary: pluggable `DataSource` abstraction (`Synthetic` / future `PCIe` / future `USB3`) feeding a shared downstream pipeline.
- Decision discipline: move to shm only with concrete evidence that lower-risk options cannot meet target requirements.

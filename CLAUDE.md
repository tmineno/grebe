# CLAUDE.md — Development Conventions

## Core Principles

### 1. Runnable-First

Every commit must leave the system in a launchable, functional state. "Compiles but doesn't run" is not acceptable.

- Large changes are delivered incrementally using stubs or interim implementations to maintain runnability at each step.
- Structural refactors (file moves, renames, directory reorganization) are committed separately from logic changes — never mixed.

### 2. Measurement-Driven Development

Performance-sensitive changes require quantitative before/after comparison using the project's existing benchmarking and profiling infrastructure.

- Record baseline metrics before starting work.
- Re-measure under identical conditions after implementation.
- Document the delta in commit messages or the technical judgment log.
- Regressions require explicit justification as intentional trade-offs; otherwise, do not merge.

### 3. Acceptance Criteria Before Implementation

Define 1–3 concrete completion conditions before starting each unit of work. Conditions should be observable (a command you can run, a log line you can check, a metric you can compare).

### 4. Commit Granularity

| Change type | Granularity rule |
|---|---|
| Feature / logic change | One feature = one commit |
| Refactor (move / rename) | Separate commit, no logic changes mixed in |
| Instrumentation / metrics | May co-exist with the logic it measures |

### 5. Interface Stability

When the project defines abstract interfaces or protocol contracts shared across process or module boundaries:

- Update both sides of the interface in the same commit.
- State the reason and impact scope of any breaking change.
- Verify that all existing operational modes remain functional after the change.

### 6. Two-Layer Verification

| Layer | Method | When |
|---|---|---|
| Automated | Built-in benchmarks, profiling, correctness checks | On every performance-relevant change |
| Manual E2E | Launch all operational modes and confirm end-to-end function | At every milestone boundary |

Prefer extending existing automated checks (correctness assertions inside benchmarks) over introducing heavyweight test frameworks — especially in proof-of-concept or exploratory projects.

### 7. Documentation Synchronization

- **Spec / requirements**: Update at milestone boundaries.
- **Task tracking (TODO)**: Mark completion at commit time.
- **Technical investigation log**: Append measurement results immediately when available.

Resolve any code–documentation drift before starting the next milestone.

### 8. Cross-Environment Verification

- Daily development may target a single primary environment.
- Milestone boundaries require verification on all supported platforms.
- Changes touching OS-specific APIs require cross-platform verification at commit time.

## Workflow Summary

```
1. Define acceptance criteria (1–3 lines)
2. Capture baseline metrics
3. Implement (runnable at every commit)
4. Re-measure, record delta
5. Confirm all operational modes pass E2E
6. Update docs, mark tasks complete
```

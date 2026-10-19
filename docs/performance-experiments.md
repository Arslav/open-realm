# Optimization Experiment Retention

## Contract

The project owner's instruction of October 18, 2026 is to preserve optimizations
even when they show no immediate gain. A different device, scene, renderer path
or later optimization may change their value. Negative measurements are useful
evidence, not a reason to destroy the implementation.

The owner's subsequent instruction is to enable good optimizations. After
correctness/quality checks and representative measurements establish a benefit,
enable the optimization by default on the measured target platforms without
asking separately. Keep the baseline toggle and results. A positive noisy
sample alone does not establish a benefit, and unmeasured platforms retain
their existing defaults. The separate rule requiring an explicit request for
ordinary/distribution builds remains in force.

Before removing an experimental implementation from the active path, retain
one of these complete forms:

- A tested runtime/build toggle with a documented default and working on/off
  paths. Keep experiments disabled by default when their benefit is unproven
  or they regress the target workload.
- A named Git branch containing implementation and tests, with its exact base
  and tip recorded in the experiment index. A local unreferenced commit is not
  sufficient retention.
- An indexed patch or source archive outside the Git repository, including its baseline,
  dependencies, fixtures/tests and build/reproduction recipe. Preserve any
  pre-existing dirty changes on which the experiment depends; a patch against
  an unidentified working tree is incomplete.

Do not keep partially reverted wiring or correctness defects enabled merely
to retain code. A failing or obsolete variant may live in an isolated archive,
with the defect and missing verification stated explicitly. Preserve the
implementation before removing its active entry points.

## Required Record

For each experiment record:

- What it changes, where its code lives and how to enable or reconstruct it.
- Exact base revision or source hashes, dependencies and relevant defaults.
- Device/backend, scene, camera, resolution, quality, warmup and sample method.
- All completed comparison windows, including regressions and no-gain results.
- Correctness tests, known gaps and reasons a future retest could differ.

Use the owning subsystem document for conclusions and an archive index for
source locations. Distinguish a complete restorable experiment from partial
historical evidence; do not claim missing old variants are reproducible.

## Cleanup Boundary

Old APKs, executables, objects, intermediate packages and redundant binary
caches may be removed when no longer needed. Keep the current working build
and the control needed for an active comparison. Preserve source archives,
patches, recipes, tests, reports and raw benchmark data. SDK/NDK, emulators,
retail archives, saves, user backups and signing keys are not old builds.

Before deleting old build products, inventory them and record paths/hashes.
Never use a broad cleanup that also removes the only experiment source copy.
`build/` is disposable; it cannot be the sole long-term home of an experiment.

## Existing Warcraft III Evidence

The Warcraft III optimization archive (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/README.md`, external archive)
indexes source snapshots/patches recovered from earlier local investigations.
Those historical files retain their original paths in the archive. Recipes
may still refer to the original `build/android/optimization/` layout and need
that layout restored before use. Their original summaries and raw profiles
remain in the local diagnostic tree; source presence alone does not establish
that every older experiment has a complete recipe or valid performance result.

See [WC3 performance](games/warcraft-3/performance.md) for measured outcomes and
[Android workflow](android.md) for device captures and state restoration.

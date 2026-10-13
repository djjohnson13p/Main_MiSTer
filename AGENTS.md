# Independent MiSTer fork development

This is the user's experimental fork of `MiSTer-devel/Main_MiSTer` at `djjohnson13p/Main_MiSTer`. Work on `independent/main`. Preserve upstream history, copyright notices and GPL licensing. Disclose AI assistance and label experimental builds clearly.

Do not create upstream pull requests, issues, reviews, comments or messages. Fetch upstream read-only. Publish source and build artifacts only to the user's fork when they have passed the relevant checks. Local upstream push is disabled deliberately.

Verify each issue against current source and comments before changing code. An open issue can already be fixed, concern another repository or express a preference. Record those distinctions. Prefer small changes with a demonstrated failing case and a regression check that exercises production code. Do not claim runtime performance improvements without measurements.

Build the unmodified upstream revision before the modified binary, using the same toolchain and build settings. Retain logs and hashes outside the source checkout. Keep host-test results, ARM compilation, hardware checks and community tests as separate evidence levels.

Use `make` to build. The inherited `build.sh` kills a running MiSTer process and uploads over FTP; do not use it for local compilation or CI. Do not automatically deploy experimental Main, flash media, change FPGA cores, or modify user ROMs, saves, configuration or the prior rebuild workspace.

Maintain `docs/fork-status.md` with changes, validation, limitations and the next concrete work. The persistent goal remains active while useful independent work remains. Hardware-dependent findings need a test plan and actual hardware evidence before a stability claim.

Track upstream using `docs/upstream-review.md`. Compare overlapping fixes by behavior and evidence, and integrate useful upstream work after validation. Keep an explicit review ledger; seeing a new commit does not mark it reviewed. Daily heartbeat `review-mister-upstream` is lower priority than active improvements and stays quiet while upstream is unchanged or non-actionable.

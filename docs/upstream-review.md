# MiSTer upstream review and integration

The fork follows the official `MiSTer-devel/Main_MiSTer` master branch. Upstream review runs during relevant development work and through a daily check at 7 AM America/Los_Angeles. An unchanged upstream revision needs no repeat review or notification. Security, compatibility and regressions take priority; unrelated work can wait behind active improvements.

## Review each new revision

From the repository, run `python scripts/upstream_review.py --fetch --output ../Evidence/upstream-review-current.json`. The report includes unreviewed commits, divergence, affected paths and patch-equivalence evidence. Ordinary checks never advance the review ledger or modify source. The default ledger is private Git metadata; reports can also use a directory outside the checkout.

After deciding how to handle the fetched revision, use `--fetch --ack` with its exact full SHA and `--note` with the decision and validation evidence. The tool rejects a changed tip, abbreviated SHA or empty note. An acknowledgement records review, including a justified deferral; it does not claim that deferred code has been integrated. Rewritten upstream history remains visible for review.

Fetch upstream read-only. Record the upstream tip, last reviewed revision, fork tip and common ancestor. Inspect new commits, changed files, related issues, existing tests and release implications. File overlap and patch equivalence help route the review; neither establishes which implementation behaves better. Inspect earlier fixes to avoid reintroducing regressions or duplicating resolved work.

For every actionable upstream change, record one decision with the compared revisions, reason and checks:

| Decision | When to use it |
| --- | --- |
| Integrate upstream | The change adds useful behavior or fixes a demonstrated defect without losing validated fork behavior. |
| Replace our implementation | Upstream handles the same problem more completely or safely. Retain the regression test and prove it passes the replacement. |
| Combine implementations | Each covers different valid cases. Resolve the overlap and test both sets of behavior. |
| Retain our implementation | Evidence shows the upstream alternative would regress supported behavior. Record the failing case and reconsider when evidence changes. |
| Defer | Scope, dependencies or hardware evidence are insufficient. Record what is needed next. |

Credit the original authors and preserve upstream commit history where practical. Correctness, compatibility, maintainability and measured cost decide the result, regardless of who wrote the change or whether AI assisted it.

## Validate integration

Use an isolated review branch or checkout. Preserve ongoing work and candidate checkpoints. Merge upstream or apply a selected upstream commit only after inspecting the actual change and dependencies. Resolve conflicts by comparing behavior, not by accepting one side wholesale.

Build the new official baseline and integrated fork with the same toolchain. Run affected regressions and the fork's existing suites; use actual hardware checks for hardware-dependent claims. Publish validated source to our own development branch and retain rollback revisions. Keep unvalidated integrations isolated. Record build failures and deferred commits rather than hiding them or marking them reviewed prematurely.

Record the accepted official baseline in `upstream-base.txt`. CI fetches that exact commit from the official repository and builds it before the fork, and candidate archives retain the baseline identifier. Update this record along with an integration so comparisons follow the official version being adopted.

The development objective is to preserve useful upstream improvements while adding validated fork improvements. That objective does not justify claiming compatibility or speed beyond the tests and measurements performed. CI and physical testing remain distinct evidence levels.

The monitor never opens upstream PRs, issues, reviews or messages, and it never automatically installs experimental software on hardware. The user decides when to invite community testing.

## Accepted integration history

| Official revision | Fork candidate | Decision and evidence limits |
| --- | --- | --- |
| `00bdff0d5b985a3a929978f86f15d930954451a2` | `-x17` | Import all three source files unchanged and preserve the upstream parent commit. The A2065/IDE acknowledgement cycle is relevant and independent of our modified paths. Production extraction tests and matched pristine/fork builds check software behavior; upstream-reported hardware results are not this fork's hardware validation. CI baseline advances to this exact revision. |

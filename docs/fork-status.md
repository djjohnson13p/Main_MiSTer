# Independent MiSTer Main development

This fork develops fixes and features for the MiSTer Main ARM software. It is an experimental, AI-assisted project owned by djjohnson13p, with no upstream pull requests planned. FPGA cores, the Linux image and Downloader remain separate projects.

The starting upstream revision is `2dd3662cccd45d54d3d383bf9f7d80f6f57e6d03`, fetched October 13, 2026. Development uses `independent/main`; `master` preserves the original fork baseline. The current candidate identifies itself with `-x2`; the earlier `-x1` candidate and its source commit remain available. `upstream-base.txt` records the accepted official baseline.

## Reviewed changes

| Change | Evidence and status |
| --- | --- |
| Restore VRR after HDMI initialization | [Upstream issue 1226](https://github.com/MiSTer-devel/Main_MiSTer/issues/1226). HDMI initialization clears packet enables; invalidate the cached VRR mode so the next mode setup restores the packets. Regression harness uses production video functions with simulated I2C. Physical display validation is pending. |
| Optional 12 hour Menu clock | [Upstream request 990](https://github.com/MiSTer-devel/Main_MiSTer/issues/990). Set `osd_clock_12h=1` in MiSTer.ini for AM/PM; the default stays 24 hour. Both formats fit the existing 32-column row. |
| Correct browser comparisons and ZIP folder paths | Duplicate parent entries now compare equivalent, as required by `std::sort`. Implicit ZIP folders require a full path prefix, so `outer/target` cannot falsely satisfy a request for root `target`. Real ZIP regressions preserve existing directory/ZIP ordering. |
| Preserve files when paging onto an expanded name | With `lookahead=0` and `browse_expand=1`, an expanded destination at the bottom row could advance the rendered viewport past an unseen file. Forward paging now reserves one row for either the current or relevant destination expansion, without deducting twice. The last item retains its full step. Production paging and rendering tests cover both directions and 8/16-row layouts. |
| Avoid unchanged SPD writes during ordinary HDMI popups | Menu transitions only request SPD updates when Direct Video or Minimig RTG metadata contains OSD state. Host tracing shows 68 writes and two reads removed per ordinary-HDMI popup entry/exit with default SPD settings. Forced refresh, resolution changes, HDMI reset and DV/RTG updates remain covered. Hardware latency is unmeasured. |

## Issue triage

| Report | Finding |
| --- | --- |
| [364](https://github.com/MiSTer-devel/Main_MiSTer/issues/364), deleted recent file or MRA parent | The deleted file fix and the later parent recovery are already in current upstream; parent recovery was merged in [1202](https://github.com/MiSTer-devel/Main_MiSTer/pull/1202). No duplicate patch. |
| [950](https://github.com/MiSTer-devel/Main_MiSTer/issues/950), ZIP ordering | Mixed directories and ZIP-as-directory ordering is an intentional upstream preference. That behavior is retained. The separate comparator defect above is not a claim to resolve this report. |
| [1013](https://github.com/MiSTer-devel/Main_MiSTer/issues/1013), zero MRA map | Current loader contains explicit handling; an open report alone is insufficient to justify another patch. |
| Wi-Fi reports 1302 and 1319 | Driver and kernel scope. A Main patch cannot supply missing Linux drivers. |
| [1326](https://github.com/MiSTer-devel/Main_MiSTer/issues/1326), missing NumLock | Existing remapping runs before emulation dispatch. The replacement key must be pressed first on the short keyboard, then NumLock on the borrowed keyboard. Test this procedure before adding another input setting. |
| [1251](https://github.com/MiSTer-devel/Main_MiSTer/issues/1251), lookahead hides rows | The original 60-long-filename case is fixed in inherited [1256](https://github.com/MiSTer-devel/Main_MiSTer/pull/1256). Further production-renderer tests exposed a separate residual case with lookahead disabled: 18 files, eight rows, and only index 16 expanded. The old pages were 0..7, 1..8, 10..16, skipping index 9. This fork corrects that case. |
| [1127](https://github.com/MiSTer-devel/Main_MiSTer/issues/1127), Info popup spikes | The optimization above removes confirmed redundant software requests. It does not establish the cause of every reported gameplay spike or a measured speedup. |
| [1144](https://github.com/MiSTer-devel/Main_MiSTer/issues/1144), identical USB controller mappings | Current and pre-report code already hash distinct physical ports, including when `Uniq` is empty. Host probes produce distinct map filenames. An earlier `controller_unique_mapping=0` followed by `=1` in the same INI section reproduces bare filenames; replace the zero entry instead of appending. The reporter's effective configuration remains unknown, so no source repair is claimed. |

## Validation and next work

The pristine upstream and earlier x1 fork both compile with Arm GCC 10.2.1, using identical settings and zero ARM warning diagnostics. The x1 candidate is 1,288,228 bytes with SHA256 `6a36103753d9bd9301b42e335328da48bb75c5a9ce8f9acebdcbe0cec0a51e4a`; the baseline is 1,288,236 bytes. Binary size differences are not performance results. Each candidate carries its own build receipt and checksums.

Host coverage includes nine VRR cases, real ZIP and comparator tests, Menu clock/layout tests, 27 SPD cases, 1,765 paging transitions and eight isolated Git review tests. Identical video and browser regressions fail against their pre-fix sources. Host GCC 15 reports an existing ZIP-cache `strncpy` warning. Independent review also caught and corrected clipping of the combined Main/OS version row.

The CI workflow pins the Linux Arm compiler archive by SHA256 and builds the same pristine baseline before the fork. It runs the host regressions and saves an experimental candidate, source archive, commit identifier, hashes and logs. [The first hosted run passed](https://github.com/djjohnson13p/Main_MiSTer/actions/runs/38485276379) for source commit `7885a7ed62fc6fe1b55afd3904e851acc5433c0e`.

The [community test procedure](community-testing.md) covers recovery, Menu navigation, paging, Info popups, controller input, core loading, HDMI reconnect and both clock modes. VRR requires compatible FreeSync and VESA displays. Physical device testing and community outreach are pending. Further software work should prioritize another reproducible defect, while comparing new official changes under the policy below.

Use the [upstream review policy](upstream-review.md) to monitor official changes, compare overlapping fixes, and integrate useful upstream work after validation. A quiet daily heartbeat and an explicit review ledger keep that work separate from untested assumptions.

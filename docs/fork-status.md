# Independent MiSTer Main development

This fork develops fixes and features for the MiSTer Main ARM software. It is an experimental, AI-assisted project owned by djjohnson13p, with no upstream pull requests planned. FPGA cores, the Linux image and Downloader remain separate projects.

The starting upstream revision is `2dd3662cccd45d54d3d383bf9f7d80f6f57e6d03`, fetched October 13, 2026. Development uses `independent/main`; `master` preserves the original fork baseline. Experimental binaries identify themselves with the `-x1` version suffix.

## Reviewed changes

| Change | Evidence and status |
| --- | --- |
| Restore VRR after HDMI initialization | [Upstream issue 1226](https://github.com/MiSTer-devel/Main_MiSTer/issues/1226). HDMI initialization clears packet enables; invalidate the cached VRR mode so the next mode setup restores the packets. Regression harness uses production video functions with simulated I2C. Physical display validation is pending. |
| Optional 12 hour Menu clock | [Upstream request 990](https://github.com/MiSTer-devel/Main_MiSTer/issues/990). Set `osd_clock_12h=1` in MiSTer.ini for AM/PM; the default stays 24 hour. Both formats fit the existing 32-column row. |
| Correct browser comparisons and ZIP folder paths | Duplicate parent entries now compare equivalent, as required by `std::sort`. Implicit ZIP folders require a full path prefix, so `outer/target` cannot falsely satisfy a request for root `target`. Real ZIP regressions preserve existing directory/ZIP ordering. |

## Issue triage

| Report | Finding |
| --- | --- |
| [364](https://github.com/MiSTer-devel/Main_MiSTer/issues/364), deleted recent file or MRA parent | The deleted file fix and the later parent recovery are already in current upstream; parent recovery was merged in [1202](https://github.com/MiSTer-devel/Main_MiSTer/pull/1202). No duplicate patch. |
| [950](https://github.com/MiSTer-devel/Main_MiSTer/issues/950), ZIP ordering | Mixed directories and ZIP-as-directory ordering is an intentional upstream preference. That behavior is retained. The separate comparator defect above is not a claim to resolve this report. |
| [1013](https://github.com/MiSTer-devel/Main_MiSTer/issues/1013), zero MRA map | Current loader contains explicit handling; an open report alone is insufficient to justify another patch. |
| Wi-Fi reports 1302 and 1319 | Driver and kernel scope. A Main patch cannot supply missing Linux drivers. |
| [1326](https://github.com/MiSTer-devel/Main_MiSTer/issues/1326), missing NumLock | Existing remapping runs before emulation dispatch. The replacement key must be pressed first on the short keyboard, then NumLock on the borrowed keyboard. Test this procedure before adding another input setting. |

## Validation and next work

The pristine upstream and reviewed fork both compile with Arm GCC 10.2.1, using identical settings. Both full ARM builds complete with zero warning diagnostics. The reviewed candidate is 1,288,228 bytes with SHA256 `6a36103753d9bd9301b42e335328da48bb75c5a9ce8f9acebdcbe0cec0a51e4a`; the baseline is 1,288,236 bytes. The eight-byte size difference is not a performance result.

All three host regression suites pass with GCC 15.3.0: nine VRR cases, real ZIP and comparator tests, and Menu clock/layout tests. Identical video and browser tests fail against the starting upstream source. Host GCC 15 reports an existing ZIP-cache `strncpy` warning; the patched ARM build adds no warning. Independent review caught and corrected clipping of the combined Main/OS version row.

The CI workflow pins the Linux Arm compiler archive by SHA256 and builds the same pristine baseline before the fork. It runs the host regressions and saves an experimental candidate, source archive, commit identifier, hashes and logs. CI execution is tracked separately from the completed local builds.

The [community test procedure](community-testing.md) covers recovery, Menu navigation, controller input, core loading, HDMI reconnect and both clock modes. VRR requires compatible FreeSync and VESA displays. Physical device testing and community outreach are pending. Next software work is to audit the lookahead and input-mapping reports against current source and reproduce any remaining defects before patching them.

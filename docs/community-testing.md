# Experimental MiSTer Main testing

The `-x1` build changes MiSTer Main, the ARM application. It preserves official FPGA cores and Linux. The fork includes AI-assisted development and is not an official MiSTer-devel release. Compilation and host regressions do not establish hardware compatibility.

## Prepare a recoverable test

Use a spare, working MiSTer card with the same official baseline you normally use. Keep your original card, working Main binary and configuration available so you can recover without relying on a network connection. Verify the candidate's SHA256 against its supplied manifest and record `SOURCE_COMMIT.txt`. Do not copy the example MiSTer.ini over your configured file.

Install only the candidate Main executable using the normal Main replacement procedure for your setup. Keep its accompanying source archive and license. Confirm the version includes `-x1` in the System Settings page before testing. No deployment or card writes are performed by the fork's CI.

## Record results

Record the exact source commit, board model, Linux version, core versions, display model and relevant INI settings. Report reproducible steps, observed and expected behavior, and whether the same steps fail with your working official Main. Share only logs you have checked for private information; no ROMs, paid unlock keys or credentials.

| Test | Expected behavior |
| --- | --- |
| Menu and System Settings | Main's experimental suffix and the complete OS version remain readable. Keyboard/controller navigation, file loading and core transitions work. |
| Clock default | With `osd_clock_12h=0` or omitted, the Menu footer keeps the original 24-hour layout. Network, Bluetooth and SDRAM indicators remain visible. |
| Clock option | With `osd_clock_12h=1`, the footer shows a 12-hour time with AM/PM in place of the weekday. Check midnight and noon using the system's existing time controls. |
| File browser | Navigate ordinary folders and ZIP archives, including implicit ZIP folders without explicit directory entries. Parent entries, mixed directory/ZIP ordering and case-insensitive matching remain usable. |
| Recent selection | Existing upstream recovery after a recent file or its parent is removed remains functional. This fork does not claim a new fix for issue 364. |
| VRR reconnect | On a compatible display and VRR core, with `vsync_adjust=0`, test `vrr_mode=2` for FreeSync and `vrr_mode=3` for VESA separately. Confirm the display reports VRR, then reconnect HDMI or switch inputs repeatedly without changing timings. Confirm VRR remains active after logged video reinitialization. |
| Video transitions | Check Menu/core switching, VRR disabled, Direct Video, HDR and Minimig RTG on the hardware that supports each mode. A scaler supplying its own VRR cannot prove MiSTer's packet behavior. |
| Gameplay and persistence | Load representative cartridge and disc cores; exercise normal controller input, saves and existing user configuration. |

## Return to the working build

Restore your known-working Main or switch back to the original card, and verify the version and normal operation. The official updater may replace the experimental Main; record the version before any subsequent tests. Mark unavailable hardware combinations as untested.

## Scope of this candidate

The changes restore VRR packet programming after HDMI reset, anchor inferred ZIP folder paths, correct duplicate parent entry comparisons, and add an optional 12-hour clock. No measured gameplay speedup, Linux driver repair, new FPGA feature or general stability guarantee is claimed. Performance work will require baseline measurements on actual hardware.

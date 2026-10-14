# Experimental MiSTer Main testing

The `-x3` build changes MiSTer Main, the ARM application. It preserves official FPGA cores and Linux. The fork includes AI-assisted development and is not an official MiSTer-devel release. Compilation and host regressions do not establish hardware compatibility.

## Prepare a recoverable test

Use a spare, working MiSTer card with the same official baseline you normally use. Keep your original card, working Main binary and configuration available so you can recover without relying on a network connection. Verify the candidate's SHA256 against its supplied manifest and record `SOURCE_COMMIT.txt`. Do not copy the example MiSTer.ini over your configured file.

Install only the candidate Main executable using the normal Main replacement procedure for your setup. Keep its accompanying source archive and license. Confirm the version includes `-x3` in the System Settings page before testing. No deployment or card writes are performed by the fork's CI.

## Record results

Record the exact source commit, board model, Linux version, core versions, display model and relevant INI settings. Report reproducible steps, observed and expected behavior, and whether the same steps fail with your working official Main. Share only logs you have checked for private information; no ROMs, paid unlock keys or credentials.

| Test | Expected behavior |
| --- | --- |
| Menu and System Settings | Main's experimental suffix and the complete OS version remain readable. Keyboard/controller navigation, file loading and core transitions work. |
| Clock default | With `osd_clock_12h=0` or omitted, the Menu footer keeps the original 24-hour layout. Network, Bluetooth and SDRAM indicators remain visible. |
| Clock option | With `osd_clock_12h=1`, the footer shows a 12-hour time with AM/PM in place of the weekday. Check midnight and noon using the system's existing time controls. |
| File browser | Navigate ordinary folders and ZIP archives, including implicit ZIP folders without explicit directory entries. Parent entries, mixed directory/ZIP ordering and case-insensitive matching remain usable. |
| Expanded-name paging | With `lookahead=0` and `browse_expand=1`, create 18 short-named test files and make the 17th sorted filename longer than 28 characters. On an eight-row browser, page forward and backward: every file must appear, especially the 10th sorted file. Repeat with lookahead enabled, 16 rows, directories and hidden datecodes. Single-item scrolling and selection must work. |
| Info popups | Compare the same repeatable Info popup sequence with official Main on ordinary HDMI. Record observed pauses or measured timing without inferring speed from host request counts. Repeat on Direct Video and Minimig RTG; confirm attached equipment still receives changing OSD metadata. Mark combinations without suitable equipment as untested. |
| Remap source choice | On the spare card, use keyboard Enter to open Button/Key remap, hold it until the dialog appears, then release it. The dialog must still ask for a source. Press the intended gamepad button, release it, and choose a keyboard output. The displayed source and saved mapping must belong to that gamepad. Repeat with another opener device, chords, and ordinary button definition. |
| Remap clear and editor | Back up test mappings. Verify the source label before clearing, then confirm the selected source's normal mapping file is cleared and unrelated mapping identities are retained. Devices sharing a mapping file follow their existing identity settings. Check long device names, Enter Finish, Esc Clear, F12/OK-hold Advanced, input/output editing and cancellation before a source is selected. |
| Recent selection | Existing upstream recovery after a recent file or its parent is removed remains functional. This fork does not claim a new fix for issue 364. |
| VRR reconnect | On a compatible display and VRR core, with `vsync_adjust=0`, test `vrr_mode=2` for FreeSync and `vrr_mode=3` for VESA separately. Confirm the display reports VRR, then reconnect HDMI or switch inputs repeatedly without changing timings. Confirm VRR remains active after logged video reinitialization. |
| Video transitions | Check Menu/core switching, VRR disabled, Direct Video, HDR and Minimig RTG on the hardware that supports each mode. A scaler supplying its own VRR cannot prove MiSTer's packet behavior. |
| Gameplay and persistence | Load representative cartridge and disc cores; exercise normal controller input, saves and existing user configuration. |

## Return to the working build

Restore your known-working Main or switch back to the original card, and verify the version and normal operation. The official updater may replace the experimental Main; record the version before any subsequent tests. Mark unavailable hardware combinations as untested.

## Scope of this candidate

The changes restore VRR packet programming after HDMI reset, anchor inferred ZIP folder paths, correct duplicate parent entry comparisons, add an optional 12-hour clock, preserve rows when paging onto an expanded filename, skip unchanged ordinary-HDMI SPD writes during menu transitions, and correct/display the remap source. No measured gameplay speedup, Linux driver repair, new FPGA feature or general stability guarantee is claimed. Performance work will require baseline measurements on actual hardware.

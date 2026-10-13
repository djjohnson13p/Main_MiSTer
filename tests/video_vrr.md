The regression for [official issue #1226](https://github.com/MiSTer-devel/Main_MiSTer/issues/1226)
uses the current production HDMI initialization, packet programming, VRR selection,
video mode application and reinitialization functions. The runner extracts their
intact definitions from `video.cpp` into temporary files; it does not keep copies
of their implementations. Simulated I2C registers record the actual register
writes. EDID input, SPI I/O, and unrelated video operations are stubbed.

Run with Python 3 and a host compiler supporting C++14:

```sh
python3 tests/video_vrr_regression.py --cxx g++
```

Use `--cxx /path/to/g++` for a compiler outside PATH, `--verbose` for production
video logging, and `--source /path/to/video.cpp` to compare an upstream source.
Add `--only freesync` or `--only vesa` to demonstrate each baseline failure
independently.
The runner fails if expected source definitions change or the compiler reports
warnings. Generated includes and the executable are removed afterward.

The nine cases cover forced and automatic FreeSync/VESA selection, three
consecutive reinitializations with unchanged timings, subsequent cache reuse,
an EDID read that skips reinitialization, and disable/re-enable transitions for
configuration, the menu core, Direct Video and Minimig RTG. They check packet
enable bits, packet payload preservation, ordinary SPD restoration, the FPGA
VRR flag and HDR packet coexistence.

The simulator proves the software register sequence; it cannot validate ADV7513
behavior or display interoperability. Hardware validation requires a MiSTer,
a core with VRR support, and a compatible display. With `vsync_adjust=0`, compare
forced FreeSync (`vrr_mode=2`) and forced VESA (`vrr_mode=3`) separately. Establish
that the display reports VRR before testing, then repeat HDMI disconnect/reconnect
or input-switch cycles that actually log `Video re-initialization`, keeping the
core and timings unchanged. Check that the display still reports VRR after every
cycle and that menu/core transitions and HDR still behave correctly. A scaler
that generates its own VRR is insufficient evidence for MiSTer's packets.

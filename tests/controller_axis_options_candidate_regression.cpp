// SPDX-License-Identifier: GPL-3.0-or-later
// Candidate extension of the frozen normal-input baseline. The driver copies
// that exact harness and adds two named fixture hooks; production bodies are
// extracted unchanged. This is a selected event interval, not whole input_cb.
#include "axis_candidate_presence.h"
static void candidate_setup(int index);
static void candidate_active_case(int index);
#define main compatibility_baseline_main
#include "axis_baseline_harness.cpp"
#undef main

static void candidate_setup(int index)
{
#if AXIS_CANDIDATE_PRESENT
    // Config/open setup is a separate regression seam. Supply admitted device
    // coefficients here, before the baseline snapshots all device/config bytes.
    for (auto &device : input) device.axis_options = {0, 1.0, 1.0};
    if (index < 52) return;
    input[0].axis_options = {0, 0.5, 1.0};
    if (index == 54 || index == 58) input[0].axis_options = {10, 1.0, 1.0};
    if (index == 55) input[0].axis_options = {0, 1.0, 2.0};
    if (index == 56) {
        input[0].bind = 1;
        input[0].axis_options = {0, 2.0, 1.0};
        input[1].axis_options = {0, 0.5, 1.0};
    }
    if (index == 57 || index == 63) input[0].lightgun = 1;
    if (index == 58) { fixture_psx = true; io_ver = 1; }
    if (index == 59) fixture_n64 = true;
    if (index == 60) {
        input[0].quirk = QUIRK_WHEEL;
        input[0].wh_steer = 0; input[0].wh_accel = 3;
        input[0].wh_brake = 4; input[0].wh_clutch = 5; input[0].wh_combo = 6;
    }
    if (index == 64) mouse_emu = 1;
    if (index == 66) {
        input[0].deadzone = 10;
        input[0].max_cardinal[0] = 127;
        input[0].max_range[0] = std::hypot(127.0f, 127.0f);
    }
    if (index == 67) input[0].axis_options = {0, 1.0, 1.0};
#else
    (void)index;
#endif
}

static void candidate_active_case(int index)
{
#if AXIS_CANDIDATE_PRESENT
    switch (index) {
    case 52: event(0,80); event(1,-40); break;
    case 53: event(3,80); event(4,-40); break;
    case 54: event(0,117); event(1,-117); break;
    case 55: event(3,64); event(4,-64); break;
    case 56: event(0,80); event(1,-40); break;
    case 57: event(3,80); event(4,-40); break;
    case 58: event(0,-118,0,-128,127); event(1,117,0,-128,127); break;
    case 59: event(0,127); break;
    case 60: event(0,80); event(0,-80); event(3,0,0,0,255); event(3,255,0,0,255); break;
    case 61: event(8,128,0,0,255); break;
    case 62: event(7,-12,0,-127,127,EV_REL); event(7,7,0,-127,127,EV_REL); break;
    case 63: event(0,80); event(1,-40); break;
    case 64: event(2,80); event(5,-96); break;
    case 65: event(0,127); event(1,127); event(0,0); event(1,0); break;
    case 66: event(0,64); break;
    case 67: ordinary_sequence(false); break;
    default: check(false,"unknown candidate finite case"); return;
    }
    if (index == 52 || index == 53 || index == 56 || index == 57) {
        check(api.size()==2 && api[0].x==40 && api[0].y==0 && api[1].x==40 && api[1].y==-20,"active half gain exact paired output");
        check(!api.empty() && api.back().side==((index==53 || index==57)?"r":"l") && api.back().player==(index==56?1:0),"actual active side and resolved player");
        const int device = index==56?1:0, stick = index==53 || index==57?1:0;
        check(math_steps.size()==2 && math_steps[0].stage=="deadzone" && math_steps[0].device==device && math_steps[0].stick==stick && math_steps[0].out_x==80 && math_steps[1].out_y==-40,"active transform follows unchanged deadzone inputs and outputs");
    }
    if (index == 54) check(api.size()==2 && api[0].x==127 && api[1].x==127 && api[1].y==-127,"active saturation reaches both generic endpoints");
    if (index == 55) check(api.size()==2 && api[0].x==32 && api[1].x==32 && api[1].y==-32 && api.back().side=="r","active right square curve exact components");
    if (index == 56) check(input[0].axis_pos[0]==80 && input[1].max_cardinal[0]==80 && input[0].max_cardinal[0]==0,"bound raw subdevice and resolved device learning remain distinct");
    if (index == 58) check(api.size()==2 && api[0].x==-128 && api[1].x==-128 && api[1].y==127 && packets.size()==2 && packets.back().mode==1,"active saturation preserves asymmetric PSX endpoint packing");
    if (index == 59) {
        check(math_steps.size()==2 && math_steps[0].stage=="deadzone" && math_steps[0].out_x==127 && math_steps[1].stage=="n64" && math_steps[1].x==64 && math_steps[1].y==0,"active half gain occurs between deadzone and actual N64 shaping");
        check(input[0].max_cardinal[0]==127 && api.size()==1 && api[0].x==65 && api[0].y==0 && api[0].side=="l" && api[0].player==0,"N64 consumes shaped component but retains raw cardinal learning and route");
    }
    if (index == 60) check(api.size()==4 && api[0].x==80 && api[1].x==-80 && api[2].x==-80 && api[2].y==-127 && api[3].y==0,"active coefficients do not shape steering or unipolar pedal routes");
    if (index == 61) check(api.size()==1 && api[0].player==15 && api[0].x==-128,"active coefficients leave unsigned paddle wire byte128 unchanged");
    if (index == 62) check(api.size()==2 && api[0].player==143 && api[0].x==-12 && api[1].x==7,"active coefficients leave spinner packets unchanged");
    if (index == 63) check(api.size()==2 && api[0].x==80 && api[1].x==80 && api[1].y==-40,"active coefficients leave positional lightgun calls unchanged");
    if (index == 64) check(api.empty() && packets.empty() && mouse_emu_x==6 && mouse_emu_y==-8,"active coefficients leave ordinary mouse route unchanged");
    if (index == 65) {
        check(input[0].axis_pos[0]==0 && input[0].axis_pos[1]==0 && input[0].ss_range[0]==32258 && input[0].max_cardinal[0]==127 && input[0].max_range[0]==std::sqrt(32258.0f),"active coefficients preserve raw cache and learned diagonal/cardinal extrema");
        check(api.size()==4 && api[0].x==64 && api[1].x==64 && api[1].y==64 && api[2].x==0 && api[2].y==64 && api[3].x==0 && api[3].y==0,"active half gain never feeds shaped values back into paired raw position");
    }
    if (index == 66) {
        check(math_steps.size()==1 && math_steps[0].x==64 && math_steps[0].out_x==59 && math_steps[0].out_y==0,"ordinary radial deadzone completes before active shaping");
        check(api.size()==1 && api[0].x==30 && api[0].y==0 && input[0].axis_pos[0]==64,"half-away rounding consumes actual post-deadzone59 while raw64 remains cached");
    }
    if (index == 67) {
        const int expected[][2] = {{80,0},{80,-40},{0,-40},{-127,-40},{-127,127}};
        check(api.size()==5,"explicit identity fields retain default output count");
        for (size_t i=0;i<api.size() && i<5;++i) check(api[i].x==expected[i][0] && api[i].y==expected[i][1],"explicit identity fields retain exact paired bytes");
    }
#else
    (void)index;
    check(false,"candidate-only case must be excluded when actual module is absent");
#endif
}

int main(int argc, char **argv)
{
    return compatibility_baseline_main(argc, argv);
}

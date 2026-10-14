static int physical_menu_button = 0;
static int bluetooth_route_queries = 0;
static int osd_enables = 0;
static int joy_bcount = 0;
static int saved_menustate = 0, osd_unlocked = 1, has_fb_terminal = 0;
static bool script_finished = true;
int hci_get_route(int) { bluetooth_route_queries++; return -1; }
void OsdSetSize(int) {}
void OsdEnable(int) { osd_enables++; }
void user_io_status_set(const char *, uint32_t) {}
void user_io_status_save(const char *) {}
const char *user_io_create_config_name() { return "fixture"; }
bool input_has_lightgun() { return false; }
void input_advanced_delete(advancedButtonMap *, int) {}
const char *get_autofire_rate_hz(int) { return "off"; }
int get_autofire_rate_count() { return 1; }

// PROBE_SCENARIOS
static unsigned int desired_checks = 0, desired_failures = 0;
static void desired(bool ok, const char *label)
{
    desired_checks++; if (!ok) desired_failures++;
    std::printf("{\"kind\":\"check\",\"label\":\"%s\",\"passed\":%s}\n", label, ok ? "true" : "false");
}
static void observe(const char *label)
{
    std::printf("{\"kind\":\"state\",\"label\":\"%s\",\"menustate\":%d,\"mapping\":%d,\"count\":%d,\"type\":%d,\"source\":%d,\"phase\":%d,\"finish\":%d,\"bt_queries\":%d,\"queued_key\":%u}\n",
        label, menustate, mapping, get_map_count(), get_map_type(), get_map_dev(), get_map_set(), get_map_finish(), bluetooth_route_queries, menu_key);
}
static void drain_ui_key()
{
    tick_ui(); now_ms += 21; tick_ui();
}
static void open_advanced_capture(bool output)
{
    begin(); finish_map_setting(0);
    abm_dev_num = 1; abm_edit_ptr = &input[1].advanced_map[0];
    const uint16_t existing_input[] = {BTN_WEST, BTN_NORTH, KEY_A, KEY_B};
    const uint16_t existing_output[] = {KEY_Q, KEY_W, KEY_E, KEY_R};
    std::memcpy(abm_edit_ptr->input_codes, existing_input, sizeof(existing_input));
    std::memcpy(abm_edit_ptr->output_codes, existing_output, sizeof(existing_output));
    menustate = MENU_ADVANCED_MAP_EDIT1; menusub = output ? 2 : 0;
    tick_ui();
    desired(menustate == MENU_ADVANCED_MAP_EDIT2 && !mapping,
        "actual Advanced edit renderer must enter its interactive screen without capture");
    event(0, KEY_ENTER, 1); drain_ui_key();
    desired(menustate == MENU_ADVANCED_MAP_CAPTURE1 && mapping && get_map_count() == 1 && get_map_set() == (output ? 2 : 1),
        "actual Enter and Edit2 case must start count-one input or output capture");
    event(0, KEY_ENTER, 0); // opener release rejected by real capture ownership
    menu_key_set(0); drain_ui_key();
    desired(mapping && get_map_button() == 0 && !get_map_finish(),
        "actual new capture must remain active and unfinished after opener release");
    observe("Advanced capture after opener release"); // historical source-choice behavior is recorded separately
}
static void long_menu()
{
    const int previous_queries = bluetooth_route_queries;
    physical_menu_button = 1; tick_ui();
    now_ms += 2999; tick_ui();
    desired(bluetooth_route_queries == previous_queries, "physical Menu must not pair before its actual three-second deadline");
    now_ms++; tick_ui(); observe("physical Menu held at three-second deadline");
    physical_menu_button = 0; tick_ui(); now_ms += 21; tick_ui();
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    if (scenario == "protected-remap")
    {
        begin(); menustate = MENU_JOYKBDMAP; tick_ui();
        desired(menustate == MENU_JOYKBDMAP1 && mapping && get_map_count() == 3,
            "actual standard Button-Key remap entry must start its capture");
        long_menu();
        desired(menustate == MENU_JOYKBDMAP1 && mapping && bluetooth_route_queries == 0,
            "current protected standard remap must reject physical Menu Bluetooth interruption");
    }
    else if (scenario == "protected-keyboard")
    {
        begin(); finish_map_setting(0); start_map_setting(0); menustate = MENU_KBDMAP1;
        long_menu();
        desired(menustate == MENU_KBDMAP1 && mapping && bluetooth_route_queries == 0,
            "current protected keyboard-remap state must reject physical Menu Bluetooth interruption");
    }
    else if (scenario.find("advanced-") == 0 && scenario != "advanced-mask")
    {
        const bool output = scenario.find("output") != std::string::npos;
        const bool held = scenario.find("held") != std::string::npos;
        open_advanced_capture(output);
        uint16_t other_codes[sizeof(abm_edit_ptr->input_codes) / sizeof(uint16_t)];
        std::memcpy(other_codes, output ? abm_edit_ptr->input_codes : abm_edit_ptr->output_codes, sizeof(other_codes));
        if (held) event(output ? 0 : 1, output ? KEY_O : BTN_EAST, 1);
        const int source = get_map_dev();
        long_menu();
        desired(menustate == MENU_ADVANCED_MAP_CAPTURE1 && osd_title == "Set Hotkey" && bluetooth_route_queries == 0,
            "physical Menu hold must leave actual Advanced capture visible without Bluetooth interruption");
        desired(mapping && get_map_count() == 1 && get_map_dev() == source,
            "protected hardware hold must preserve pending or held capture source and phase");
        if (!held) event(output ? 0 : 1, output ? KEY_O : BTN_EAST, 1);
        event(output ? 0 : 1, output ? KEY_O : BTN_EAST, 0); tick_ui();
        desired(!mapping && menustate == MENU_ADVANCED_MAP_EDIT1,
            "owned chord release after physical Menu hold must reach the actual capture finish consumer");
        desired((output ? abm_edit_ptr->output_codes[0] : abm_edit_ptr->input_codes[0]) == (output ? KEY_O : BTN_EAST)
            && !std::memcmp(other_codes, output ? abm_edit_ptr->input_codes : abm_edit_ptr->output_codes, sizeof(other_codes)),
            "capture after protected hold must retain its selected code and untouched opposite half");
        tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_EDIT2 && !mapping,
            "completed capture must render its interactive edit screen again");
        event(0, KEY_F12, 1); drain_ui_key(); event(0, KEY_F12, 0); drain_ui_key();
        desired(menustate == MENU_ADVANCED_MAP_LIST2 && !mapping && !ignore_osd_release,
            "normal USB F12 must navigate after completion instead of remaining trapped in capture");
        desired(saved.size() == 1 && saved[0].device == 1,
            "actual edit navigation must save against its original Advanced source identity");
        long_menu();
        desired(menustate == MENU_BTPAIR2 && !mapping && bluetooth_route_queries == 1,
            "fresh physical Menu after completion and release must retain ordinary Advanced-list pairing");
        event(0, KEY_ENTER, 1); drain_ui_key();
        desired(menustate == MENU_NONE1 && !mapping,
            "USB Enter after completed capture must dismiss normal Bluetooth popup");
    }
    else if (scenario == "advanced-mask")
    {
        begin(); finish_map_setting(0); abm_edit_ptr = &input[1].advanced_map[0];
        menustate = MENU_ADVANCED_MAP_EDIT3; tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_EDIT4 && !mapping,
            "actual Edit3-4 core-button mask screen is not a capture state");
        long_menu();
        desired(menustate == MENU_BTPAIR2 && !mapping && bluetooth_route_queries == 1,
            "Bluetooth on mask screen occurs outside capture and is distinct from the capture defect");
        event(0, KEY_ENTER, 1); drain_ui_key();
        desired(menustate == MENU_NONE1 && !mapping,
            "USB Enter outside capture must retain normal Bluetooth popup exit");
    }
    else if (scenario == "normal-outside")
    {
        begin(); finish_map_setting(0); menustate = MENU_COMMON1;
        long_menu();
        desired(menustate == MENU_BTPAIR2 && !mapping && bluetooth_route_queries == 1,
            "normal long Menu outside remapping must retain Bluetooth pairing entry");
        event(0, KEY_ENTER, 1); drain_ui_key();
        desired(menustate == MENU_NONE1 && !mapping,
            "normal USB Enter outside remapping must exit the no-Bluetooth popup");
    }
    else if (scenario == "capture-short")
    {
        open_advanced_capture(false);
        physical_menu_button = 1; tick_ui(); now_ms += 500;
        physical_menu_button = 0; tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_CAPTURE1 && mapping && bluetooth_route_queries == 0,
            "short physical Menu release must not cause the long-press Bluetooth interruption");
    }
    else if (scenario == "escape-clear-input" || scenario == "escape-clear-output" || scenario == "controller-clear")
    {
        const bool output = scenario == "escape-clear-output";
        open_advanced_capture(output);
        uint16_t other_codes[sizeof(abm_edit_ptr->input_codes) / sizeof(uint16_t)];
        std::memcpy(other_codes, output ? abm_edit_ptr->input_codes : abm_edit_ptr->output_codes, sizeof(other_codes));
        long_menu();
        desired(menustate == MENU_ADVANCED_MAP_CAPTURE1 && mapping && bluetooth_route_queries == 0,
            "physical Menu hold must preserve capture until its actual clear control");
        if (scenario == "controller-clear")
        {
            input[1].mmap[SYS_BTN_OSD_KTGL + 1] = input[1].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
            event(1, BTN_MODE, 1); tick_ui();
            desired(mapping && !get_map_cancel(), "actual controller Menu clear must retain its separate five-second timer");
            now_ms += 5000; tick_ui(); event(1, BTN_MODE, 0);
        }
        else
        {
            tap(0, KEY_ESC); tick_ui();
        }
        const auto *codes = output ? abm_edit_ptr->output_codes : abm_edit_ptr->input_codes;
        bool empty = true;
        for (size_t i = 0; i < sizeof(abm_edit_ptr->input_codes) / sizeof(uint16_t); i++) empty &= !codes[i];
        desired(!mapping && menustate == MENU_ADVANCED_MAP_EDIT1 && empty,
            "actual Escape or controller Menu clear must finish capture and clear only its edited half");
        desired(!std::memcmp(other_codes, output ? abm_edit_ptr->input_codes : abm_edit_ptr->output_codes, sizeof(other_codes)) && cleared.empty(),
            "actual count-one clear must retain opposite chord and avoid clearing all source remaps");
    }
    else if (scenario == "scripts-hold")
    {
        begin(); finish_map_setting(0); menustate = MENU_SCRIPTS1;
        menu_key_set(0); drain_ui_key();
        physical_menu_button = 1; tick_ui(); now_ms += 3000; tick_ui();
        desired(ui_prefix_key == KEY_BACKSPACE && menustate == MENU_SCRIPTS1 && !bluetooth_route_queries,
            "existing Scripts physical Menu hold must retain its Backspace command instead of pairing");
        physical_menu_button = 0; tick_ui();
        desired(!menu && !bluetooth_route_queries, "consumed Scripts hold release must not become an unrelated menu command");
    }
    else return 2;
    observe("final");
    std::printf("{\"kind\":\"summary\",\"checks\":%u,\"failures\":%u,\"fixture_checks\":%u,\"fixture_failures\":%d}\n",
        desired_checks, desired_failures, checks, failures);
    return desired_failures || failures ? 1 : 0;
}

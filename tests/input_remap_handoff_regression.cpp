// Host boundary fixtures for the production keyboard/UI handoff extraction.
using uint = unsigned int;
static bool osd_is_visible = true;
static bool use_ps2ctl = false;
static bool joy_force = false;
static int emu_mode = EMU_NONE;
static int kbd_toggle = 0;
static int kbd_mouse_emu = 0;
static int mouse_emu_x = 0, mouse_emu_y = 0, mouse_sniper = 0, mouse_btn = 0;
static unsigned long mouse_timer = 0;
static std::vector<std::pair<uint16_t, int>> sent_keys;
static std::vector<std::pair<uint16_t, int>> dispatched_keys;
static int reset_checks = 0, runtime_map_updates = 0;
static unsigned char core_type = CORE_TYPE_8BIT;
static int kbd_reset = 0, kbd_reset_ovr = 0, coldreset_req = 0;
static int menustate = MENU_COMMON1, parentstate = 0;
static uint32_t menusub = 0;
static int firstmenu = 0, adjvisible = 0, helptext_idx = 0;
static unsigned int menumask = 0;
static int abm_dev_num = -1;
static advancedButtonMap abm_edit_map = {};
static advancedButtonMap *abm_edit_ptr = nullptr;
static char s[256] = {};
static const char *info_top = "top", *info_bottom = "bottom";
static bool menu = false, back = false, up = false, down = false;
static bool left = false, right = false, plus = false, minus = false, recent = false;
static bool ignore_osd_release = false;
static int select_ini = 0;
static std::string osd_title;
static unsigned int ui_prefix_key = 0;
static int framebuffer_disables = 0;

uint32_t get_key_mod();
uint32_t get_ps2_code(uint16_t);
uint32_t get_amiga_code(uint16_t);
void menu_key_set(unsigned int);
void production_user_io_kbd(uint16_t, int);
void production_user_io_check_reset(unsigned short, char);
void register_activity() {}
void spi_uio_cmd(int) {}
void request_screenshot(const char *, bool) {}
bool hasAPI1_5() { return false; }
void set_volume(int) {}
void setBrightness(int, int) {}
void PrintDirectory() {}
bool has_menu() { return true; }
bool is_f12_mod_needed() { return false; }
bool video_fb_state() { return false; }
int video_chvt(int) { return 0; }
bool user_io_osd_is_visible() { return osd_is_visible; }
bool is_archie() { return false; }
int input_state();
void set_emu_mode(int mode) { emu_mode = mode; }
void open_joystick_setup() {}
void send_keycode(uint16_t key, int value) { sent_keys.push_back({key, value}); }
int keyrah_trans(int key, int) { return key; }
bool is_minimig() { return false; }
void minimig_reset() {}
void user_io_check_reset(uint16_t modifiers, int mode) { reset_checks++; production_user_io_check_reset(modifiers, mode); }
bool update_advanced_state(int, uint16_t, int) { runtime_map_updates++; return true; } // no active F12 chord
int user_io_get_kbdemu() { return EMU_NONE; }
void joy_digital(int, uint32_t, int, int, int) {}
void mouse_cb() {}
int user_io_menu_button() { return 0; }
int user_io_user_button() { return 0; }
int get_map_vid() { return 0; }
int get_map_pid() { return 0; }
void send_map_cmd(int) {}
void video_menu_bg(uint32_t, int = 0) {}
uint32_t user_io_status_get(const char *) { return 0; }
void OsdMenuCtl(int) {}
void video_fb_enable(int) { framebuffer_disables++; }
void OsdSetTitle(const char *text, int = 0) { osd_title = text; }
void OsdWrite(int, const char * = "", int = 0, int = 0) {}
int OsdGetSize() { return 16; }
void OsdUpdate() {}
void sleep(unsigned int) {}
void MenuWrite(int, const char * = "", int = 0, int = 0) {}
void menu_parse_buttons() {} // core display metadata is external to this event proof
void get_button_name_for_code(uint16_t code, int, char *dest, size_t size)
{ std::snprintf(dest, size, "B%u", code); }
void menu_button_name(int, char *dest, size_t size) { std::snprintf(dest, size, "-"); }

// Combined UI/event assertions.

static unsigned int handoff_checks = 0, handoff_failures = 0;
static void desired(bool ok, const char *label)
{
    handoff_checks++;
    if (!ok) handoff_failures++;
    std::printf("{\"kind\":\"check\",\"label\":\"%s\",\"passed\":%s}\n", label, ok ? "true" : "false");
}
static void observe(const char *label)
{
    std::printf("{\"kind\":\"state\",\"label\":\"%s\",\"menustate\":%d,\"mapping\":%d,\"source\":%d,\"phase\":%d,\"advance\":%s,\"queued_key\":%u,\"decoded_key\":%u,\"menu_event\":%s,\"ignore_release\":%s,\"normal_saves\":%u}\n",
        label, menustate, mapping, get_map_dev(), get_map_set(), get_map_advance() ? "true" : "false",
        menu_key, ui_prefix_key, menu ? "true" : "false", ignore_osd_release ? "true" : "false", (unsigned)normal_map_saves.size());
}
static void settle_ui_key()
{
    tick_ui(); // actual debounce notices the changed queued key
    now_ms += 21;
    tick_ui(); // actual debounce releases the key to the real F12 UI prefix
}
static void open_capture()
{
    begin();
    menustate = MENU_JOYKBDMAP;
    tick_ui(); // actual entry clears codes and starts capture
    desired(menustate == MENU_JOYKBDMAP1 && mapping && get_map_dev() == -1,
        "production remap entry must initialize capture");
}
static void open_advanced_while_f12_held(int device = 0, int physical_code = KEY_F12)
{
    open_capture();
    if (device == 2) input[2].bind = 0;
    if (physical_code == KEY_A) input[0].kbdmap[KEY_A] = KEY_F12;
    event(device, physical_code, 1);
    desired(get_map_advance(), "first F12 press must expose Advanced before handoff");
    tick_ui();
    desired(menustate == MENU_ADVANCED_MAP_LIST1 && !mapping && abm_dev_num == 0,
        "production remap UI must consume Advanced and finish capture");
    tick_ui();
    desired(menustate == MENU_ADVANCED_MAP_LIST2 && osd_title == "Advanced",
        "production Advanced list must render and become interactive");
    observe("Advanced list interactive with F12 still held");
}
static void owned_event(int device, int code, int value, const char *label)
{
    const size_t dispatched = dispatched_keys.size(), sent = sent_keys.size();
    const int resets = reset_checks, updates = runtime_map_updates;
    const uint32_t previous_key = menu_key, mods = get_key_mod();
    event(device, code, value);
    desired(dispatched_keys.size() == dispatched && sent_keys.size() == sent
        && reset_checks == resets && runtime_map_updates == updates
        && menu_key == previous_key && get_key_mod() == mods, label);
}
static void run_inventory_hook(const std::string &scenario)
{
    prepare_inventory_fixture();
    if (scenario.find("inventory-") == 0) inventory_changed();
    else if (scenario.find("joycon-") == 0) { input[1].quirk = QUIRK_JOYCON; joycon_changed(1); }
    else { state = 1; inventory_initialize(); }
}
int main(int argc, char **argv)
{
    const std::string scenario = argc > 1 ? argv[1] : "after-handoff";
    if (scenario == "existing-suite") return existing_main();
    if (scenario == "before-ui")
    {
        open_capture(); event(0, KEY_F12, 1); event(0, KEY_F12, 0);
        settle_ui_key();
        observe("F12 press-release drained before remap UI");
        desired(menustate == MENU_ADVANCED_MAP_LIST2,
            "first-command F12 tap before UI must deterministically enter Advanced");
    }
    else if (scenario == "after-handoff")
    {
        open_advanced_while_f12_held(); event(0, KEY_F12, 0);
        observe("physical F12 release dispatched after capture handoff");
        settle_ui_key();
        observe("F12 release processed by Advanced list2");
        desired(menustate == MENU_ADVANCED_MAP_LIST2,
            "release of consumed Advanced command must not exit its list");
    }
    else if (scenario == "render-boundary")
    {
        open_capture(); event(0, KEY_F12, 1); tick_ui();
        event(0, KEY_F12, 0);
        // Rendering and debounce proceed together; release becomes a menu
        // event once the list is interactive, rather than being a queued latch.
        settle_ui_key();
        observe("F12 release around first Advanced render");
        desired(menustate == MENU_ADVANCED_MAP_LIST2,
            "Advanced entry must survive release at its render boundary");
    }
    else if (scenario == "normal-f12")
    {
        begin(); finish_map_setting(0); menustate = MENU_ADVANCED_MAP_LIST2; abm_dev_num = 0;
        menu_key_set(0); settle_ui_key(); normal_map_saves.clear();
        event(0, KEY_F12, 1); settle_ui_key();
        observe("normal F12 press outside capture");
        desired(menustate == MENU_COMMON1 && ignore_osd_release && normal_map_saves.size() == 1,
            "normal F12 press must retain global menu navigation and arm release ignore");
        event(0, KEY_F12, 0); settle_ui_key();
        observe("normal paired F12 release outside capture");
        desired(!ignore_osd_release && normal_map_saves.size() == 1 && !menu,
            "normal paired F12 release must be ignored once without duplicate navigation");
    }
    else if (scenario == "normal-unpaired-release")
    {
        begin(); finish_map_setting(0); menustate = MENU_ADVANCED_MAP_LIST2; abm_dev_num = 0;
        menu_key_set(0); settle_ui_key(); normal_map_saves.clear();
        event(0, KEY_F12, 0); settle_ui_key();
        observe("normal unpaired F12 release outside capture");
        desired(menustate == MENU_COMMON1 && normal_map_saves.size() == 1,
            "normal unpaired F12 release must retain current global menu handling");
    }
    else if (scenario == "later-normal-press")
    {
        open_advanced_while_f12_held(); event(0, KEY_F12, 0); settle_ui_key();
        // A future consumed-release policy must not swallow a new normal pair.
        menustate = MENU_ADVANCED_MAP_LIST2; normal_map_saves.clear();
        event(0, KEY_F12, 1); settle_ui_key(); event(0, KEY_F12, 0); settle_ui_key();
        desired(menustate == MENU_COMMON1 && normal_map_saves.size() == 1 && !ignore_osd_release,
            "a later normal F12 pair must remain usable after consumed-command release");
    }
    else if (scenario == "foreign-release-ownership")
    {
        open_advanced_while_f12_held();
        // Another keyboard may have held F12 before capture. Its unrelated
        // release keeps normal OSD semantics and must not consume device0's token.
        event(2, KEY_F12, 0); settle_ui_key();
        desired(menustate == MENU_COMMON1 && normal_map_saves.size() == 1,
            "a foreign unpaired F12 release must retain normal global menu behavior");
        menu_key_set(0); settle_ui_key();
        event(0, KEY_F12, 0); settle_ui_key();
        observe("matching consumed release after a foreign release");
        desired(!menu && menu_key == 0,
            "foreign release must not consume ownership of the later matching command release");
    }
    else if (scenario == "canonical-release")
    {
        open_advanced_while_f12_held(2);
        event(0, KEY_F12, 0); settle_ui_key();
        observe("canonical alias release after bound-node command press");
        desired(menustate == MENU_ADVANCED_MAP_LIST2,
            "consumed release ownership must match a canonical alias of the press device");
    }
    else if (scenario == "normalized-release")
    {
        open_advanced_while_f12_held(0, KEY_A);
        event(0, KEY_A, 0); settle_ui_key();
        observe("normalized F12 alias release after command handoff");
        desired(menustate == MENU_ADVANCED_MAP_LIST2,
            "consumed release ownership must match the normalized F12 event code");
    }
    else if (scenario == "pending-repeat")
    {
        open_capture(); event(0, KEY_F12, 1);
        owned_event(0, KEY_F12, 2, "owned repeat before UI must not dispatch reset modifiers runtime maps or keyboard");
        owned_event(0, KEY_F12, 0, "owned first release before UI must not dispatch and must retain the pending request");
        owned_event(0, KEY_F12, 2, "repeat after early release must not dispatch or cancel the pending request");
        desired(get_map_advance() && get_map_advance() && get_map_dev() == 0 && get_map_button() == 0,
            "repeated getters must retain pending Advanced without data or source mutation");
        input[2].mmap[SYS_BTN_OSD_KTGL + 1] = input[2].mmap[SYS_BTN_OSD_KTGL + 2] = BTN_MODE;
        input[2].osd_combo = 3;
        event(2, BTN_MODE, 0); event(2, KEY_A, 1); event(2, KEY_A, 2); event(2, KEY_A, 0);
        desired(get_map_advance() && get_map_button() == 0 && input[2].osd_combo == 0,
            "queued foreign events must update combo state and preserve the pending command");
        tick_ui(); tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_LIST2, "queued events cannot erase Advanced before UI handoff");
    }
    else if (scenario == "after-handoff-repeat")
    {
        open_advanced_while_f12_held();
        owned_event(0, KEY_F12, 2, "owned repeat after handoff must not reach normal keyboard side effects");
        owned_event(0, KEY_F12, 0, "owned held release must be suppressed after its repeat");
        settle_ui_key();
        desired(menustate == MENU_ADVANCED_MAP_LIST2, "owned repeat and held release must preserve Advanced list");
    }
    else if (scenario == "selected-source-repeat")
    {
        open_capture(); map_to_keyboard(); event(2, KEY_F12, 2);
        desired(!get_map_advance() && get_map_dev() == 1 && get_map_button() == 0,
            "F12 repeat in a selected next-source phase must not enter Advanced without a fresh command");
        event(2, KEY_F12, 1); tick_ui(); tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_LIST2 && abm_dev_num == 1 && saved.size() == 1,
            "fresh Advanced from a foreign keyboard must retain the previously selected pad");
        owned_event(2, KEY_F12, 0, "owned release identity must belong to the command keyboard rather than selected pad");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "command keyboard release must preserve selected-pad Advanced");
    }
    else if (scenario == "second-before-ui")
    {
        open_capture(); event(0, KEY_F12, 1); event(0, KEY_F12, 0); event(0, KEY_F12, 1);
        desired(get_map_advance() && get_map_button() == 0, "second fresh absorbed F12 must rearm the pending held release");
        tick_ui(); tick_ui();
        owned_event(0, KEY_F12, 0, "release of the second absorbed F12 must be consumed after handoff");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "second absorbed held release must not exit Advanced");
    }
    else if (scenario == "repeat-does-not-rearm")
    {
        open_capture(); event(0, KEY_F12, 1); event(0, KEY_F12, 0); event(0, KEY_F12, 2);
        tick_ui(); tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_LIST2, "released-command repeat must preserve pending handoff");
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0); settle_ui_key();
        desired(dispatched_keys.size() == before + 1 && menustate == MENU_COMMON1,
            "repeat must not rearm an already consumed release and swallow a later unpaired release");
    }
    else if (scenario == "stale-fresh-recovery")
    {
        open_advanced_while_f12_held(); // Missed old release, then a fresh normal press.
        size_t before = dispatched_keys.size(); int resets = reset_checks, updates = runtime_map_updates;
        event(0, KEY_F12, 1); settle_ui_key();
        desired(menustate == MENU_COMMON1 && ignore_osd_release,
            "fresh normal press must supersede stale owned release and retain global navigation");
        event(0, KEY_F12, 2); event(0, KEY_F12, 0); settle_ui_key();
        desired(dispatched_keys.size() == before + 3 && reset_checks == resets + 3 && runtime_map_updates == updates + 3
            && !ignore_osd_release && normal_map_saves.size() == 1,
            "fresh recovered press repeat and release must all retain normal dispatch and paired-release handling");
    }
    else if (scenario == "double-release")
    {
        open_advanced_while_f12_held(); owned_event(0, KEY_F12, 0, "first owned release must be consumed once");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "first owned release must preserve the list");
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0); settle_ui_key();
        desired(dispatched_keys.size() == before + 1 && menustate == MENU_COMMON1,
            "duplicate unpaired release must retain global handling after exactly one consumed release");
    }
    else if (scenario == "wrong-code")
    {
        open_advanced_while_f12_held(); size_t before = dispatched_keys.size();
        event(0, KEY_A, 0); settle_ui_key();
        desired(dispatched_keys.size() == before + 1 && menustate == MENU_ADVANCED_MAP_LIST2,
            "wrong code on the owned device must retain normal dispatch without consuming ownership");
        owned_event(0, KEY_F12, 0, "later matching release must remain owned after a wrong-code release");
    }
    else if (scenario == "bound-alias")
    {
        open_advanced_while_f12_held(2); input[3].bind = 0;
        owned_event(3, KEY_F12, 0, "another bound leaf must match the effective canonical command device");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "bound-leaf release must preserve Advanced");
    }
    else if (scenario == "normalized-alias")
    {
        open_advanced_while_f12_held(0, KEY_A); input[0].kbdmap[KEY_B] = KEY_F12;
        owned_event(0, KEY_B, 0, "another physical key normalized to F12 must match the normalized command identity");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "normalized-alias release must preserve Advanced");
    }
    else if (scenario == "foreign-repeat")
    {
        open_advanced_while_f12_held(); size_t before = dispatched_keys.size();
        event(2, KEY_F12, 2);
        desired(dispatched_keys.size() == before + 1, "foreign-device repeat must retain normal keyboard dispatch");
        owned_event(0, KEY_F12, 0, "foreign repeat must not consume the later matching owned release");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "foreign repeat cannot erase command ownership");
    }
    else if (scenario == "combo-bookkeeping")
    {
        open_capture(); input[0].mmap[SYS_BTN_OSD_KTGL + 1] = input[0].mmap[SYS_BTN_OSD_KTGL + 2] = KEY_F12;
        event(0, KEY_F12, 1); desired(input[0].osd_combo == 3 && get_map_advance(), "accepted command must retain real combo press bookkeeping");
        tick_ui(); tick_ui(); owned_event(0, KEY_F12, 0, "owned combo release must not drive normal Menu rewrite or keyboard side effects");
        desired(input[0].osd_combo == 0, "suppressed command release must still clear real combo bits");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "owned combo release must retain Advanced");
    }
    else if (scenario == "default-finish" || scenario == "dismiss-one" || scenario == "dismiss-two"
             || scenario == "invalid-dismiss-one" || scenario == "invalid-dismiss-two")
    {
        open_capture(); event(0, KEY_F12, 1);
        int dismiss = scenario == "default-finish" ? 0 : scenario.find("one") != std::string::npos ? 1 : 2;
        if (scenario.find("invalid-") == 0) finish_map_setting(dismiss, true);
        else finish_map_setting(dismiss);
        desired(!mapping && !get_map_advance(), "default finish or invalid dismiss handoff must clear the pending request");
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(dispatched_keys.size() == before + 1 && menu_key == (KEY_F12 | UPSTROKE),
            "default finish or dismiss must clear the command identity rather than retain a release token");
        desired(dismiss ? cleared == std::vector<int>{0} : cleared.empty(), "default finish or dismiss must preserve selected source clear semantics");
    }
    else if (scenario == "invalid-menu-handoff")
    {
        open_capture(); event(0, KEY_F12, 1); menu_core = true; finish_map_setting(0, true); menu_core = false;
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(!mapping && !get_map_advance() && dispatched_keys.size() == before + 1,
            "explicit handoff in a now-disabled Menu context must clear rather than retain command ownership");
    }
    else if (scenario == "invalid-double-handoff")
    {
        open_advanced_while_f12_held(); finish_map_setting(0, true);
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(dispatched_keys.size() == before + 1,
            "another explicit finish after capture already handed off cannot preserve a held command record");
    }
    else if (scenario == "new-capture")
    {
        open_advanced_while_f12_held(); start_map_setting(3, 0, &store.entry);
        event(0, KEY_F12, 0);
        desired(get_map_dev() == -1 && !get_map_advance() && !get_map_finish(), "new capture must reset held-command ownership and ignore its old opener release");
        finish_map_setting(0); size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(dispatched_keys.size() == before + 1, "new capture and unselected abort must not retain old command ownership");
    }
    else if (scenario == "count-one-handoff")
    {
        begin(); start_map_setting(1, 1, &store.entry); event(0, KEY_F12, 1); finish_map_setting(0, true);
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(dispatched_keys.size() == before + 1 && store.entry.input_codes[0] == KEY_F12,
            "explicit finish on a count-one data edit cannot preserve an Advanced command identity");
    }
    else if (scenario == "timer-handoff")
    {
        open_capture(); event(1, BTN_SOUTH, 1); now_ms += 4999;
        desired(!get_map_advance(), "mapped-OK handoff must retain the real five-second deadline");
        now_ms++; tick_ui(); tick_ui();
        desired(menustate == MENU_ADVANCED_MAP_LIST2 && abm_dev_num == 1, "elapsed mapped-OK hold must retain real Advanced handoff");
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0); settle_ui_key();
        desired(dispatched_keys.size() == before + 1 && menustate == MENU_COMMON1,
            "timer-based Advanced handoff cannot invent ownership of an unrelated F12 release");
    }
    else if (scenario == "ordinary-hooks")
    {
        open_advanced_while_f12_held(); prepare_inventory_fixture();
        input_switch(0); input_notify_mode(); input_switch(-1);
        desired(input_state() == 0 && ungrabbed_devices == 2, "ordinary visibility and emulation hooks must retain real grab/mouse effects");
        owned_event(0, KEY_F12, 0, "ordinary visibility and emulation hooks must preserve held-command ownership");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "ordinary hooks must not turn consumed command release into global navigation");
    }
    else if (scenario == "soft-reset")
    {
        open_advanced_while_f12_held(); production_user_io_check_reset(0x45, 0); user_io_set_kbd_reset(1);
        desired(kbd_reset && kbd_reset_ovr, "real generic keyboard soft-reset setters must execute in the control");
        owned_event(0, KEY_F12, 0, "ordinary same-process core reset must preserve held-command ownership");
        desired(kbd_reset && kbd_reset_ovr, "consumed command release must not clear an unrelated soft-reset request");
        settle_ui_key(); desired(menustate == MENU_ADVANCED_MAP_LIST2, "soft reset must not recreate the command release as global navigation");
    }
    else if (scenario == "inventory-pending" || scenario == "joycon-pending" || scenario == "initialize-pending")
    {
        open_capture(); map_to_keyboard(); event(0, KEY_F12, 1); run_inventory_hook(scenario);
        desired(get_map_finish() && get_map_dev() == -1 && !get_map_advance(), "real inventory hook must turn pending Advanced into harmless unselected Finish");
        tick_ui(); desired(!mapping && menustate == MENU_COMMON1 && abm_dev_num == -1 && cleared.empty() && saved.size() == 1,
            "real UI must consume invalidated pending capture without opening Advanced or mutating reused device mappings");
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(dispatched_keys.size() == before + 1, "replacement device release must not inherit invalidated command ownership");
    }
    else if (scenario == "inventory-held" || scenario == "joycon-held" || scenario == "initialize-held")
    {
        open_advanced_while_f12_held(); run_inventory_hook(scenario);
        size_t before = dispatched_keys.size(); event(0, KEY_F12, 0);
        desired(dispatched_keys.size() == before + 1 && menu_key == (KEY_F12 | UPSTROKE),
            "actual inventory rebuild after handoff must clear held ownership before device indices can be reused");
    }
    else return 2;
    std::printf("{\"kind\":\"summary\",\"scenario\":\"%s\",\"checks\":%u,\"failures\":%u}\n",
        scenario.c_str(), handoff_checks, handoff_failures);
    return handoff_failures ? 1 : 0;
}

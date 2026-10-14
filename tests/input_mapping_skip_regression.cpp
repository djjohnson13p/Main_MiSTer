static unsigned int probe_checks = 0, probe_failures = 0;
static void expect(bool ok, const char *label)
{
	probe_checks++; if (!ok) probe_failures++;
	std::printf("{\"kind\":\"check\",\"label\":\"%s\",\"passed\":%s}\n", label, ok ? "true" : "false");
}
static void observe(const char *label)
{
	std::printf("{\"kind\":\"state\",\"label\":\"%s\",\"slot\":%d,\"device\":%d,\"half\":%d,\"combo\":%d,\"map\":[%u,%u,%u,%u,%u,%u,%u,%u]}\n",
		label, get_map_button(), get_map_dev(), mapping_set, input[1].osd_combo,
		input[1].map[0], input[1].map[1], input[1].map[2], input[1].map[3],
		input[1].map[4], input[1].map[5], input[1].map[6], input[1].map[7]);
}
static int complete()
{
	std::printf("{\"kind\":\"summary\",\"checks\":%u,\"failures\":%u,\"fixture_checks\":%u,\"fixture_failures\":%d}\n",
		probe_checks, probe_failures, checks, failures);
	return probe_failures || failures ? 1 : 0;
}
static void setup(bool alternative, int slot, bool one_button)
{
	begin();
	for (auto &name : joy_bnames) std::strcpy(name, "button");
	input[1].mmap[SYS_BTN_OSD_KTGL + 1] = BTN_MODE;
	input[1].mmap[SYS_BTN_OSD_KTGL + 2] = one_button ? BTN_MODE : BTN_SOUTH;
	start_map_setting(8);
	if (alternative)
	{
		for (int n = 0; n < 8; n++) tap(1, KEY_EMU + n);
		finish_map_setting(0); start_map_setting(8, 1);
		input[1].has_map = input[1].has_mmap = 1; // already loaded map fixture
	}
	for (int n = 0; n < slot; n++) tap(1, KEY_EMU + 20 + n);
	observe("before skip");
}
int main(int argc, char **argv)
{
	if (argc != 5) return 2;
	std::string control = argv[1];
	bool alternative = std::atoi(argv[2]); int slot = std::atoi(argv[3]); bool reverse = std::atoi(argv[4]);
	if (control == "existing-suite") return existing_main();
	if (control == "placeholder")
	{
		setup(alternative, 3, false);
		std::strcpy(joy_bnames[0], "-"); std::strcpy(joy_bnames[1], "-");
		tap(1, KEY_EMU + 23);
		expect(get_map_button() == 6 && get_map_dev() == 1, "actual automatic placeholder loop must skip two named slots and retain selected device");
		expect((alternative ? input[1].map[3] >> 16 : input[1].map[3] & 0xFFFF) == KEY_EMU + 23,
			"automatic placeholder skip must preserve the preceding completed direction");
		expect((alternative ? input[1].map[4] >> 16 : input[1].map[4] & 0xFFFF) == 0
			&& (alternative ? input[1].map[5] >> 16 : input[1].map[5] & 0xFFFF) == 0,
			"automatic placeholder skip must clear only the skipped half");
		tap(1, BTN_NORTH);
		expect(get_map_button() == 7 && (alternative ? input[1].map[6] >> 16 : input[1].map[6] & 0xFFFF) == BTN_NORTH,
			"next real input after automatic placeholders must complete normally");
		if (alternative) expect((input[1].map[4] & 0xFFFF) == KEY_EMU + 4 && (input[1].map[5] & 0xFFFF) == KEY_EMU + 5,
			"alternate placeholder skips must retain existing base directions and buttons");
	}
	else if (control == "hardware-skip")
	{
		setup(alternative, 4, false); event(1, BTN_MODE, 1);
		send_map_cmd(KEY_ALTERASE);
		expect(get_map_button() == 5 && get_map_dev() == 1, "actual hardware skip dispatcher must retain target and advance once");
		expect((alternative ? input[1].map[4] >> 16 : input[1].map[4] & 0xFFFF) == 0,
			"hardware skip while a member is held must clear that pending slot");
		event(1, BTN_MODE, 0); tap(1, BTN_NORTH);
		expect(get_map_button() == 6 && (alternative ? input[1].map[5] >> 16 : input[1].map[5] & 0xFFFF) == BTN_NORTH,
			"hardware skip must release pending capture before the next ordinary assignment");
	}
	else if (control == "keyboard-remap")
	{
		begin(); start_map_setting(0);
		tap(0, KEY_A); tap(0, KEY_SPACE);
		expect(get_map_type() == 2 && input[0].kbdmap[KEY_A] == KEY_SPACE && get_map_button() == 0,
			"type-two remap must retain Space as target data rather than a skip command");
		event(0, KEY_B, 1); event(0, KEY_ALTERASE, 1); event(0, KEY_B, 0); tap(0, KEY_P);
		expect(get_map_type() == 2 && input[0].kbdmap[KEY_B] == KEY_P && get_map_button() == 0,
			"type-two Alterase early reset must retain its existing editor behavior");
	}
	else if (control == "core-definition")
	{
		begin(); menu_core = true; start_map_setting(SYS_BTN_CNT_ESC + 1);
		tap(0, KEY_A);
		expect(get_map_type() == 0 && get_map_button() == 0 && get_map_dev() == 0,
			"Menu-core first keyboard input must retain keyboard-definition selection and negative-phase bypass");
		tap(0, KEY_A); tap(0, KEY_ALTERASE); tap(0, KEY_B);
		expect(get_map_button() == 3 && input[0].map[0] == KEY_A && input[0].map[1] == 0 && input[0].map[2] == KEY_B,
			"Menu-core keyboard definition and accepted skip must retain raw slot semantics");
	}
	else if (control == "negative-skip" || control == "core-calibration")
	{
		begin(); menu_core = true; start_map_setting(SYS_BTN_CNT_ESC + 3);
		expect(get_map_button() == -6, "Menu-core definition must retain initial negative Dpad phase");
		if (control == "negative-skip")
		{
			tap(0, KEY_SPACE);
			expect(get_map_button() == -4 && get_map_dev() == -1 && tmp_axis_n == 0,
				"accepted negative-phase skip must retain pair stepping without selecting a keyboard target");
			fixture_absinfo.minimum = 0; fixture_absinfo.maximum = 255;
			event(1, 2, 255, EV_ABS);
			expect(get_map_button() == -3 && get_map_dev() == 1 && tmp_axis_n == 1 && tmp_axis[0] == (2 | 0x20000),
				"actual analog maximum after skip must retain source selection and packed-axis capture");
			send_map_cmd(KEY_ALTERASE);
			expect(get_map_button() == -2 && tmp_axis_n == 1 && input[1].map[SYS_AXIS1_Y] == 0,
				"negative analog-Y skip must retain existing axis-index clearing and stepping");
		}
		else
		{
			fixture_absinfo.minimum = 0; fixture_absinfo.maximum = 255;
			for (int axis = 0; axis < 6; axis++) event(1, axis, 255, EV_ABS);
			expect(get_map_button() == 0 && get_map_dev() == 1 && tmp_axis_n == 4,
				"actual Menu-core Dpad and stick maxima must complete six negative calibration stages");
			// This focused phase fixture supplies the digital-definition results;
			// the actual positive analog calibration branch then executes.
			mapping_button = 23; mapping_current_key = 0;
			event(1, 6, 255, EV_ABS); event(1, 6, 255, EV_ABS);
			expect(get_map_button() == 24 && input[1].map[SYS_AXIS_X] == (6 | 0x20000),
				"positive analog-X calibration must retain packed axis and reject duplicate X as Y");
			// Axes 7/8 are reserved for paddles in the production dispatcher.
			event(1, 9, 255, EV_ABS);
			expect(get_map_button() == 25 && input[1].map[SYS_AXIS_Y] == (9 | 0x20000),
				"positive analog-Y calibration must retain its distinct-axis rule and completion");
		}
	}
	else goto skip_cases;
	return complete();

skip_cases:
	bool one_button = control == "single-menu";
	setup(alternative, slot, one_button);
	uint32_t prior_low = input[1].map[slot] & 0xFFFF;
	if (control == "queued-space" || control == "queued-space-repeat")
	{
		event(0, KEY_SPACE, 1); event(1, BTN_NORTH, 1);
		observe("ordinary press while Space remains held");
		event(0, KEY_SPACE, control == "queued-space" ? 0 : 2);
		expect(mapping_current_key == BTN_NORTH && mapping_current_dev == 1,
			"unaccepted queued Space release or repeat must preserve the new ordinary capture");
		event(1, BTN_NORTH, 0);
		expect(get_map_button() == slot + 2, "new ordinary release after queued Space event must complete its slot");
		expect((alternative ? input[1].map[slot + 1] >> 16 : input[1].map[slot + 1] & 0xFFFF) == BTN_NORTH,
			"queued Space event must preserve assigned ordinary button data");
		expect(get_map_dev() == 1, "queued Space event must retain the selected ordinary input device");
		return complete();
	}
	if (control == "space") tap(0, KEY_SPACE);
	else
	{
		event(1, BTN_MODE, 1);
		if (!one_button) event(1, BTN_SOUTH, 1);
		observe("Menu press members queued");
		event(1, reverse && !one_button ? BTN_SOUTH : BTN_MODE, 0);
		if (!one_button) event(1, reverse ? BTN_MODE : BTN_SOUTH, 0);
	}
	observe("after skip releases");
	expect(get_map_button() == slot + 1, "skip must advance exactly one slot");
	expect((alternative ? input[1].map[slot] >> 16 : input[1].map[slot] & 0xFFFF) == 0,
		"skipped half must not retain a Menu-combo member");
	if (alternative) expect((input[1].map[slot] & 0xFFFF) == prior_low, "alternate skip must preserve base half");
	expect(get_map_dev() == ((alternative || slot || !one_button) ? 1 : -1),
		"accepted skip must retain an existing source and avoid selecting a target for a lone Menu command");
	tap(1, BTN_NORTH);
	observe("after next ordinary button tap");
	expect(get_map_button() == slot + 2, "ordinary button after skip must complete the next slot");
	expect((alternative ? input[1].map[slot + 1] >> 16 : input[1].map[slot + 1] & 0xFFFF) == BTN_NORTH,
		"ordinary button after skip must actually be assigned");
	expect(get_map_dev() == 1, "next ordinary assignment must retain the chosen source device");
	return complete();
}

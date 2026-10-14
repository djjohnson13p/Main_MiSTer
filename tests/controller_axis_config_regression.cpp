// SPDX-License-Identifier: GPL-3.0-or-later
// Ordinary actual-source integration fixture, prepared with AI assistance.
// No legacy deadzone setup, numeric conversion, file lexer or device loop runs.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#ifdef __linux__
#include <linux/input.h>
#else
// Only the unused ff_effect field in the intact devInput declaration maps here.
// This proves fixture state changes, not Linux/ARM struct size or device ABI.
struct FixtureOpaqueFFEffect { unsigned char unused[1]; };
#endif

#include "axis_config_types.inc"

struct GetterObservation { int dev, force; std::string value; };
static std::vector<GetterObservation> getter_observations;

#include "axis_config_cfg.inc"
#include "axis_config_setter.inc"
#include "axis_config_cases.inc"

static_assert(std::is_trivial<controller_axis::Entry>::value, "Entry is plain data");
static_assert(std::is_trivial<controller_axis::Coefficients>::value, "Coefficients is plain data");
static_assert(std::is_trivial<controller_axis::Identity>::value, "Identity is plain data");
static_assert(std::is_standard_layout<cfg_t>::value, "cfg field mask uses actual offsets");
static_assert(std::is_standard_layout<devInput>::value, "device field mask uses actual offsets");
static_assert(sizeof(cfg.controller_saturation) / sizeof(*cfg.controller_saturation) == 32, "actual S list32");
static_assert(sizeof(cfg.controller_multiplier) / sizeof(*cfg.controller_multiplier) == 32, "actual M list32");
static_assert(sizeof(cfg.controller_nonlinearity) / sizeof(*cfg.controller_nonlinearity) == 32, "actual E list32");

static int checks = 0, failures = 0, parsed_returns = 0, setter_returns = 0;
static bool legacy_cfg_preserved = true, untargeted_cfg_preserved = true, setter_other_input_preserved = true;
struct RowObservation
{
    std::string delivered;
    int before_s, before_m, before_e, after_s, after_m, after_e;
    int errors_before, errors_after;
};
static std::vector<RowObservation> row_observations;
struct SetterObservation
{
    int dev;
    controller_axis::Coefficients before, after;
    std::size_t getter_before, getter_after;
};
static std::vector<SetterObservation> setter_observations;

static void check(bool value, const char *label)
{
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}

static int count(const controller_axis::Entry *entries)
{
    int result = 0;
    while (result < 32 && entries[result].selector[0]) ++result;
    return result;
}

static std::vector<unsigned char> legacy_cfg_bytes()
{
    std::vector<unsigned char> bytes(sizeof(cfg));
    std::memcpy(bytes.data(), &cfg, sizeof(cfg));
    std::memset(bytes.data() + offsetof(cfg_t, controller_saturation), 0, sizeof(cfg.controller_saturation));
    std::memset(bytes.data() + offsetof(cfg_t, controller_multiplier), 0, sizeof(cfg.controller_multiplier));
    std::memset(bytes.data() + offsetof(cfg_t, controller_nonlinearity), 0, sizeof(cfg.controller_nonlinearity));
    return bytes;
}

static std::vector<unsigned char> legacy_input_bytes()
{
    std::vector<unsigned char> bytes(sizeof(input));
    std::memcpy(bytes.data(), input, sizeof(input));
    for (int dev = 0; dev < NUMDEV; ++dev)
        std::memset(bytes.data() + sizeof(devInput) * static_cast<std::size_t>(dev) + offsetof(devInput, axis_options), 0, sizeof(input[dev].axis_options));
    return bytes;
}

static bool same(const controller_axis::Coefficients &value, int s, double m, double e)
{
    return value.saturation == s && value.multiplier == m && value.nonlinearity == e;
}

static void parse_row(const std::string &delivered)
{
    // This is already delivered text. No claim of original raw INI spelling.
    std::vector<char> writable(delivered.begin(), delivered.end());
    writable.push_back(0);
    const auto legacy_before = legacy_cfg_bytes();
    std::vector<unsigned char> full_before(sizeof(cfg));
    std::memcpy(full_before.data(), &cfg, sizeof(cfg));
    RowObservation record = {delivered, count(cfg.controller_saturation), count(cfg.controller_multiplier), count(cfg.controller_nonlinearity), 0, 0, 0, cfg_error_count, 0};
    ini_parse_var(writable.data());
    ++parsed_returns; // Actual void function completed; no invented API status.
    record.after_s = count(cfg.controller_saturation);
    record.after_m = count(cfg.controller_multiplier);
    record.after_e = count(cfg.controller_nonlinearity);
    record.errors_after = cfg_error_count;
    row_observations.push_back(record);
    const bool preserved = legacy_before == legacy_cfg_bytes();
    legacy_cfg_preserved &= preserved;
    check(preserved, "new delivered row preserves all other cfg bytes");
    const ini_var_t *selected = nullptr;
    for (const auto &var : ini_vars)
        if (!strcasecmp(var.name, writable.data())) selected = &var;
    if (!selected) throw std::runtime_error("fixture delivered key did not resolve");
    std::size_t offset, size;
    if (selected->type == AXIS_SATURATION)
    { offset = offsetof(cfg_t, controller_saturation); size = sizeof(cfg.controller_saturation); }
    else if (selected->type == AXIS_MULTIPLIER)
    { offset = offsetof(cfg_t, controller_multiplier); size = sizeof(cfg.controller_multiplier); }
    else if (selected->type == AXIS_NONLINEARITY)
    { offset = offsetof(cfg_t, controller_nonlinearity); size = sizeof(cfg.controller_nonlinearity); }
    else throw std::runtime_error("fixture attempted a non-axis option");
    std::vector<unsigned char> full_after(sizeof(cfg));
    std::memcpy(full_after.data(), &cfg, sizeof(cfg));
    std::memset(full_before.data() + offset, 0, size);
    std::memset(full_after.data() + offset, 0, size);
    const bool untargeted = full_before == full_after;
    untargeted_cfg_preserved &= untargeted;
    check(untargeted, "delivered row preserves every cfg byte outside its own list");
}

static void open_snapshot(int dev)
{
    const auto legacy_before = legacy_input_bytes();
    const auto cfg_before = legacy_cfg_bytes();
    SetterObservation record = {dev, input[dev].axis_options, {}, getter_observations.size(), 0};
    setup_controller_axes(dev);
    ++setter_returns; // Direct actual setter boundary, not whole enumeration.
    record.after = input[dev].axis_options;
    record.getter_after = getter_observations.size();
    setter_observations.push_back(record);
    const bool preserved = legacy_before == legacy_input_bytes();
    setter_other_input_preserved &= preserved;
    check(preserved, "setter preserves every input byte outside coefficient fields");
    check(cfg_before == legacy_cfg_bytes(), "setter preserves legacy cfg bytes");
}

static void reset()
{
    // Declared initial state boundary; full cfg_parse/device initialization excluded.
    std::memset(&cfg, 0, sizeof(cfg));
    std::memset(input, 0, sizeof(input));
    std::memset(cfg_errors, 0, sizeof(cfg_errors));
    cfg_error_count = 0;
    fixture_selected_section_boundary();
    std::strcpy(cfg.controller_deadzone[0], "unchanged existing setting");
    std::strcpy(cfg.bootcore, "unchanged core");
    cfg.video_saturation = 71;
    for (int dev = 0; dev < 2; ++dev)
    {
        input[dev].vid = static_cast<uint16_t>(0x1e8f + dev);
        input[dev].pid = static_cast<uint16_t>(0x1603 + dev);
        input[dev].bind = dev;
        input[dev].num = static_cast<uint8_t>(dev + 1);
        input[dev].unique_hash = static_cast<uint32_t>(0x1234 + dev);
        std::snprintf(input[dev].id, sizeof(input[dev].id), "usb-1.%d/Pad", dev + 2);
        std::snprintf(input[dev].sysfs, sizeof(input[dev].sysfs), "/devices/controller%d/input", dev);
        std::snprintf(input[dev].idstr, sizeof(input[dev].idstr), "%04x_%04x", input[dev].vid, input[dev].pid);
        input[dev].axis_options = {0, 1.0, 1.0};
        input[dev].deadzone = 10;
        input[dev].max_cardinal[0] = 127;
        input[dev].ss_range[1] = 123;
        input[dev].advanced_last_pressed_keycode = 61;
    }
}

static void initial_three()
{
    parse_row("saturation=0x1E8F1603,10");
    parse_row("multiplier=0x1E8F1603,0.5");
    parse_row("nonlinearity=0x1E8F1603,1.5");
}

static void execute_case(const std::string &name)
{
    reset();
    if (name == "delivered_three_options")
    {
        initial_three();
        check(count(cfg.controller_saturation) == 1 && cfg.controller_saturation[0].value == 10.0, "actual S table and case");
        check(count(cfg.controller_multiplier) == 1 && cfg.controller_multiplier[0].value == .5, "actual M table and case");
        check(count(cfg.controller_nonlinearity) == 1 && cfg.controller_nonlinearity[0].value == 1.5, "actual E table and case");
        check(cfg_error_count == 0, "ordinary three settings no diagnostics");
    }
    else if (name == "delivered_whitespace_case")
    {
        parse_row("SaTuRaTiOn \t=  vid:0x1E8F , 010  ");
        parse_row("MULTIPLIER=pid:1603\t+5e-1\t");
        parse_row("nonlinearity = usb-1.2/ , 1.5");
        open_snapshot(0);
        check(same(input[0].axis_options, 10, .5, 1.5), "actual parser whitespace/case plus module syntax");
    }
    else if (name == "invalid_first_resets_only_own")
    {
        initial_three();
        fixture_selected_section_boundary();
        parse_row("saturation=Pad,65");
        check(count(cfg.controller_saturation) == 0, "first invalid row clears its replacing list");
        check(count(cfg.controller_multiplier) == 1 && count(cfg.controller_nonlinearity) == 1, "other lists not reset by new-key occurrence");
        check(cfg_error_count == 1 && std::strstr(cfg_errors[0], "SATURATION"), "actual invalid diagnostic names S");
        parse_row("saturation=Pad,12");
        check(count(cfg.controller_saturation) == 1 && cfg.controller_saturation[0].value == 12.0, "valid row follows invalid with no consumed slot");
    }
    else if (name == "invalid_later_preserves_rows")
    {
        parse_row("multiplier=Pad,0.5");
        parse_row("multiplier=Pad,5");
        parse_row("multiplier=controller,2");
        check(count(cfg.controller_multiplier) == 2 && cfg.controller_multiplier[0].value == .5 && cfg.controller_multiplier[1].value == 2.0, "later invalid leaves earlier row and consumes no slot");
        check(cfg_error_count == 1, "one ordinary unsupported-row diagnostic");
    }
    else if (name == "rejected_rows_no_slots")
    {
        parse_row("saturation=Pad,-1");
        parse_row("multiplier=Pad,not-a-number");
        parse_row("nonlinearity=Pad,4");
        check(count(cfg.controller_saturation) == 0 && count(cfg.controller_multiplier) == 0 && count(cfg.controller_nonlinearity) == 0, "unsupported rows accept no entries");
        check(cfg_error_count == 3, "three normal diagnostics observed");
        initial_three();
        check(count(cfg.controller_saturation) == 1 && count(cfg.controller_multiplier) == 1 && count(cfg.controller_nonlinearity) == 1, "ordinary rows recover independently");
    }
    else if (name == "capacity_32_diagnostic")
    {
        for (int n = 0; n < 32; ++n) parse_row("multiplier=Pad" + std::to_string(n) + ",2");
        const auto last = cfg.controller_multiplier[31];
        parse_row("multiplier=Last,3");
        check(count(cfg.controller_multiplier) == 32, "actual full list stays at32");
        check(std::strcmp(last.selector, cfg.controller_multiplier[31].selector) == 0 && last.value == cfg.controller_multiplier[31].value, "full-list diagnostic preserves last accepted row");
        check(cfg_error_count == 1 && std::strcmp(cfg_errors[0], "MULTIPLIER: too many controller settings") == 0, "exact actual full-list diagnostic");
    }
    else if (name == "selected_section_replaces")
    {
        parse_row("saturation=Pad,10");
        parse_row("saturation=controller,11");
        fixture_selected_section_boundary();
        parse_row("saturation=usb,12");
        parse_row("saturation=Pad,13");
        check(count(cfg.controller_saturation) == 2 && cfg.controller_saturation[0].value == 12.0 && cfg.controller_saturation[1].value == 13.0, "actual selected-section flag reset restarts then appends");
    }
    else if (name == "independent_first_match")
    {
        parse_row("saturation=unmatched,20"); parse_row("saturation=Pad,10"); parse_row("saturation=usb-1.2/,30");
        parse_row("multiplier=pid:1603,.5"); parse_row("multiplier=Pad,2");
        parse_row("nonlinearity=controller0,1.5"); parse_row("nonlinearity=Pad,3");
        open_snapshot(0);
        check(same(input[0].axis_options, 10, .5, 1.5), "first matching row independently per option, no specificity ranking");
        check(getter_observations.size() == 1, "one actual identity getter per nonempty setter");
    }
    else if (name == "empty_lists_defaults")
    {
        input[0].axis_options = {30, 3.0, 2.0};
        open_snapshot(0);
        check(same(input[0].axis_options, 0, 1.0, 1.0), "absent lists retire stale coefficients");
        check(getter_observations.empty(), "absent lists bypass actual identity getter");
    }
    else if (name == "explicit_identity")
    {
        parse_row("saturation=Pad,0"); parse_row("multiplier=Pad,1"); parse_row("nonlinearity=Pad,1");
        open_snapshot(0);
        check(same(input[0].axis_options, 0, 1.0, 1.0), "explicit rows produce exact identity tuple");
        check(getter_observations.size() == 1 && getter_observations[0].force == 1, "explicit identity matching uses actual getter");
    }
    else if (name == "numeric_selector_families")
    {
        parse_row("saturation=0X1e8f1603,10"); parse_row("multiplier=ViD:1e8f,.5"); parse_row("nonlinearity=PID:0x1603,1.5");
        open_snapshot(0);
        check(same(input[0].axis_options, 10, .5, 1.5), "ordinary VIDPID/VID/PID families through setter");
    }
    else if (name == "literal_id_sysfs_unique")
    {
        parse_row("saturation=USB-1.2/,10"); parse_row("multiplier=CONTROLLER0,.5"); parse_row("nonlinearity=1E8F_1603,1.5");
        open_snapshot(0);
        check(same(input[0].axis_options, 10, .5, 1.5), "actual id/sysfs/unique case-insensitive identities");
        check(getter_observations.size() == 1 && getter_observations[0].value == "1e8f_1603", "actual unconfigured unique spelling");
    }
    else if (name == "unique_unconfigured")
    {
        parse_row("saturation=1e8f_1603_00001234,30"); parse_row("saturation=1e8f_1603,5");
        open_snapshot(0);
        check(same(input[0].axis_options, 5, 1.0, 1.0), "unconfigured helper does not invent unique hash");
        check(getter_observations[0].value == "1e8f_1603", "actual no-hash output captured");
    }
    else if (name == "unique_configured" || name == "unique_force_nonmatching_config")
    {
        cfg.controller_unique_mapping[0] = 1;
        cfg.controller_unique_mapping[1] = name == "unique_configured" ? 1 : 0x12345678;
        parse_row("saturation=1e8f_1603_00001234,30"); parse_row("saturation=1e8f_1603,5");
        open_snapshot(0);
        check(same(input[0].axis_options, 30, 1.0, 1.0), "actual force_unique behavior selects configured hash");
        check(getter_observations.size() == 1 && getter_observations[0].value == "1e8f_1603_00001234" && getter_observations[0].force == 1, "actual getter arguments and hash spelling");
    }
    else if (name == "two_devices_independent")
    {
        parse_row("saturation=0x1e8f1603,10"); parse_row("saturation=0x1e901604,20");
        parse_row("multiplier=controller0,.5"); parse_row("multiplier=controller1,2");
        open_snapshot(0); open_snapshot(1);
        check(same(input[0].axis_options, 10, .5, 1.0) && same(input[1].axis_options, 20, 2.0, 1.0), "independent actual device coefficient snapshots");
        check(getter_observations.size() == 2 && getter_observations[0].value == "1e8f_1603" && getter_observations[1].value == "1e90_1604", "transient getter identity not retained across device setup");
    }
    else if (name == "delivered_cfg_update_without_reopen" || name == "reopen_refresh")
    {
        initial_three(); open_snapshot(0);
        fixture_selected_section_boundary();
        parse_row("saturation=Pad,12"); parse_row("multiplier=Pad,2"); parse_row("nonlinearity=Pad,3");
        check(same(input[0].axis_options, 10, .5, 1.5), "actual delivered cfg updates do not change active device snapshot");
        check(setter_returns == 1, "no implicit setter call during delivered cfg update");
        if (name == "reopen_refresh")
        {
            open_snapshot(0);
            check(same(input[0].axis_options, 12, 2.0, 3.0), "next actual setter invocation refreshes snapshot");
        }
    }
    else if (name == "reopen_absent_defaults")
    {
        initial_three(); open_snapshot(0);
        // Config-list absence boundary only; this does not execute full cfg_parse.
        std::memset(cfg.controller_saturation, 0, sizeof(cfg.controller_saturation));
        std::memset(cfg.controller_multiplier, 0, sizeof(cfg.controller_multiplier));
        std::memset(cfg.controller_nonlinearity, 0, sizeof(cfg.controller_nonlinearity));
        const auto getter_before = getter_observations.size();
        check(same(input[0].axis_options, 10, .5, 1.5), "list absence alone leaves active snapshot");
        open_snapshot(0);
        check(same(input[0].axis_options, 0, 1.0, 1.0), "reopen resets absent values to full identity");
        check(getter_observations.size() == getter_before, "absent reopen has no extra getter");
    }
    else if (name == "setter_preserves_legacy_fields")
    {
        initial_three();
        input[0].bind = 1; // Setter API receives dev0 explicitly; no binding-redirect claim.
        input[0].lightgun = 1;
        open_snapshot(0);
        check(same(input[0].axis_options, 10, .5, 1.5), "setter remains event independent for supplied device");
        check(input[0].deadzone == 10 && input[0].lightgun == 1 && input[0].bind == 1 && input[0].max_cardinal[0] == 127 && input[0].ss_range[1] == 123, "legacy calibration/player/mode fields retained");
        check(same(input[1].axis_options, 0, 1.0, 1.0), "unselected device unchanged");
    }
    else throw std::runtime_error("unknown exact case");
}

static std::string quote(const std::string &text)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(c) << std::dec;
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}

static std::string coeff(const controller_axis::Coefficients &value)
{
    std::ostringstream out;
    out << std::setprecision(17) << '[' << value.saturation << ',' << value.multiplier << ',' << value.nonlinearity << ']';
    return out.str();
}

static std::string entries(const controller_axis::Entry *values)
{
    std::ostringstream out;
    out << '[';
    for (int n = 0; n < 32; ++n)
    {
        if (n) out << ',';
        std::size_t length = 0;
        while (length < sizeof(values[n].selector) && values[n].selector[length]) ++length;
        out << "{\"selector\":" << quote(std::string(values[n].selector, length))
            << ",\"value\":" << std::setprecision(17) << values[n].value << '}';
    }
    out << ']';
    return out.str();
}

static void report(const std::string &name)
{
    std::ostringstream out;
    out << "CASE_JSON={\"name\":" << quote(name) << ",\"assertions\":" << checks
        << ",\"failed_assertions\":" << failures << ",\"parse_void_returns\":" << parsed_returns
        << ",\"setter_void_returns\":" << setter_returns
        << ",\"legacy_cfg_bytes_preserved\":" << (legacy_cfg_preserved ? "true" : "false")
        << ",\"untargeted_cfg_bytes_preserved\":" << (untargeted_cfg_preserved ? "true" : "false")
        << ",\"setter_other_input_bytes_preserved\":" << (setter_other_input_preserved ? "true" : "false")
        << ",\"lists\":{\"saturation\":" << entries(cfg.controller_saturation)
        << ",\"multiplier\":" << entries(cfg.controller_multiplier)
        << ",\"nonlinearity\":" << entries(cfg.controller_nonlinearity) << "},\"errors\":[";
    for (int i = 0; i < cfg_error_count; ++i) { if (i) out << ','; out << quote(cfg_errors[i]); }
    out << "],\"getter_calls\":[";
    for (std::size_t i = 0; i < getter_observations.size(); ++i)
    {
        if (i) out << ',';
        const auto &g = getter_observations[i];
        out << "{\"dev\":" << g.dev << ",\"force\":" << g.force << ",\"value\":" << quote(g.value) << '}';
    }
    out << "],\"setters\":[";
    for (std::size_t i = 0; i < setter_observations.size(); ++i)
    {
        if (i) out << ',';
        const auto &s = setter_observations[i];
        out << "{\"dev\":" << s.dev << ",\"before\":" << coeff(s.before) << ",\"after\":" << coeff(s.after)
            << ",\"getter_before\":" << s.getter_before << ",\"getter_after\":" << s.getter_after << '}';
    }
    out << "],\"delivered_rows\":[";
    for (std::size_t i = 0; i < row_observations.size(); ++i)
    {
        if (i) out << ',';
        const auto &r = row_observations[i];
        out << "{\"delivered\":" << quote(r.delivered) << ",\"before_counts\":[" << r.before_s << ',' << r.before_m << ',' << r.before_e
            << "],\"after_counts\":[" << r.after_s << ',' << r.after_m << ',' << r.after_e
            << "],\"errors_before\":" << r.errors_before << ",\"errors_after\":" << r.errors_after << '}';
    }
    out << "],\"active_devices\":[" << coeff(input[0].axis_options) << ',' << coeff(input[1].axis_options) << "]}";
    std::cout << out.str() << '\n';
}

int main(int argc, char **argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("one catalog index required");
        char *end = nullptr;
        const long index = std::strtol(argv[1], &end, 10);
        if (!end || *end || index < 0 || static_cast<std::size_t>(index) >= sizeof(case_names) / sizeof(*case_names))
            throw std::runtime_error("invalid catalog index");
        const std::string name = case_names[index];
        execute_case(name);
        check(legacy_cfg_preserved && untargeted_cfg_preserved && setter_other_input_preserved, "whole declared legacy state preservation");
        report(name);
        return failures ? 1 : 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FIXTURE_ERROR " << error.what() << '\n';
        return 2;
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// External normal-input checks for the AI-assisted experimental module prototype.
// This does not execute cfg.cpp, setup_deadzone, input_cb or any hardware code.
#include "controller_axis.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <type_traits>

using controller_axis::Coefficients;
using controller_axis::Entry;
using controller_axis::Identity;
using controller_axis::Kind;
using controller_axis::SATURATION;
using controller_axis::MULTIPLIER;
using controller_axis::NONLINEARITY;

static_assert(std::is_trivial<Entry>::value && std::is_standard_layout<Entry>::value, "Entry must remain POD");
static_assert(std::is_trivial<Identity>::value && std::is_standard_layout<Identity>::value, "Identity must remain POD");
static_assert(std::is_trivial<Coefficients>::value && std::is_standard_layout<Coefficients>::value, "Coefficients must remain POD");

namespace
{
unsigned int checks = 0;
unsigned int failures = 0;

void check(bool condition, const char *name)
{
	++checks;
	if (!condition)
	{
		++failures;
		std::printf("FAIL %s\n", name);
	}
}

void equal(int actual, int expected, const char *name)
{
	++checks;
	if (actual != expected)
	{
		++failures;
		std::printf("FAIL %s actual=%d expected=%d\n", name, actual, expected);
	}
}

void completed(const char *name, unsigned int before)
{
	std::printf("COMPLETED %s checks=%u\n", name, checks - before);
}

void accepted(Kind kind, const char *row, const char *selector, double value)
{
	Entry result = {};
	const bool ok = controller_axis::parse(kind, row, result);
	check(ok, row);
	check(std::strcmp(result.selector, selector) == 0, row);
	check(result.value == value, row);
}

void rejected(Kind kind, const char *row)
{
	Entry result = { "unchanged-selector", 2.25 };
	const Entry before = result;
	check(!controller_axis::parse(kind, row, result), row);
	check(std::memcmp(result.selector, before.selector, sizeof(result.selector)) == 0 && result.value == before.value, "rejection preserves entry");
}

Entry entry(Kind kind, const char *row)
{
	Entry result = {};
	check(controller_axis::parse(kind, row, result), row);
	return result;
}

void parsing()
{
	const unsigned int before = checks;
	accepted(SATURATION, "045e028e,0", "045e028e", 0.0);
	accepted(SATURATION, "vid:045e 64", "vid:045e", 64.0);
	accepted(SATURATION, "\tX-Box\t, 002 \t", "X-Box", 2.0);
	accepted(MULTIPLIER, "pad,0", "pad", 0.0);
	accepted(MULTIPLIER, "pad +0", "pad", 0.0);
	accepted(MULTIPLIER, "pad,4", "pad", 4.0);
	accepted(MULTIPLIER, "pad .5", "pad", 0.5);
	accepted(MULTIPLIER, "pad 1.", "pad", 1.0);
	accepted(MULTIPLIER, "pad +1.25", "pad", 1.25);
	accepted(MULTIPLIER, "pad 4e-1", "pad", 0.4);
	accepted(MULTIPLIER, "pad 2E+0", "pad", 2.0);
	accepted(NONLINEARITY, "pad,1", "pad", 1.0);
	accepted(NONLINEARITY, "pad\t3", "pad", 3.0);
	accepted(NONLINEARITY, "pad +2.25", "pad", 2.25);
	accepted(NONLINEARITY, "pad 1.5e0", "pad", 1.5);
	accepted(NONLINEARITY, "pad 2E+0", "pad", 2.0);
	// A previous unrelated conversion error must not reject a valid new row.
	errno = ERANGE;
	accepted(MULTIPLIER, "pad 1", "pad", 1.0);

	const char *bad_s[] = { "pad -1", "pad 65", "pad +1", "pad 1.0", "pad 1e1", "pad word", "pad 4extra" };
	for (const char *row : bad_s) rejected(SATURATION, row);
	const char *bad_m[] = { "pad -0", "pad -1", "pad 4.1", "pad 1e3", "pad nan", "pad inf", "pad 0x1p0", "pad 1,25", "pad 1e", "pad ." };
	for (const char *row : bad_m) rejected(MULTIPLIER, row);
	const char *bad_e[] = { "pad 0.9", "pad 3.1", "pad -0", "pad 3e1", "pad curve" };
	for (const char *row : bad_e) rejected(NONLINEARITY, row);
	const char *bad_rows[] = { "", "pad", "pad,", ",1", "pad 1 2", "pad 1," };
	for (const char *row : bad_rows) rejected(MULTIPLIER, row);
	completed("delivered_rows_normal_values_and_unsupported_values", before);
}

void selectors_and_lists()
{
	const unsigned int before = checks;
	const Identity xbox = { 0x045e, 0x028e, "Microsoft X-Box 360 pad", "/devices/pci0000:00/usb1/1-2/input0", "XBOX_045e028e_2f6d3b10" };
	const char *matches[] = { "045e028e", "0X045E028E", "VID:0x045E", "vid:45e", "PiD:028e", "pid:0X28E", "x-BoX", "USB1/1-2", "_2F6D3B10" };
	for (const char *selector : matches) check(controller_axis::selector_matches(selector, xbox), selector);
	const char *misses[] = { "054c09cc", "vid:054c", "pid:09cc", "other-controller", "_different_hash", "" };
	for (const char *selector : misses) check(!controller_axis::selector_matches(selector, xbox), selector);
	const Identity small = { 0, 0x000a, "small pad", "", "small" };
	check(controller_axis::selector_matches("a", small), "one digit combined hex");
	check(controller_axis::selector_matches("pid:0xa", small), "one digit pid hex");
	check(controller_axis::selector_matches("vid:0", small), "one digit vid hex");
	const Identity misleading = { 0x054c, 0x09cc, "045e028e vid:045e pid:028e", "0x045e028e", "045e028e" };
	check(!controller_axis::selector_matches("045e028e", misleading), "combined mismatch has no literal fallthrough");
	check(!controller_axis::selector_matches("0x045e028e", misleading), "prefixed combined mismatch has no literal fallthrough");
	check(!controller_axis::selector_matches("vid:045e", misleading), "vid mismatch has no literal fallthrough");
	check(!controller_axis::selector_matches("pid:028e", misleading), "pid mismatch has no literal fallthrough");
	// Other syntax is a literal selector, not a scanf conversion or a wildcard.
	const Identity literal = { 1, 2, "pad[1]*", "", "current_unique_text" };
	check(controller_axis::selector_matches("[1]*", literal), "literal punctuation");
	check(!controller_axis::selector_matches("pad*", literal), "no wildcard matching");
	check(controller_axis::selector_matches("unique_text", literal), "uses supplied unique identity");

	Entry saturation[4] = { entry(SATURATION, "other-pad 8"), entry(SATURATION, "x-box 16"), entry(SATURATION, "045e028e 32"), {} };
	Entry multiplier[3] = { entry(MULTIPLIER, "vid:045e 1.5"), entry(MULTIPLIER, "x-box 2"), {} };
	Entry curve[3] = { entry(NONLINEARITY, "pid:028e 2"), entry(NONLINEARITY, "x-box 3"), {} };
	const Coefficients snapshot = {
		static_cast<int>(controller_axis::first_match(saturation, 4, xbox, 0.0)),
		controller_axis::first_match(multiplier, 3, xbox, 1.0),
		controller_axis::first_match(curve, 3, xbox, 1.0)
	};
	check(snapshot.saturation == 16 && snapshot.multiplier == 1.5 && snapshot.nonlinearity == 2.0, "independent first accepted matches");
	check(controller_axis::first_match(saturation, 1, xbox, 0.0) == 0.0, "count boundary returns default");
	Entry empty[32] = {};
	check(controller_axis::first_match(empty, 32, xbox, 0.0) == 0.0, "empty saturation list default");
	check(controller_axis::first_match(empty, 32, xbox, 1.0) == 1.0, "empty multiplier/curve list default");
	Entry terminated[3] = { entry(MULTIPLIER, "other-pad 2"), {}, entry(MULTIPLIER, "x-box 3") };
	check(controller_axis::first_match(terminated, 3, xbox, 1.0) == 1.0, "empty selector ends accepted rows");
	completed("ordinary_selector_families_and_independent_ordered_lists", before);
}

void named_math()
{
	const unsigned int before = checks;
	struct Named
	{
		const char *name;
		int component;
		bool psx;
		Coefficients coefficients;
		int expected;
	};
	const Named cases[] = {
		{ "saturation_positive_endpoint", 111, false, {16, 1.0, 1.0}, 127 },
		{ "saturation_positive_interior", 55, false, {16, 1.0, 1.0}, 63 },
		{ "saturation_negative_interior", -55, false, {16, 1.0, 1.0}, -63 },
		{ "saturation_psx_negative_endpoint", -112, true, {16, 1.0, 1.0}, -128 },
		{ "saturation_psx_negative_interior", -56, true, {16, 1.0, 1.0}, -64 },
		{ "half_gain_positive_tie", 127, false, {0, 0.5, 1.0}, 64 },
		{ "half_gain_negative_tie", -127, false, {0, 0.5, 1.0}, -64 },
		{ "half_gain_psx_negative", -128, true, {0, 0.5, 1.0}, -64 },
		{ "double_gain_before_clamp", 63, false, {0, 2.0, 1.0}, 126 },
		{ "double_gain_clamped", 64, false, {0, 2.0, 1.0}, 127 },
		{ "square_positive", 64, false, {0, 1.0, 2.0}, 32 },
		{ "square_negative", -64, false, {0, 1.0, 2.0}, -32 },
		{ "square_psx_negative", -64, true, {0, 1.0, 2.0}, -32 },
		{ "cube_positive", 64, false, {0, 1.0, 3.0}, 16 },
		{ "cube_psx_negative", -64, true, {0, 1.0, 3.0}, -16 },
		{ "fractional_curve_positive", 64, false, {0, 1.0, 1.5}, 45 },
		{ "fractional_curve_psx_negative", -64, true, {0, 1.0, 1.5}, -45 },
		{ "combined_positive", 74, false, {16, 1.5, 2.0}, 85 },
		{ "combined_negative", -74, false, {16, 1.5, 2.0}, -85 },
		{ "combined_psx_negative", -56, true, {16, 1.5, 2.0}, -48 },
		{ "limits_positive_endpoint", 63, false, {64, 4.0, 3.0}, 127 },
		{ "limits_psx_negative_endpoint", -64, true, {64, 4.0, 3.0}, -128 },
		{ "limits_interior", 21, false, {64, 4.0, 3.0}, 19 },
		{ "active_zero", 0, false, {64, 4.0, 3.0}, 0 },
		{ "positive_endpoint_clamp", 127, true, {64, 4.0, 3.0}, 127 }
	};
	for (const Named &test : cases)
	{
		equal(controller_axis::transform(test.component, test.psx, test.coefficients), test.expected, test.name);
	}
	completed("named_saturation_gain_curve_combination_and_rounding", before);
}

void finite_travel()
{
	const unsigned int before = checks;
	const Coefficients defaults = { 0, 1.0, 1.0 };
	const Coefficients explicit_identity = {
		static_cast<int>(entry(SATURATION, "pad 0").value),
		entry(MULTIPLIER, "pad 1").value,
		entry(NONLINEARITY, "pad 1").value
	};
	const Coefficients active[] = {
		{16, 1.0, 1.0}, {0, 0.0, 1.0}, {0, 0.5, 1.0},
		{0, 2.0, 1.0}, {0, 1.0, 2.0}, {0, 1.0, 3.0},
		{16, 1.5, 2.0}, {64, 4.0, 3.0}, {8, 1.2, 1.5}
	};
	for (int psx = 0; psx <= 1; ++psx)
	{
		const int minimum = psx ? -128 : -127;
		for (int a = minimum; a <= 127; ++a)
		{
			equal(controller_axis::transform(a, psx != 0, defaults), a, "absent/default identity full finite range");
			equal(controller_axis::transform(a, psx != 0, explicit_identity), a, "explicit identity full finite range");
		}
		for (const Coefficients &coefficients : active)
		{
			int previous = minimum;
			for (int a = minimum; a <= 127; ++a)
			{
				const int result = controller_axis::transform(a, psx != 0, coefficients);
				check(result >= minimum && result <= 127, "bounded active finite travel");
				check(result >= previous, "monotonic active finite travel");
				check((a < 0 && result <= 0) || (a > 0 && result >= 0) || (a == 0 && result == 0), "sign and zero active finite travel");
				if (coefficients.multiplier == 0.0) equal(result, 0, "zero gain full finite range");
				previous = result;
			}
		}
	}
	for (const Coefficients &coefficients : active)
	{
		for (int a = 0; a <= 127; ++a)
		{
			equal(controller_axis::transform(-a, false, coefficients), -controller_axis::transform(a, false, coefficients), "generic sign symmetry full finite range");
		}
	}
	completed("identity_and_active_full_finite_generic_psx_travel", before);
}
}

int main()
{
	parsing();
	selectors_and_lists();
	named_math();
	finite_travel();
	std::printf("%s controller_axis_module_checks checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}

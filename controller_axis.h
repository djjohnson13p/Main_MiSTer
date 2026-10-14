// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental MiSTer fork prototype, prepared with AI assistance.
#ifndef CONTROLLER_AXIS_H
#define CONTROLLER_AXIS_H

#include <cstddef>
#include <cstdint>

namespace controller_axis
{
enum Kind
{
	SATURATION,
	MULTIPLIER,
	NONLINEARITY
};

// Plain data: cfg/device storage is zeroed and copied by the existing code.
struct Entry
{
	char selector[256];
	double value;
};

struct Identity
{
	uint16_t vid;
	uint16_t pid;
	const char *id;
	const char *sysfs;
	// Pass the current get_unique_mapping(dev, 1) result, without rebuilding it.
	const char *unique;
};

struct Coefficients
{
	int saturation;
	double multiplier;
	double nonlinearity;
};

// Parse one complete value delivered by the existing INI lexer, at most 255
// bytes. On rejection, out is unchanged. Selector and number are separated by
// one or more commas/spaces/tabs; outer horizontal spaces are accepted.
// S: decimal digits, 0..64. M/E: optional +, decimal fraction/exponent, finite
// conversion, M 0..4 and E 1..3. Leading minus (including -0) is not accepted.
bool parse(Kind kind, const char *delivered, Entry &out);

// Valid numeric selector families take precedence and never fall through to
// substring matching on mismatch. Other selectors are literal case-insensitive
// substrings of the supplied id, sysfs or unique identity.
bool selector_matches(const char *selector, const Identity &identity);

// Entries are accepted rows in order, terminated by an empty selector or count.
// Call separately for each option with defaults 0, 1 and 1, respectively.
double first_match(const Entry *entries, std::size_t count,
	const Identity &identity, double fallback);

// Caller supplies defaults {0, 1.0, 1.0} or admitted coefficients and an actual
// post-deadzone component: [-127,127], or [-128,127] for PSX. Only local mapped
// stick components opt in. Raw state, downstream N64 shaping and routing remain
// caller responsibilities. The exact default tuple returns component directly.
int transform(int component, bool psx, const Coefficients &coefficients);
}

#endif

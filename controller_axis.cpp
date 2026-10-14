// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental MiSTer fork prototype, prepared with AI assistance.
#include "controller_axis.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace controller_axis
{
namespace
{
bool horizontal_space(char c)
{
	return c == ' ' || c == '\t';
}

bool delimiter(char c)
{
	return horizontal_space(c) || c == ',';
}

bool digit(char c)
{
	return c >= '0' && c <= '9';
}

char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool contains(const char *text, const char *needle)
{
	if (!text) return false;
	for (; *text; ++text)
	{
		const char *a = text;
		const char *b = needle;
		while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
		if (!*b) return true;
	}
	return false;
}

// Decimal grammar is checked before strtod so hex, inf, nan and minus-zero
// spellings are not part of this option's ordinary configuration syntax.
bool decimal(const char *text)
{
	const char *p = text;
	if (*p == '+') ++p;
	bool has_digit = false;
	while (digit(*p)) { has_digit = true; ++p; }
	if (*p == '.')
	{
		++p;
		while (digit(*p)) { has_digit = true; ++p; }
	}
	if (!has_digit) return false;
	if (*p == 'e' || *p == 'E')
	{
		++p;
		if (*p == '+' || *p == '-') ++p;
		if (!digit(*p)) return false;
		while (digit(*p)) ++p;
	}
	return !*p;
}

bool hex(const char *text, std::size_t max_digits, uint32_t &value)
{
	if (text[0] == '0' && lower(text[1]) == 'x') text += 2;
	const std::size_t length = std::strlen(text);
	if (!length || length > max_digits) return false;
	uint32_t result = 0;
	for (std::size_t i = 0; i < length; ++i)
	{
		const char c = lower(text[i]);
		unsigned int nibble;
		if (digit(c)) nibble = static_cast<unsigned int>(c - '0');
		else if (c >= 'a' && c <= 'f') nibble = static_cast<unsigned int>(c - 'a' + 10);
		else return false;
		result = (result << 4) | nibble;
	}
	value = result;
	return true;
}
}

bool parse(Kind kind, const char *delivered, Entry &out)
{
	if (!delivered) return false;
	const std::size_t length = std::strlen(delivered);
	if (!length || length > 255) return false;

	const char *selector = delivered;
	while (horizontal_space(*selector)) ++selector;
	const char *p = selector;
	while (*p && !delimiter(*p)) ++p;
	const std::size_t selector_length = static_cast<std::size_t>(p - selector);
	if (!selector_length || !*p) return false;
	while (delimiter(*p)) ++p;
	const char *number = p;
	while (*p && !horizontal_space(*p)) ++p;
	const std::size_t number_length = static_cast<std::size_t>(p - number);
	while (horizontal_space(*p)) ++p;
	if (!number_length || *p) return false;

	char token[256];
	std::memcpy(token, number, number_length);
	token[number_length] = 0;
	char *end = nullptr;
	double value;
	if (kind == SATURATION)
	{
		for (const char *q = token; *q; ++q) if (!digit(*q)) return false;
		errno = 0;
		const unsigned long integer = std::strtoul(token, &end, 10);
		if (errno || end == token || *end || integer > 64) return false;
		value = static_cast<double>(integer);
	}
	else if (kind == MULTIPLIER || kind == NONLINEARITY)
	{
		if (!decimal(token)) return false;
		errno = 0;
		value = std::strtod(token, &end);
		if (errno || end == token || *end || !std::isfinite(value)) return false;
		if (kind == MULTIPLIER ? (value < 0.0 || value > 4.0) : (value < 1.0 || value > 3.0)) return false;
	}
	else return false;

	Entry accepted = {};
	std::memcpy(accepted.selector, selector, selector_length);
	accepted.value = value;
	out = accepted;
	return true;
}

bool selector_matches(const char *selector, const Identity &identity)
{
	if (!selector || !*selector) return false;
	uint32_t numeric;
	if (hex(selector, 8, numeric))
	{
		return numeric == ((static_cast<uint32_t>(identity.vid) << 16) | identity.pid);
	}
	if (std::strlen(selector) >= 4 && lower(selector[1]) == 'i' &&
		lower(selector[2]) == 'd' && selector[3] == ':')
	{
		if (lower(selector[0]) == 'v' && hex(selector + 4, 4, numeric)) return numeric == identity.vid;
		if (lower(selector[0]) == 'p' && hex(selector + 4, 4, numeric)) return numeric == identity.pid;
	}
	return contains(identity.id, selector) || contains(identity.sysfs, selector) || contains(identity.unique, selector);
}

double first_match(const Entry *entries, std::size_t count,
	const Identity &identity, double fallback)
{
	for (std::size_t i = 0; i < count && entries[i].selector[0]; ++i)
	{
		if (selector_matches(entries[i].selector, identity)) return entries[i].value;
	}
	return fallback;
}

int transform(int component, bool psx, const Coefficients &coefficients)
{
	if (!component || (!coefficients.saturation && coefficients.multiplier == 1.0 && coefficients.nonlinearity == 1.0)) return component;
	const double bound = psx && component < 0 ? 128.0 : 127.0;
	const double u = std::fmin(std::fabs(static_cast<double>(component)) / (bound - coefficients.saturation), 1.0);
	const double v = std::fmin(coefficients.multiplier * std::pow(u, coefficients.nonlinearity), 1.0);
	const int magnitude = static_cast<int>(std::round(bound * v));
	return component < 0 ? -magnitude : magnitude;
}
}

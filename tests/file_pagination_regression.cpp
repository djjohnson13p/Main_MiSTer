#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <strings.h>
#include <vector>
#include "cfg.h"

cfg_t cfg = {};
static int osd_rows = 16;
int OsdGetSize() { return osd_rows; }

// Record first rows by their unique fixture identifier. The continuation
// consists only of 'x' characters, so it cannot be mistaken for another file.
static std::set<int> visible;
static std::set<int> highlighted;
void ScrollReset() { visible.clear(); highlighted.clear(); }
void OsdWriteOffset(unsigned char, const char *s, unsigned char selected,
	unsigned char, char, char, char = 0, int = 32, int = 0)
{
	if (s[1] == 'I' && std::isdigit(static_cast<unsigned char>(s[2])))
	{
		int item = std::atoi(s + 2);
		visible.insert(item);
		if (selected) highlighted.insert(item);
	}
}

static int fs_Options = 0;
static const char *home_dir = nullptr;
static char filter[256] = {};
#include "file_pagination_production.inc"

static int failures = 0;
static unsigned int transitions = 0;

static void check(bool condition, const std::string &message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		failures++;
	}
}

enum Pattern { LONG_FILES, SHORT_FILES, MIXED_ENTRIES, DESTINATION_EXPANDS };

static void make_list(int count, Pattern pattern)
{
	DirItem.assign(count, direntext_t{});
	const int lengths[] = {27, 28, 29, 54, 55};
	for (int i = 0; i < count; i++)
	{
		auto &entry = DirItem[i];
		char identifier[16];
		std::snprintf(identifier, sizeof(identifier), "I%04d ", i);
		int length = pattern == LONG_FILES ? 64 : pattern == SHORT_FILES ? 12 :
			pattern == DESTINATION_EXPANDS ? (i == 2 * osd_rows ? 64 : 12) : lengths[i % 5];
		std::string name(identifier);
		name.append(length - name.size(), 'x');
		std::snprintf(entry.altname, sizeof(entry.altname), "%s", name.c_str());
		entry.de.d_type = DT_REG;
		if (pattern == MIXED_ENTRIES)
		{
			if (i % 7 == 0) entry.de.d_type = DT_DIR;
			if (i % 7 == 1) std::strcpy(entry.datecode, "261013");
		}
	}
}

static std::set<int> render(int expand, const std::string &context)
{
	PrintDirectory(expand);
	check(visible.count(iSelectedEntry), context + ": selected item must remain visible");
	check(highlighted == std::set<int>{iSelectedEntry}, context + ": highlight must identify the selected file");
	check(iFirstEntry >= 0 && iFirstEntry <= flist_last_first_entry(), context + ": viewport must stay within the list");
	if (!visible.empty())
	{
		check(*visible.begin() >= 0 && *visible.rbegin() < (int)DirItem.size(), context + ": all rows must belong to the list");
		check((int)visible.size() == *visible.rbegin() - *visible.begin() + 1, context + ": rendered file indices must be contiguous");
	}
	return visible;
}

static void verify_transition(const std::set<int> &before, const std::set<int> &after,
	int direction, int previous, const std::string &context)
{
	transitions++;
	if (!before.empty() && !after.empty())
	{
		check(direction > 0 ? *after.begin() <= *before.rbegin() + 1 : *before.begin() <= *after.rbegin() + 1,
			context + ": consecutive pages must not skip a file (direction=" + std::to_string(direction)
			+ " selected=" + std::to_string(previous) + "->" + std::to_string(iSelectedEntry)
			+ " before=" + std::to_string(*before.begin()) + ".." + std::to_string(*before.rbegin())
			+ " after=" + std::to_string(*after.begin()) + ".." + std::to_string(*after.rbegin()) + ")");
	}
}

static void traverse(int direction, int expand, const std::string &context,
	bool original = false)
{
	iFirstEntry = 0;
	iSelectedEntry = direction > 0 ? 0 : (int)DirItem.size() - 1;
	flist_center_selected();
	std::set<int> seen;
	auto before = render(expand, context);
	seen.insert(before.begin(), before.end());
	bool finished = false;
	for (size_t page = 0; page <= DirItem.size(); page++)
	{
		int previous = iSelectedEntry;
		scan_navigation(direction > 0 ? SCANF_NEXT_PAGE : SCANF_PREV_PAGE, "BIN", 0);
		auto after = render(expand, context);
		seen.insert(after.begin(), after.end());
		verify_transition(before, after, direction, previous, context);
		if (previous == iSelectedEntry)
		{
			finished = true;
			break;
		}
		before = after;
	}
	check(finished, context + ": paging must reach a boundary without looping");
	std::vector<int> missing;
	for (int i = 0; i < (int)DirItem.size(); i++) if (!seen.count(i)) missing.push_back(i);
	if (original)
	{
		std::printf("Issue 1251 original fixture, missing indices:");
		for (int item : missing) std::printf(" %d", item);
		std::puts("");
		check(seen.count(23) && seen.count(39), "issue 1251: historical skipped entries 23 and 39 must appear");
	}
	check(missing.empty(), context + ": repeated paging must display every file");
}

static void original_report()
{
	osd_rows = 16;
	cfg.lookahead = 1;
	cfg.browse_expand = 1;
	cfg.rbf_hide_datecode = 0;
	make_list(60, LONG_FILES);
	traverse(1, 1, "original 60-long-filename report", true);
}

static void focused_boundaries()
{
	// These list lengths exercise a short list, exact page boundary, one item
	// over it, and a middle page that can consume an expanded selection row.
	for (int rows : {8, 16})
	for (int lookahead : {0, 1})
	{
		osd_rows = rows;
		cfg.lookahead = lookahead;
		for (int count : {1, rows - 1, rows, rows + 1, 2 * rows + 3})
		for (Pattern pattern : {LONG_FILES, SHORT_FILES, MIXED_ENTRIES})
		{
			make_list(count, pattern);
			std::string context = "rows=" + std::to_string(rows) + " lookahead=" + std::to_string(lookahead)
				+ " count=" + std::to_string(count) + " pattern=" + std::to_string(pattern);
			cfg.browse_expand = 1;
			cfg.rbf_hide_datecode = 0;
			traverse(1, 1, context);
			traverse(-1, 1, context);
			if (pattern == MIXED_ENTRIES)
			{
				// Hidden datecodes permit long core names to expand; visible
				// datecodes and directory rows remain one line in the renderer.
				cfg.rbf_hide_datecode = 1;
				traverse(1, 1, context + " hidden datecodes");
				traverse(-1, 1, context + " hidden datecodes");
			}
			if (pattern == LONG_FILES && count == 2 * rows + 3)
			{
				cfg.browse_expand = 0;
				traverse(1, 1, context + " expansion disabled");
				traverse(-1, 1, context + " expansion disabled");
				cfg.browse_expand = 1;
				// The UI suppresses expansion while a navigation key repeats.
				traverse(1, 0, context + " held navigation key");
				traverse(-1, 0, context + " held navigation key");
			}
		}
	}
}

static void destination_expansion_regression()
{
	// With edge scrolling, paging from a short selection to a long selection
	// at the bottom row must make room for its continuation without hiding the
	// first new item. The smallest 8-row fixture has 18 ordinary files: Right
	// selects 0, 8, 16 and 17, and the old pages omit file 9.
	for (int rows : {8, 16})
	for (int lookahead : {0, 1})
	{
		osd_rows = rows;
		cfg.lookahead = lookahead;
		cfg.browse_expand = 1;
		cfg.rbf_hide_datecode = 0;
		make_list(2 * rows + 2, DESTINATION_EXPANDS);
		std::string context = "short-to-long destination rows=" + std::to_string(rows)
			+ " lookahead=" + std::to_string(lookahead);
		traverse(1, 1, context);
		traverse(-1, 1, context);
		// A long final item stays on one row at the bottom. Do not add a
		// prospective penalty when no continuation row can be rendered.
		make_list(2 * rows + 1, DESTINATION_EXPANDS);
		iFirstEntry = 0;
		iSelectedEntry = rows;
		flist_center_selected();
		auto before = render(1, context + " final item");
		scan_navigation(SCANF_NEXT_PAGE, "BIN", 0);
		auto after = render(1, context + " final item");
		check(iSelectedEntry == 2 * rows, context + ": a one-row final destination must preserve the full step");
		verify_transition(before, after, 1, rows, context + " final item");
		// Each page expands only its selected item. Even if both the old and
		// prospective selections are long, deduct one row, not two.
		make_list(3 * rows + 2, LONG_FILES);
		iFirstEntry = 0;
		iSelectedEntry = rows;
		flist_center_selected();
		before = render(1, context + " both selections expand");
		scan_navigation(SCANF_NEXT_PAGE, "BIN", 0);
		after = render(1, context + " both selections expand");
		check(iSelectedEntry - rows == (int)before.size(), context + ": two long selections must cost only one displayed row");
		verify_transition(before, after, 1, rows, context + " both selections expand");
	}
}

static void arbitrary_selection_boundaries()
{
	for (int rows : {8, 16})
	for (int lookahead : {0, 1})
	for (int hide : {0, 1})
	{
		osd_rows = rows;
		cfg.lookahead = lookahead;
		cfg.browse_expand = 1;
		cfg.rbf_hide_datecode = hide;
		make_list(2 * rows + 3, MIXED_ENTRIES);
		for (int selected = 0; selected < (int)DirItem.size(); selected++)
		for (int first : {selected, selected - rows / 2, selected - rows + 1})
		for (int direction : {-1, 1})
		{
			iSelectedEntry = selected;
			iFirstEntry = std::max(0, std::min(first, flist_last_first_entry()));
			flist_center_selected();
			std::string context = "arbitrary selection rows=" + std::to_string(rows)
				+ " lookahead=" + std::to_string(lookahead) + " hidden datecodes=" + std::to_string(hide);
			auto before = render(1, context);
			scan_navigation(direction > 0 ? SCANF_NEXT_PAGE : SCANF_PREV_PAGE, "BIN", 0);
			auto after = render(1, context);
			verify_transition(before, after, direction, selected, context);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 2 && !std::strcmp(argv[1], "--only-destination"))
		destination_expansion_regression();
	else
	{
		original_report();
		if (!(argc == 2 && !std::strcmp(argv[1], "--only-original")))
		{
			destination_expansion_regression();
			focused_boundaries();
			arbitrary_selection_boundaries();
		}
	}
	if (failures) std::fprintf(stderr, "%d pagination checks failed\n", failures);
	else std::printf("PASS: production file pagination, row expansion and boundary coverage (%u transitions)\n", transitions);
	return failures ? 1 : 0;
}

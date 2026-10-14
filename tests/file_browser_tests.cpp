#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "miniz.h"

// The runner extracts these definitions unchanged from file_io.cpp/file_io.h.
// Only the storage root is supplied by the harness; ZIP parsing uses real miniz.
static std::string storage_root;
const char *getRootDir() { return storage_root.c_str(); }
static int archive_opens = 0;
static int counted_archive_open(const char *path, int flags)
{
	archive_opens++;
	return ::open(path, flags);
}
#define open counted_archive_open
#include "file_browser_production.inc"
#undef open

static int failures = 0;
static void check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static direntext_t entry(const char *name, unsigned char type, unsigned int flags = 0, const char *datecode = "")
{
	direntext_t result = {};
	std::snprintf(result.de.d_name, sizeof(result.de.d_name), "%s", name);
	std::snprintf(result.altname, sizeof(result.altname), "%s", name);
	std::snprintf(result.datecode, sizeof(result.datecode), "%s", datecode);
	result.de.d_type = type;
	result.flags = flags;
	return result;
}

static void test_ordering()
{
	const std::vector<direntext_t> samples = {
		entry("..", DT_DIR), entry("..", DT_DIR),
		entry("1 rarely updated files.zip", DT_DIR, DT_EXT_ZIP),
		entry("2 frequently updated files", DT_DIR),
		entry("3 rarely updated files.zip", DT_DIR, DT_EXT_ZIP),
		entry("1 regular file.bin", DT_REG),
		entry("A", DT_DIR), entry("a", DT_DIR),
		entry("Core", DT_REG, 0, "250101"),
		entry("Core", DT_REG, 0, "260101"),
		entry("A.bin", DT_REG), entry("a.rom", DT_REG),
		entry("AA.bin", DT_REG), entry("", DT_REG),
	};
	DirentComp less;
	for (const auto &a : samples)
	{
		check(!less(a, a), "comparator must be irreflexive, including parent directory");
		for (const auto &b : samples)
		{
			check(!(less(a, b) && less(b, a)), "comparator must be asymmetric");
			for (const auto &c : samples)
			{
				if (less(a, b) && less(b, c))
					check(less(a, c), "comparator ordering must be transitive");
				if (!less(a, b) && !less(b, a) && !less(b, c) && !less(c, b))
					check(!less(a, c) && !less(c, a), "comparator equivalence must be transitive");
			}
		}
	}
	check(!less(samples[0], samples[1]) && !less(samples[1], samples[0]),
		"duplicate parent entries must compare equivalent");

	std::vector<direntext_t> mixed = {
		samples[5], samples[4], samples[3], samples[2], samples[0]
	};
	// Avoid invoking std::sort with an invalid comparator when testing old code.
	if (!less(samples[0], samples[0]))
	{
		std::sort(mixed.begin(), mixed.end(), less);
		for (size_t i = 0; i < mixed.size(); i++)
			check(!std::strcmp(mixed[i].altname, samples[i ? i + 1 : 0].altname),
				"ZIPs and folders must retain upstream mixed-directory ordering");
		std::vector<direntext_t> duplicates(1000, samples[0]);
		std::sort(duplicates.begin(), duplicates.end(), less);
		check(duplicates.size() == 1000, "sort must handle repeated equivalent parent entries");
	}
	check(less(samples[8], samples[9]), "core datecode ordering must be preserved");
	check(!less(samples[10], samples[11]) && !less(samples[11], samples[10]),
		"case-insensitive equal stems must retain upstream equivalence");
}

static void add_zip_file(mz_zip_archive *zip, const char *name)
{
	const char payload[] = "test ROM";
	check(mz_zip_writer_add_mem(zip, name, payload, sizeof(payload), MZ_DEFAULT_COMPRESSION),
		"ZIP fixture member must be written");
}

static void test_zip_directory_paths()
{
	const std::string archive_path = storage_root + "/browser.zip";
	mz_zip_archive zip = {};
	check(mz_zip_writer_init_file(&zip, archive_path.c_str(), 0), "ZIP fixture must open");
	// Implicit directories: there are deliberately no explicit folder records.
	add_zip_file(&zip, "outer/target/game.bin");
	add_zip_file(&zip, "real/game.bin");
	add_zip_file(&zip, "real/deep/game.bin");
	add_zip_file(&zip, "realish/game.bin");
	add_zip_file(&zip, "casefolder/game.bin");
	check(mz_zip_writer_add_mem(&zip, "explicit/", "", 0, 0),
		"explicit ZIP directory record must be written");
	check(mz_zip_writer_finalize_archive(&zip), "ZIP fixture must finalize");
	check(mz_zip_writer_end(&zip), "ZIP fixture must close");

	check(isPathDirectory("browser.zip"), "ZIP root must be navigable");
	check(isPathDirectory("browser.zip/outer"), "implicit root folder must be recognized");
	check(isPathDirectory("browser.zip/outer/target"), "implicit nested folder must be recognized");
	check(isPathDirectory("browser.zip/real"), "implicit root folder with multiple members must be recognized");
	check(isPathDirectory("browser.zip/real/deep"), "implicit folder with a full path must be recognized");
	check(isPathDirectory("browser.zip/CASEFOLDER"), "existing case-insensitive ZIP lookup behavior must be preserved");
	check(isPathDirectory("browser.zip/explicit"), "explicit ZIP directory must be recognized");
	check(!isPathDirectory("browser.zip/target"), "nested folder must not also exist at ZIP root");
	check(!isPathDirectory("browser.zip/deep"), "nested suffix must not be mistaken for a root folder");
	check(!isPathDirectory("browser.zip/get"), "a suffix inside a component must not be mistaken for a folder");
	check(!isPathDirectory("browser.zip/reali"), "folder prefixes must respect component boundaries");
	check(!isPathDirectory("browser.zip/missing"), "absent folder must not exist");
	check(!isPathDirectory("browser.zip/real/game.bin"), "regular ZIP member must not be a directory");
	mz_zip_reader_end(&last_zip_archive);
	if (last_zip_cfile) std::fclose(last_zip_cfile);
	last_zip_cfile = nullptr;
	last_zip_fname[0] = 0;
}

static void test_zip_cache_name()
{
	// Reach the old256-byte cache boundary within ordinary Windows path limits.
	const size_t target_length = 256;
	const std::string first = storage_root + "/" + std::string(80, 'a');
	const std::string suffix = "/cache.zip";
	check(first.size() + suffix.size() + 1 < target_length, "temporary root must fit the long cache fixture");
	if (first.size() + suffix.size() + 1 >= target_length) return;
	const std::string folder = first + "/" + std::string(target_length - first.size() - suffix.size() - 1, 'b');
	check(!mkdir(first.c_str(), 0700) && !mkdir(folder.c_str(), 0700), "long ZIP fixture folders must be created");
	const std::string archive_path = folder + suffix;
	mz_zip_archive zip = {};
	check(mz_zip_writer_init_file(&zip, archive_path.c_str(), 0), "long-path ZIP fixture must open");
	add_zip_file(&zip, "folder/Inner.bin");
	check(mz_zip_writer_finalize_archive(&zip) && mz_zip_writer_end(&zip), "long-path ZIP fixture must close");
	std::vector<char> path(archive_path.begin(), archive_path.end());
	path.push_back(0);
	archive_opens = 0;
	check(OpenZipfileCached(path.data(), 0), "real archive at the old cache boundary must open");
	bool terminated = std::memchr(last_zip_fname, 0, sizeof(last_zip_fname)) != nullptr;
	check(terminated, "cached ZIP name must have a terminator after a long container path");
	bool complete = terminated && archive_path.size() < sizeof(last_zip_fname)
		&& !std::strcmp(last_zip_fname, archive_path.c_str());
	check(complete, "cached ZIP name must retain the complete container path");
	// Avoid intentionally executing an invalid C-string comparison on old code.
	if (complete)
	{
		check(OpenZipfileCached(path.data(), 0) && archive_opens == 1, "repeated long archive lookup must reuse its reader");
		std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; });
		check(OpenZipfileCached(path.data(), 0) && archive_opens == 1, "cached archive comparison must retain existing case-insensitive behavior");
	}
	mz_zip_reader_end(&last_zip_archive);
	if (last_zip_cfile) std::fclose(last_zip_cfile);
	last_zip_cfile = nullptr;
	last_zip_fname[0] = 0;
}

static void test_filesystem_directory_paths()
{
	const std::string folder = storage_root + "/folder";
	check(!mkdir(folder.c_str(), 0700), "ordinary directory fixture must be created");
	const std::string file = storage_root + "/ordinary.bin";
	FILE *fixture = std::fopen(file.c_str(), "wb");
	check(fixture != nullptr, "ordinary file fixture must be created");
	if (fixture) std::fclose(fixture);
	check(isPathDirectory(""), "storage root must remain navigable");
	check(isPathDirectory("folder"), "ordinary directory must remain navigable");
	check(!isPathDirectory("ordinary.bin"), "ordinary file must not be a directory");
	check(!isPathDirectory("absent"), "absent ordinary directory must not exist");
	const std::string link = storage_root + "/folder-link";
	if (!symlink("folder", link.c_str()))
		check(isPathDirectory("folder-link"), "directory symlink must remain navigable");
	else
		std::puts("directory symlink fixture skipped: host cannot create symlinks");
}

int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	storage_root = argv[1];
	test_ordering();
	test_zip_directory_paths();
	test_zip_cache_name();
	test_filesystem_directory_paths();
	if (failures) std::fprintf(stderr, "%d checks failed\n", failures);
	else std::puts("file browser comparator and real ZIP path regression checks passed");
	return failures ? 1 : 0;
}

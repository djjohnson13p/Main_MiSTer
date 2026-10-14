#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <new>
#include <string>

#include "../../sxmlc.h"
#include "../../user_io.h"
#include "../../input.h"
#include "../../file_io.h"
#include "../../menu.h"
#include "../../fpga_io.h"
#include "../../lib/md5/md5.h"
#include "../../shmem.h"
#include "../../str_util.h"
#include "../../cheats.h"

#include "buffer.h"
#include "mra_loader.h"

#define kBigTextSize 1024
struct arc_struct {
	char md5[kBigTextSize];
	char zipname[kBigTextSize];
	char partzipname[kBigTextSize];
	char partname[kBigTextSize];
	char romname[kBigTextSize];
	char error_msg[kBigTextSize];
	char cheatname[kBigTextSize];
	int romindex;
	int offset;
	int length;
	int repeat;
	int insiderom;
	int patchaddr;
	int dataop;
	int validrom0;
	int insidesw;
	int insideinterleave;
	int insidecheats;
	int ifrom;
	int ito;
	int imap;
	int file_size;
	uint32_t address;
	uint32_t crc;
	buffer_data *data;
	struct MD5Context context;
};

static char arcade_error_msg[kBigTextSize] = {};
static char arcade_root[kBigTextSize];
static char mame_root[kBigTextSize];
static char arcade_setname[kBigTextSize] = {};

static bool is_vertical = false;
static int rotation_dir = 0; // 0 = None, 1 = CW, 2 = CCW

static sw_struct switches = {};

static int  nvram_idx  = 0;
static int  nvram_size = 0;
static char nvram_name[261] = {};
static char nvram_path[2100] = {};
static bool nvram_path_ready = false;
static bool nvram_ready = false;
static bool nvram_failed = false;

struct nvram_pending_struct {
	size_t depth;
	bool applicable;
	bool index_valid;
	bool size_valid;
	int index;
	int size;
};
static nvram_pending_struct nvram_pending = {};

static int arcade_nvm_name(const char *name, char *out, size_t capacity)
{
	if (!name || !out || !capacity || !*name || strchr(name, '/'))
	{
		errno = EINVAL;
		return 0;
	}
	size_t length = strlen(name);
	if (length >= sizeof(nvram_name) || length >= capacity)
	{
		errno = ENAMETOOLONG;
		return 0;
	}
	char leaf[sizeof(nvram_name)];
	memcpy(leaf, name, length + 1);
	if (length >= 4 && !strcasecmp(leaf + length - 4, ".mra"))
	{
		memcpy(leaf + length - 4, ".nvm", 5);
	}
	memcpy(out, leaf, length + 1);
	return 1;
}

static int arcade_nvm_target(const char *root, const char *name, char *out, size_t capacity)
{
	if (!root || root[0] != '/')
	{
		errno = EINVAL;
		return 0;
	}
	char leaf[sizeof(nvram_name)];
	if (!arcade_nvm_name(name, leaf, sizeof(leaf))) return 0;
	char relative[sizeof(CONFIG_DIR"/nvram/") + sizeof(leaf) - 1] = CONFIG_DIR"/nvram/";
	memcpy(relative + sizeof(CONFIG_DIR"/nvram/") - 1, leaf, strlen(leaf) + 1);
	return FileResolveFullPath(root, relative, out, capacity);
}

static int arcade_nvm_number(const char *text, unsigned long maximum, int *out)
{
	if (!text || !out)
	{
		errno = EINVAL;
		return 0;
	}
	while (isspace((unsigned char)*text)) text++;
	if (!*text || *text == '-')
	{
		errno = EINVAL;
		return 0;
	}
	char *end;
	errno = 0;
	unsigned long value = strtoul(text, &end, 0);
	if (errno == ERANGE || value > maximum || value > INT_MAX)
	{
		errno = ERANGE;
		return 0;
	}
	if (end == text)
	{
		errno = EINVAL;
		return 0;
	}
	while (isspace((unsigned char)*end)) end++;
	if (*end)
	{
		errno = EINVAL;
		return 0;
	}
	*out = (int)value;
	return 1;
}

static void arcade_nvm_retire()
{
	nvram_ready = false;
	nvram_idx = 0;
	nvram_size = 0;
}

static void arcade_nvm_reset()
{
	arcade_nvm_retire();
	nvram_name[0] = 0;
	nvram_path[0] = 0;
	nvram_path_ready = false;
	nvram_failed = false;
	nvram_pending = {};
}

static void arcade_nvm_fail()
{
	arcade_nvm_retire();
	nvram_failed = true;
	nvram_pending = {};
}

static void arcade_nvm_start(bool applicable)
{
	if (nvram_pending.depth)
	{
		if (nvram_pending.applicable || applicable) arcade_nvm_retire();
		nvram_pending.applicable = false;
		if (nvram_pending.depth == SIZE_MAX) arcade_nvm_fail();
		else nvram_pending.depth++;
		return;
	}
	nvram_pending = {};
	nvram_pending.depth = 1;
	nvram_pending.applicable = applicable && !nvram_failed;
	if (nvram_pending.applicable) arcade_nvm_retire();
}

void arcade_nvm_save()
{
	if (nvram_ready)
	{
		char path[sizeof(nvram_path)];
		memcpy(path, nvram_path, strlen(nvram_path) + 1);
		const unsigned char index = (unsigned char)nvram_idx;
		const int size = nvram_size;
		char directory[sizeof(nvram_path)];
		memcpy(directory, path, strlen(path) + 1);
		char *slash = strrchr(directory, '/');
		if (!slash || directory[0] != '/') return;
		*slash = 0;

		uint8_t *buf = new (std::nothrow) uint8_t[size];
		if (buf)
		{
			FileCreatePath(directory);
			printf("Request for nvram (idx=%d, size=%d) data\n", index, size);

			user_io_set_index(index);
			user_io_set_upload(1);
			user_io_file_rx_data(buf, size);
			user_io_set_upload(0);

			FileSave(path, buf, size);
			delete [] buf;
		}
		else errno = ENOMEM;
	}
}

static void arcade_nvm_load()
{
	if (nvram_ready)
	{
		char path[sizeof(nvram_path)];
		memcpy(path, nvram_path, strlen(nvram_path) + 1);
		const unsigned char index = (unsigned char)nvram_idx;
		const int size = nvram_size;
		uint8_t *buf = new (std::nothrow) uint8_t[size];
		if (buf)
		{
			memset(buf, 0, size);

			if (FileLoad(path, buf, size) > 0)
			{
				printf("Sending nvram (idx=%d, size=%d) to core\n", index, size);
				user_io_set_index(index);
				user_io_set_download(1);
				user_io_file_tx_data(buf, size);
				user_io_set_download(0);
			}

			delete [] buf;
		}
		else errno = ENOMEM;
	}
}

static void arcade_nvm_close(bool applicable)
{
	if (!nvram_pending.depth)
	{
		if (applicable) arcade_nvm_retire();
		return;
	}
	if (--nvram_pending.depth) return;
	if (nvram_pending.applicable && !nvram_failed && nvram_path_ready &&
		nvram_pending.index_valid && nvram_pending.size_valid)
	{
		nvram_idx = nvram_pending.index;
		nvram_size = nvram_pending.size;
		nvram_ready = nvram_idx && nvram_size;
		arcade_nvm_load();
	}
	nvram_pending = {};
}

sw_struct *arcade_sw()
{
	return &switches;
}

void arcade_sw_send()
{
	sw_struct *sw = arcade_sw();
	if (sw->dip_num)
	{
		user_io_set_index(254);
		user_io_set_download(1);
		user_io_file_tx_data((uint8_t*)&sw->dip_cur, sizeof(sw->dip_cur));
		user_io_set_download(0);
	}
}

static int arcade_dip_path(const char *name, char *path, size_t capacity)
{
	if (!name || !path || !capacity)
	{
		errno = EINVAL;
		return 0;
	}
	const size_t prefix_length = sizeof("dips/") - 1;
	size_t name_length = strlen(name);
	if (capacity <= prefix_length || name_length >= capacity - prefix_length)
	{
		errno = ENAMETOOLONG;
		return 0;
	}
	try
	{
		std::string resolved("dips/");
		resolved.append(name, name_length);
		memcpy(path, resolved.c_str(), resolved.size() + 1);
	}
	catch (const std::bad_alloc &)
	{
		errno = ENOMEM;
		return 0;
	}
	return 1;
}

void arcade_sw_save()
{
	sw_struct *sw = arcade_sw();
	if (sw->dip_num && sw->dip_saved != sw->dip_cur)
	{
		char path[1024];
		if (!arcade_dip_path(sw->name, path, sizeof(path))) return;
		if (FileSaveConfig(path, &sw->dip_cur, sizeof(sw->dip_cur)))
		{
			sw->dip_saved = sw->dip_cur;
		}
	}
}

void arcade_sw_load()
{
	sw_struct *sw = arcade_sw();
	char path[1024];
	if (!arcade_dip_path(sw->name, path, sizeof(path))) return;
	FileLoadConfig(path, &sw->dip_cur, sizeof(sw->dip_cur));
}

enum arcade_path_error {
	ARCADE_PATH_OK, ARCADE_PATH_INVALID, ARCADE_PATH_TOO_LONG, ARCADE_PATH_UNAVAILABLE
};

static int arcade_path_fail(arcade_path_error *error, int code)
{
	if (error) *error = code == ENAMETOOLONG ? ARCADE_PATH_TOO_LONG :
		code == EINVAL ? ARCADE_PATH_INVALID : ARCADE_PATH_UNAVAILABLE;
	errno = code;
	return 0;
}

static int arcade_path_copy(const char *name, char *out, size_t capacity, arcade_path_error *error = nullptr)
{
	if (error) *error = ARCADE_PATH_OK;
	if (!name || !out || !capacity) return arcade_path_fail(error, EINVAL);
	const size_t length = strlen(name);
	if (length >= capacity) return arcade_path_fail(error, ENAMETOOLONG);
	memmove(out, name, length + 1);
	return 1;
}

static int arcade_path_copy_root(const char *root, char *out, size_t capacity, arcade_path_error *error = nullptr)
{
	if (!root || root[0] != '/') return arcade_path_fail(error, EINVAL);
	return arcade_path_copy(root, out, capacity, error);
}

// Admit the entire literal join before deriving a parent or publishing aliases.
static int arcade_path_join(const char *root, const char *name, char *out, size_t capacity,
	arcade_path_error *error = nullptr, bool parent = false)
{
	if (error) *error = ARCADE_PATH_OK;
	if (!root || !name || !out || !capacity) return arcade_path_fail(error, EINVAL);
	const size_t root_length = strlen(root), name_length = strlen(name);
	if (name_length >= capacity || capacity - name_length < 2 || root_length > capacity - name_length - 2)
		return arcade_path_fail(error, ENAMETOOLONG);
	try
	{
		std::string owned(root, root_length);
		owned += '/';
		owned.append(name, name_length);
		if (parent) owned.resize(owned.find_last_of('/'));
		memcpy(out, owned.c_str(), owned.size() + 1);
	}
	catch (const std::bad_alloc &) { return arcade_path_fail(error, ENOMEM); }
	return 1;
}

static int arcade_path_parent(const char *root, const char *name, char *out, size_t capacity, arcade_path_error *error = nullptr)
{
	return arcade_path_join(root, name, out, capacity, error, true);
}

static int arcade_path_resolve(const char *root, const char *name, char *out, size_t capacity, arcade_path_error *error = nullptr)
{
	if (error) *error = ARCADE_PATH_OK;
	if (!root || root[0] != '/' || !name || !name[0]) return arcade_path_fail(error, EINVAL);
	if (!FileResolveFullPath(root, name, out, capacity)) return arcade_path_fail(error, errno);
	return 1;
}

static void arcade_path_info(arcade_path_error error, bool rbf)
{
	if (error == ARCADE_PATH_TOO_LONG) Info(rbf ? "RBF path too long!" : "XML path too long!");
	else if (error == ARCADE_PATH_INVALID) Info(rbf ? "Invalid RBF path!" : "Invalid XML path!");
	else Info(rbf ? "RBF path unavailable!" : "XML path unavailable!");
}

static int set_arcade_root(const char *path, const char *root = nullptr, bool need_cores = true,
	arcade_path_error *error = nullptr)
{
	if (error) *error = ARCADE_PATH_OK;
	if (!path || !path[0]) return arcade_path_fail(error, EINVAL);
	char pending_arcade[kBigTextSize], fallback[kBigTextSize];
	if (!arcade_path_copy(path, pending_arcade, sizeof(pending_arcade), error) ||
		!arcade_path_copy_root(root ? root : getRootDir(), fallback, sizeof(fallback), error)) return 0;
	char *p = strstr(pending_arcade, "/_");
	if (p) p = strchr(p + 1, '/');
	if (p) *p = 0;
	else memcpy(pending_arcade, fallback, strlen(fallback) + 1);

	char pending_mame[kBigTextSize] = "mame";
	if (need_cores && !arcade_path_join(pending_arcade, "cores", pending_mame, sizeof(pending_mame), error)) return 0;
	memcpy(pending_mame, "mame", 5);
	if (findGamesDir(pending_mame, sizeof(pending_mame)))
	{
		if (strnlen(pending_mame, sizeof(pending_mame)) == sizeof(pending_mame))
			return arcade_path_fail(error, ENAMETOOLONG);
		p = strrchr(pending_mame, '/');
		if (p) *p = 0;
		else pending_mame[0] = 0;
	}
	else memcpy(pending_mame, pending_arcade, strlen(pending_arcade) + 1);

	// Both root candidates are complete before either shared value changes.
	memcpy(arcade_root, pending_arcade, strlen(pending_arcade) + 1);
	memcpy(mame_root, pending_mame, strlen(pending_mame) + 1);
	printf("arcade_root %s\n", arcade_root);
	printf("mame_root %s\n", mame_root);
	return 1;
}

static const char *get_arcade_root(int rbf, arcade_path_error *error = nullptr)
{
	static char path[kBigTextSize];
	if (rbf ? !arcade_path_join(arcade_root, "cores", path, sizeof(path), error) :
		!arcade_path_copy(mame_root, path, sizeof(path), error)) return NULL;
	return path;
}

static int      unitlen = 0;
static int      romlen[8] = {};
static int      romblkl = 0;
static uint8_t* romdata = 0;
static uint8_t  romindex = 0;

static void rom_start(unsigned char index)
{
	romindex = index;
	if (romdata) free(romdata);
	romdata = 0;
	memset(romlen, 0, sizeof(romlen));
	romblkl = 0;
	unitlen = 1;
}

#define BLKL (1024*1024)
static int rom_checksz(int idx, int chunk)
{
	if ((romlen[idx] + chunk) > romblkl)
	{
		if (romlen[idx] + chunk > romblkl + BLKL)
		  romblkl += (chunk + BLKL);
		else
		  romblkl += BLKL;
		romdata = (uint8_t*)realloc(romdata, romblkl);
		if (!romdata)
		{
			printf("realloc failed - romblkl %d \n",romblkl);
			romblkl = 0;
			memset(romlen, 0, sizeof(romlen));
			return 0;
		}
	}
	return 1;
}

static int rom_data(const uint8_t *buf, int chunk, int map, struct MD5Context *md5context)
{
	uint8_t offsets[8]; // assert (unitlen <= 8)
	int bytes_in_iter = 0;

	if (md5context) MD5Update(md5context, buf, chunk);

	int idx = 0;
	if (!map) map = 1;

	int map_reg = map;
	for (int i = 0; i < unitlen; i++)
	{
		if (map_reg & 0xf)
			break;
		map_reg >>= 4;
		idx++;
	}

	if (idx >= unitlen)
		return 0; // illegal map
	if (!rom_checksz(idx, chunk*unitlen))
		return 0;

	map_reg = map;
	bool first = true;
	int gaps = 0;
	for (int i = 0; i < unitlen; i++)
	{
		if (map_reg & 0xf)
		{
			offsets[bytes_in_iter] = idx + (map_reg & 0xf) - 1 + gaps;
			bytes_in_iter++;
			first = false;
		}
		else if(!first)
		{
			gaps++;
		}
		map_reg >>= 4;
	}

	while (chunk)
	{
		for (int i = 0; i < bytes_in_iter; i++)
		{
			*(romdata + romlen[idx] + offsets[i]) = *buf++;
			chunk--;
		}
		romlen[idx] += unitlen;
	}

	return 1;
}

static int rom_file(const char *name, uint32_t crc32, int start, int len, int map, struct MD5Context *md5context)
{
	fileTYPE f = {};
	static uint8_t buf[8192];
	if (!FileOpenZip(&f, name, crc32)) return 0;
	if (start) FileSeek(&f, start, SEEK_SET);
	unsigned long bytes2send = f.size - f.offset;
	if (len > 0 && len < (int)bytes2send) bytes2send = len;

	while (bytes2send)
	{
		uint16_t chunk = (bytes2send > sizeof(buf)) ? sizeof(buf) : bytes2send;

		FileReadAdv(&f, buf, chunk);
		if (!rom_data(buf, chunk, map, md5context))
		{
			FileClose(&f);
			return 0;
		};

		bytes2send -= chunk;
	}

	FileClose(&f);
	return 1;
}

static int rom_patch(const uint8_t *buf, int offset, uint16_t len, int dataop)
{
	if ((offset + len) > romlen[0]) return 0;
	if (!dataop)
	{
		memcpy(romdata + offset, buf, len);
	}
	else
	{
		for (int i = 0; i < len; i++) romdata[offset + i] ^= buf[i];
	}

	return 1;
}

static void rom_finish(int send, uint32_t address, int index)
{
	if (romlen[0] && romdata)
	{
		if (send)
		{
			uint8_t *data = romdata;
			int len = romlen[0];

			// set index byte (0=bios rom, 1-n=OSD entry index)
			user_io_set_index(romindex);

			// prepare transmission of new file
			user_io_set_download(1, address ? len : 0);

			if (address)
			{
				shmem_put(fpga_mem(address), len, data);
			}
			else
			{
				char str[32];
				sprintf(str, "ROM #%d", index);

				ProgressMessage(0, 0, 0, 0);
				while (romlen[0] > 0)
				{
					ProgressMessage("Sending", str, len - romlen[0], len);

					uint32_t chunk = (romlen[0] > 4096) ? 4096 : romlen[0];
					user_io_file_tx_data(data, chunk);

					romlen[0] -= chunk;
					data += chunk;
				}
				ProgressMessage(0, 0, 0, 0);
			}

			// signal end of transmission
			user_io_set_download(0);
			printf("file_finish: 0x%X bytes sent to FPGA\n\n", len);
		}
		else
		{
			printf("file_finish: discard the ROM\n\n");
		}
		free(romdata);
		romdata = 0;
		return;
	}

	if (romdata)
	{
		free(romdata);
		romdata = 0;
	}
	printf("file_finish: no data, discarded\n\n");
}


/*
 * adapted from https://gist.github.com/xsleonard/7341172
 *
 * hexstr_to_char will take hex strings in two types:
 *
 * 00 01 02
 * 000102
 * 00 01 2 03
 * 0001 0203
 *
 * and return an array and a length of binary values
 *
 * caller must free string that is returned
 *
 * */
unsigned char* hexstr_to_char(const char* hexstr, size_t *out_len)
{
	size_t len = strlen(hexstr);
	unsigned char* chrs = (unsigned char*)malloc(len + 1);
	if (!chrs)
		printf("hexstr_to_char: malloc failed len+1=%d\n",len+1);
	int dest = 0;
	// point to the beginning of the array
	const char *ptr = hexstr;
	while (*ptr) {
		// check to see if we have a space
		while (*ptr == '\n' || *ptr == '\r' || *ptr == ' ' || *ptr == ',' || *ptr == '\t' || *ptr == 9 /*horiz tab*/) ptr++;
		if (*ptr == 0) break;

		// pull two characters off
		int val1 = (*ptr % 32 + 9) % 25 * 16;
		ptr++;
		/* check to odd numbers of characters*/
		if (*ptr == 0) {
			int val = (ptr[-1] % 32 + 9) % 25;
			chrs[dest++] = val;
			break;
		}
		int val2 = (*ptr % 32 + 9) % 25;
		ptr++;
		chrs[dest++] = val1 + val2;
	}
	chrs[dest] = 0;
	*out_len = dest; /* dest is 0 based, so we don't need to subtract 1*/
	return chrs;
}

/*
 *  xml_send_rom
 *
 *  This is a callback from the XML parser of the MRA file
 *
 *  It parses the MRA, and sends commands to send rom parts to the fpga
 * */
static int xml_send_rom(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	static char message[32];
	struct arc_struct *arc_info = (struct arc_struct *)sd->user;

	int cheat_size = 0;
	int cheat_max = 0;

	// Special nodes must not reset element state; CDATA is one text fragment.
	if (evt == XML_EVENT_START_NODE)
	{
		if (node->tag_type == TAG_CDATA)
		{
			evt = XML_EVENT_TEXT;
			text = node->tag;
		}
		else if (node->tag_type != TAG_FATHER && node->tag_type != TAG_SELF) return true;
	}
	else if (evt == XML_EVENT_END_NODE && node->tag_type != TAG_END && node->tag_type != TAG_SELF) return true;

	switch (evt)
	{
	case XML_EVENT_START_DOC:
		arcade_nvm_retire();
		nvram_pending = {};
		nvram_failed = false;
		message[0] = 0;
		arc_info->insiderom = 0;
		arc_info->insidesw = 0;
		arc_info->insidecheats = 0;
		break;

	case XML_EVENT_START_NODE:

		/* initialization */
		// initialize things for each tag (node):
		buffer_destroy(arc_info->data);
		arc_info->data = buffer_init(kBigTextSize);
		arc_info->partname[0] = 0;
		arc_info->cheatname[0] = 0;
		arc_info->offset = 0;
		arc_info->length = -1;
		arc_info->repeat = 1;
		arc_info->crc = 0;

		/* on the beginning of a rom tag, we need to reset the state*/
		if (!strcasecmp(node->tag, "rom"))
		{
			arc_info->insiderom = 1;
			arc_info->romname[0] = 0;
			arc_info->romindex = 0;
			arc_info->md5[0] = 0;
			arc_info->ifrom = 0;
			arc_info->ito = 0;
			arc_info->imap = 0;
			arc_info->zipname[0] = 0;
			arc_info->address = 0;
			arc_info->insideinterleave = 0;
			MD5Init(&arc_info->context);
			ProgressMessage(0, 0, 0, 0);
		}

		if (!strcasecmp(node->tag, "switches"))
		{
			arc_info->insidesw = 1;
			switches.dip_cur = 0;
			switches.dip_def = 0;
			switches.dip_num = 0;
			memset(&switches.dip, 0, sizeof(switches.dip));
		}


		if (!strcasecmp(node->tag, "interleave"))
		{
			arc_info->insideinterleave = 1;
			arc_info->ifrom = 8; // default 8.
			arc_info->ito = 0;
			arc_info->imap = 0;
		}

		// for each part tag, we clear the partzipname since it is optional and may not appear in the part tag
		if (!strcasecmp(node->tag, "part"))
		{
			arc_info->partzipname[0] = 0;
			arc_info->imap = 0;
		}

		if (!strcasecmp(node->tag, "patch"))
		{
			arc_info->patchaddr = 0;
			arc_info->dataop = 0;
		}

		//printf("XML_EVENT_START_NODE: tag [%s]\n",node->tag);
		if (!strcasecmp(node->tag, "nvram") &&
			(node->tag_type == TAG_FATHER || node->tag_type == TAG_SELF))
		{
			arcade_nvm_start(!arc_info->insiderom && !arc_info->insidesw && !arc_info->insidecheats);
		}
		// walk the attributes and save them in the data structure as appropriate
		for (int i = 0; i < node->n_attributes; i++)
		{
			//printf("attribute %d name [%s] value [%s]\n",i,node->attributes[i].name,node->attributes[i].value);
			if (!strcasecmp(node->attributes[i].name, "zip") && !strcasecmp(node->tag, "rom"))
			{
				strcpy(arc_info->zipname, node->attributes[i].value);
			}
			if (!strcasecmp(node->attributes[i].name, "name") && !strcasecmp(node->tag, "rom"))
			{
				strcpy(arc_info->romname, node->attributes[i].value);
			}
			if (!strcasecmp(node->attributes[i].name, "md5") && !strcasecmp(node->tag, "rom"))
			{
				strcpy(arc_info->md5, node->attributes[i].value);
			}
			if (!strcasecmp(node->attributes[i].name, "index") && !strcasecmp(node->tag, "rom"))
			{
				arc_info->romindex = atoi(node->attributes[i].value);
				sprintf(message, "Assembling ROM #%d", arc_info->romindex);
			}
			if (!strcasecmp(node->attributes[i].name, "address") && !strcasecmp(node->tag, "rom"))
			{
				arc_info->address = strtoul(node->attributes[i].value, NULL, 0);
			}

			if (!strcasecmp(node->attributes[i].name, "names") && !strcasecmp(node->tag, "buttons"))
			{
				set_ovr_buttons(node->attributes[i].value, 0);
			}

			if (!strcasecmp(node->attributes[i].name, "default") && !strcasecmp(node->tag, "buttons"))
			{
				set_ovr_buttons(node->attributes[i].value, 1);
			}

			/* these only exist if we are inside the rom tag, and in a part tag*/
			if (arc_info->insiderom)
			{
				if (!strcasecmp(node->attributes[i].name, "input") && !strcasecmp(node->tag, "interleave"))
				{
					arc_info->ifrom = strtol(node->attributes[i].value, NULL, 0);
				}
				if (!strcasecmp(node->attributes[i].name, "output") && !strcasecmp(node->tag, "interleave"))
				{
					arc_info->ito = strtol(node->attributes[i].value, NULL, 0);
				}

				if (!strcasecmp(node->attributes[i].name, "zip") && !strcasecmp(node->tag, "part"))
				{
					strcpy(arc_info->partzipname, node->attributes[i].value);
				}
				if (!strcasecmp(node->attributes[i].name, "name") && !strcasecmp(node->tag, "part"))
				{
					strcpy(arc_info->partname, node->attributes[i].value);
				}
				if (!strcasecmp(node->attributes[i].name, "offset") && !strcasecmp(node->tag, "part"))
				{
					arc_info->offset = strtoul(node->attributes[i].value, NULL, 0);
				}
				if (!strcasecmp(node->attributes[i].name, "length") && !strcasecmp(node->tag, "part"))
				{
					arc_info->length = strtoul(node->attributes[i].value, NULL, 0);
				}
				if (!strcasecmp(node->attributes[i].name, "repeat") && !strcasecmp(node->tag, "part"))
				{
					arc_info->repeat = strtoul(node->attributes[i].value, NULL, 0);
				}
				if (!strcasecmp(node->attributes[i].name, "crc") && !strcasecmp(node->tag, "part"))
				{
					arc_info->crc = strtoul(node->attributes[i].value, NULL, 16);
				}
				if (!strcasecmp(node->attributes[i].name, "offset") && !strcasecmp(node->tag, "patch"))
				{
					arc_info->patchaddr = strtoul(node->attributes[i].value, NULL, 0);
				}
				if (!strcasecmp(node->attributes[i].name, "operation") && !strcasecmp(node->tag, "patch"))
				{
					if (!strcasecmp(node->attributes[i].value, "xor")) arc_info->dataop = 1;
				}
				if (!strcasecmp(node->attributes[i].name, "map") && !strcasecmp(node->tag, "part"))
				{
					arc_info->imap = strtoul(node->attributes[i].value, NULL, 16);
					if (!arc_info->insideinterleave && arc_info->imap)
					{
						unitlen = strlen(node->attributes[i].value);
						if (unitlen > 8) unitlen = 8;
						for (int i = 1; i < 8; i++) romlen[i] = romlen[0];
					}
				}
			}
			else if (arc_info->insidesw)
			{
				sw_struct* sw = &switches;
				if (!strcasecmp(node->tag, "switches"))
				{
					if (!strcasecmp(node->attributes[i].name, "default"))
					{
						size_t len = 0;
						unsigned char* binary = hexstr_to_char(node->attributes[i].value, &len);
						for (size_t i = 0; i < len; i++) sw->dip_def |= binary[i] << (i * 8);

						free(binary);
					}
				}

				if (!strcasecmp(node->tag, "dip"))
				{
					if (!strcasecmp(node->attributes[i].name, "name"))
					{
						snprintf(sw->dip[sw->dip_num].name, sizeof(sw->dip[sw->dip_num].name), node->attributes[i].value);
					}

					if (!strcasecmp(node->attributes[i].name, "bits"))
					{
						int b = 0, e = 0;
						int num = sscanf(node->attributes[i].value, "%d,%d", &b, &e);
						if (num <= 0 || b < 0 || b > 63 || e < 0 || e > 63 || (num == 2 && e < b))
						{
							printf("Invalid bits field: ""%s"" (%d, %d, %d)\n", node->attributes[i].value, num, b, e);
						}
						else
						{
							uint64_t mask = 1;
							if (num == 1) e = b;
							sw->dip[sw->dip_num].start = b;
							for (int i = 0; i < (e - b); i++) mask = (mask << 1) | 1;
							sw->dip[sw->dip_num].mask = mask << b;
							sw->dip[sw->dip_num].size = e - b + 1;
						}
					}

					if (!strcasecmp(node->attributes[i].name, "ids"))
					{
						int n = 0;
						char *val = node->attributes[i].value;
						while (*val && n < 32)
						{
							char *p = strchr(val, ',');
							size_t len = p ? p - val : strlen(val);
							size_t sz = len + 1;
							if (sz > sizeof(sw->dip[0].id[0])) sz = sizeof(sw->dip[0].id[0]);
							snprintf(sw->dip[sw->dip_num].id[n], sz, val);
							val += len;
							if (*val == ',') val++;
							n++;
						}
						sw->dip[sw->dip_num].num = n;
					}

					if (!strcasecmp(node->attributes[i].name, "values"))
					{
						int n = 0;
						char *val = node->attributes[i].value;
						while (*val && n < 32)
						{
							char *endp = 0;
							uint64_t v = strtoul(val, &endp, 0);
							if (endp <= val)
							{
								printf("Invalid values field: ""%s""\n", node->attributes[i].value);
								break;
							}

							sw->dip[sw->dip_num].val[n] = v;
							val = endp;
							while (*val && (*val == ' ' || *val == ',')) val++;
							n++;
						}
						sw->dip[sw->dip_num].has_val = 1;
					}
				}
			}
			else if (arc_info->insidecheats)
			{
				if (!strcasecmp(node->tag, "cheat"))
				{
					if (!strcasecmp(node->attributes[i].name, "name"))
					{
						strcpyz(arc_info->cheatname, node->attributes[i].value);
					}
				}
			}
			else
			{
				if (!strcasecmp(node->tag, "nvram") &&
					(node->tag_type == TAG_FATHER || node->tag_type == TAG_SELF) && nvram_pending.applicable &&
					nvram_pending.depth == 1 && !nvram_failed)
				{
					if (!strcasecmp(node->attributes[i].name, "index"))
					{
						nvram_pending.index_valid = arcade_nvm_number(node->attributes[i].value, 255, &nvram_pending.index);
					}
					if (!strcasecmp(node->attributes[i].name, "size"))
					{
						nvram_pending.size_valid = arcade_nvm_number(node->attributes[i].value, INT_MAX, &nvram_pending.size);
					}
				}

				if (!strcasecmp(node->tag, "cheats"))
				{
					if (!strcasecmp(node->attributes[i].name, "size"))
					{
						cheat_size = strtoul(node->attributes[i].value, NULL, 0);
					}
					if (!strcasecmp(node->attributes[i].name, "max"))
					{
						cheat_max = strtoul(node->attributes[i].value, NULL, 0);
					}
				}
			}
		}

		/* at the beginning of each rom - tell the user_io to start a new message */
		if (!strcasecmp(node->tag, "rom"))
		{

			// clear an error message if we have a second rom0
			// this is kind of a problem - you will never see the
			// error from the first rom0?
			//
			if (arc_info->romindex == 0 && strlen(arc_info->zipname))
				arc_info->error_msg[0] = 0;

			rom_start(arc_info->romindex);
		}

		if (!strcasecmp(node->tag, "cheats"))
		{
			arc_info->insidecheats = 1;
			cheats_init_arcade(cheat_size, cheat_max);
		}

		if (arc_info->insiderom && !strcasecmp(node->tag, "interleave"))
		{
			int valid = 1;
			if (arc_info->ifrom != 8) valid = 0;
			if (arc_info->ito < 8 || arc_info->ito>64 || (arc_info->ito & 7)) valid = 0;
			if (arc_info->ito < arc_info->ifrom) valid = 0;

			unitlen = arc_info->ifrom ? arc_info->ito / arc_info->ifrom : 1;
			if (unitlen < 0 || unitlen>8) valid = 0;

			if (!valid)
			{
				printf("Invalid interleave format (from=%d to %d)!\n", arc_info->ifrom, arc_info->ito);

				arc_info->ifrom = 0;
				arc_info->ito = 0;
				arc_info->imap = 0;
				unitlen = 1;
			}
			else
			{
				printf("Using interleave: input %d, output %d\n", arc_info->ifrom, arc_info->ito);
			}

			for (int i = 1; i < 8; i++) romlen[i] = romlen[0];
		}

		ProgressMessage("Loading", message, ftell(sd->file), arc_info->file_size);
		break;

	case XML_EVENT_TEXT:
		/* the text node is the data between tags, ie:  <part>this text</part>
		 *
		 * the buffer_append is part of a buffer library that will realloc automatically
		 */
		{
			int result = buffer_append(arc_info->data, text);
			if (result<0)
				printf("buffer_append failed %d\n",result);
			if (result==-1)
				printf("-1 no data given\n");
			if (result==-2)
				printf("-2 could not allocate\n");
		}
		//printf("XML_EVENT_TEXT: text [%s]\n",text);
		break;

	case XML_EVENT_END_NODE:
		//printf("XML_EVENT_END_NODE: tag [%s]\n",node->tag );

		// At the end of a rom node (when it is closed) we need to calculate hash values and clean up
		if (!strcasecmp(node->tag, "rom"))
		{
			message[0] = 0;

			if (arc_info->insiderom)
			{
				unsigned char checksum[16];
				MD5Final(checksum, &arc_info->context);

				char hex[40];
				char *p = hex;
				for (int i = 0; i < 16; i++)
				{
					sprintf(p, "%02x", (unsigned int)checksum[i]);
					p += 2;
				}

				int checksumsame = !strlen(arc_info->zipname) || !strcasecmp(arc_info->md5, hex);
				int no_checksum = !strcasecmp(arc_info->md5, "none") || !strlen(arc_info->md5);

				if (!no_checksum)
				{
					if (checksumsame == 0)
					{
						printf("\n*** Checksum mismatch\n");
						printf("    md5-orig = %s\n", arc_info->md5);
						printf("    md5-calc = %s\n\n", hex);

						if (!strlen(arc_info->error_msg))
							snprintf(arc_info->error_msg, kBigTextSize, "md5 mismatch for rom %d", arc_info->romindex);
					}
					else
					{
						// this code sets the validerom0 and clears the message
						// if a rom with index 0 has a correct md5. It supresses
						// sending any further rom0 messages
						if (arc_info->romindex == 0)
						{
							arc_info->validrom0 = 1;
							arc_info->error_msg[0] = 0;
						}
					}
				}

				checksumsame |= no_checksum;

				rom_finish(checksumsame, arc_info->address, arc_info->romindex);
			}
			arc_info->insiderom = 0;
		}

		// At the end of a part node, send the rom part if we are inside a rom tag
		//int user_io_file_tx_body_filepart(const char *name,int start, int len)
		if (!strcasecmp(node->tag, "part") && arc_info->insiderom)
		{
			// suppress rom0 if we already sent a valid one
			// this is useful for merged rom sets - if the first one was valid, use it
			// the second might not be
			if (arc_info->romindex == 0 && arc_info->validrom0 == 1) break;
			char fname[kBigTextSize * 2 + 16];
			int start, length, repeat;
			uint32_t crc32;
			repeat = arc_info->repeat;
			start = arc_info->offset;
			crc32 = arc_info->crc;
			length = 0;
			if (arc_info->length > 0) length = arc_info->length;
			//printf("partname[%s]\n",arc_info->partname);
			//printf("zipname [%s]\n",arc_info->zipname);
			//printf("offset[%d]\n",arc_info->offset);
			//printf("length[%d]\n",arc_info->length);
			//printf("repeat[%d]\n",arc_info->repeat);
			//

			if (unitlen == 1 || (arc_info->imap & 0xF)) printf("%6X: ", romlen[0]);
			else printf("        ");

			//user_io_file_tx_body_filepart(getFullPath(fname),0,0);
			if (strlen(arc_info->partname))
			{
				char zipnames_list[kBigTextSize];

				if (strlen(arc_info->partzipname))
				{
					strcpy(zipnames_list, arc_info->partzipname);
				} else {
					strcpy(zipnames_list, arc_info->zipname);
				}

				char *zipname = NULL;
				char *zipptr = zipnames_list;
				const char *root = get_arcade_root(0);
				int result = 0;
				while ((zipname = strsep(&zipptr, "|")) != NULL)
				{
					sprintf(fname, (zipname[0] == '/') ? "%s%s/%s" : "%s/mame/%s/%s", root, zipname, arc_info->partname);

					if(unitlen>1) printf("file: %s, start=%d, len=%d, map(%d)=%X\n", fname, start, length, unitlen, arc_info->imap);
					else printf("file: %s, start=%d, len=%d\n", fname, start, length);

					for (int i = 0; i < repeat; i++)
					{
						result = rom_file(fname, crc32, start, length, arc_info->imap, &arc_info->context);

						// we should check file not found error for the zip
						if (result == 0)
						{
							break;
						}
					}

					if (result)
					{
						break;
					}
				}
				if (result == 0)
				{
					printf("%s does not exist\n", arc_info->partname);
					snprintf(arc_info->error_msg, kBigTextSize, "%s\n%s not found", fname, arc_info->partname);
				}
			}
			else // we have binary data?
			{
				size_t len = 0;
				unsigned char* binary = hexstr_to_char(arc_info->data->content, &len);
				int prev_len = romlen[0];
				printf("data: ");
				if (binary)
				{
					for (int i = 0; i < repeat; i++) rom_data(binary, len, arc_info->imap, &arc_info->context);
					free(binary);
				}
				printf("%d(0x%X) bytes from xml\n", romlen[0] - prev_len, romlen[0] - prev_len);
			}

			if (!arc_info->insideinterleave) unitlen = 1;
		}

		if (!strcasecmp(node->tag, "patch") && arc_info->insiderom)
		{
			size_t len = 0;
			unsigned char* binary = hexstr_to_char(arc_info->data->content, &len);
			if (binary)
			{
				rom_patch(binary, arc_info->patchaddr, len, arc_info->dataop);
				free(binary);
			}
		}

		if (arc_info->insidesw && !strcasecmp(node->tag, "dip"))
		{
			sw_struct* sw = &switches;

			int n = sw->dip_num;
			for (int i = 0; i < sw->dip[n].num; i++)
			{
				sw->dip[n].val[i] = ((sw->dip[n].has_val) ? sw->dip[n].val[i] : i) << sw->dip[n].start;
			}

			if (sw->dip_num < 63) sw->dip_num++;
		}

		if (!strcasecmp(node->tag, "nvram") &&
			(node->tag_type == TAG_END || node->tag_type == TAG_SELF))
		{
			arcade_nvm_close(!arc_info->insiderom && !arc_info->insidesw && !arc_info->insidecheats);
		}

		if (!strcasecmp(node->tag, "switches"))
		{
			arc_info->insidesw = 0;
		}

		if (arc_info->insidecheats && !strcasecmp(node->tag, "cheat"))
		{
			size_t len = 0;
			unsigned char* binary = hexstr_to_char(arc_info->data->content, &len);
			cheats_add_arcade(arc_info->cheatname, (char *)binary, len);
			free(binary);
		}

		if (!strcasecmp(node->tag, "cheats"))
		{
			cheats_finalize_arcade();
			arc_info->insidecheats = 0;
		}

		if (!strcasecmp(node->tag, "interleave"))
		{
			arc_info->ifrom = 0;
			arc_info->ito = 0;
			arc_info->imap = 0;
			unitlen = 1;
			arc_info->insideinterleave = 0;
			printf("Disable interleave\n");
		}
		break;

	case XML_EVENT_ERROR:
		arcade_nvm_fail();
		printf("XML parse: %s: ERROR %d\n", text, n);
		snprintf(arc_info->error_msg, kBigTextSize, "XML parse: %s: ERROR %d\n", text, n);
		break;
	case XML_EVENT_END_DOC:
		if (nvram_pending.depth) arcade_nvm_fail();
		break;
	default:
		break;
	}

	return true;
}

enum arcade_scalar_kind { ARCADE_SCALAR_NONE, ARCADE_SCALAR_RBF, ARCADE_SCALAR_SETNAME, ARCADE_SCALAR_ROTATION };

struct arcade_scalar_owner {
	arcade_scalar_kind kind;
	size_t depth;
	bool invalid;
	bool depth_failed;
};

static arcade_scalar_kind arcade_scalar_type(const char *tag)
{
	if (!strcasecmp(tag, "rbf")) return ARCADE_SCALAR_RBF;
	if (!strcasecmp(tag, "setname")) return ARCADE_SCALAR_SETNAME;
	if (!strcasecmp(tag, "rotation")) return ARCADE_SCALAR_ROTATION;
	return ARCADE_SCALAR_NONE;
}

// Count only an owned scalar's real boundaries, not the document's tag stack.
static bool arcade_scalar_start(arcade_scalar_owner &owner, arcade_scalar_kind kind)
{
	if (owner.depth)
	{
		owner.invalid = true;
		if (owner.depth == SIZE_MAX) owner.depth_failed = true;
		else ++owner.depth;
		return false;
	}
	if (kind == ARCADE_SCALAR_NONE) return false;
	owner = { kind, 1, false, false };
	return true;
}

static bool arcade_scalar_close(arcade_scalar_owner &owner, arcade_scalar_kind kind)
{
	if (!owner.depth || owner.depth_failed) return false;
	if (owner.depth == 1 && kind != owner.kind) owner.invalid = true;
	return --owner.depth == 0;
}

static bool arcade_scalar_append(char *out, size_t &used, const char *text, bool reject_overflow)
{
	if (used >= kBigTextSize) return false;
	size_t length = strlen(text);
	const size_t remaining = kBigTextSize - 1 - used;
	if (length > remaining)
	{
		if (reject_overflow) return false;
		length = remaining;
	}
	if (length) memcpy(out + used, text, length);
	used += length;
	out[used] = 0;
	return true;
}

struct arcade_rotation_scan {
	char prefix[9];
	char suffix[18];
	size_t prefix_used;
	size_t suffix_used;
	bool nonempty;
	bool seen_ccw;
};

static void arcade_rotation_append(arcade_rotation_scan &scan, const char *text)
{
	for (const char *p = text; *p; ++p)
	{
		scan.nonempty = true;
		if (scan.prefix_used < 8)
		{
			scan.prefix[scan.prefix_used++] = *p;
			scan.prefix[scan.prefix_used] = 0;
		}
		if (scan.suffix_used == 17) memmove(scan.suffix, scan.suffix + 1, 16);
		else ++scan.suffix_used;
		scan.suffix[scan.suffix_used - 1] = *p;
		scan.suffix[scan.suffix_used] = 0;
		if (strstr(scan.suffix, "ccw") || strstr(scan.suffix, "CCW") ||
		    strstr(scan.suffix, "counterclockwise") || strstr(scan.suffix, "counter-clockwise"))
			scan.seen_ccw = true;
	}
}

struct arcade_rbf_scan {
	arcade_scalar_owner owner;
	char pending[kBigTextSize];
	size_t used;
	char *output;
	bool invalid;
};

static int xml_scan_rbf(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	arcade_rbf_scan &scan = *(arcade_rbf_scan *)sd->user;
	if (evt == XML_EVENT_START_NODE)
	{
		if (node->tag_type == TAG_CDATA) { evt = XML_EVENT_TEXT; text = node->tag; }
		else if (node->tag_type != TAG_FATHER && node->tag_type != TAG_SELF) return true;
	}
	else if (evt == XML_EVENT_END_NODE && node->tag_type != TAG_END && node->tag_type != TAG_SELF) return true;

	switch (evt)
	{
	case XML_EVENT_START_DOC:
	{
		char *output = scan.output;
		scan = {};
		scan.output = output;
		scan.output[0] = 0;
		break;
	}
	case XML_EVENT_START_NODE:
		if (arcade_scalar_start(scan.owner, arcade_scalar_type(node->tag) == ARCADE_SCALAR_RBF ? ARCADE_SCALAR_RBF : ARCADE_SCALAR_NONE))
		{
			scan.used = 0;
			scan.pending[0] = 0;
		}
		if (scan.owner.invalid) scan.invalid = true;
		break;
	case XML_EVENT_TEXT:
		if (scan.owner.depth && !scan.owner.invalid && !arcade_scalar_append(scan.pending, scan.used, text, true))
			scan.invalid = scan.owner.invalid = true;
		break;
	case XML_EVENT_END_NODE:
		if (!scan.owner.depth)
		{
			if (arcade_scalar_type(node->tag) == ARCADE_SCALAR_RBF) scan.invalid = true;
		}
		else if (arcade_scalar_close(scan.owner, arcade_scalar_type(node->tag)))
		{
			if (scan.owner.invalid) scan.invalid = true;
			else if (scan.used) memcpy(scan.output, scan.pending, scan.used + 1);
			scan.owner = {};
		}
		break;
	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		scan.invalid = true;
		break;
	case XML_EVENT_END_DOC:
		if (scan.owner.depth) scan.invalid = true;
		scan.owner = {};
		break;
	default:
		break;
	}
	return true;
}

struct arcade_preparse_scan {
	arcade_scalar_owner owner;
	char setname[kBigTextSize];
	size_t used;
	arcade_rotation_scan rotation;
	int samedir;
	int captured_same;
	bool foundsetname;
	bool foundrotation;
};

static int xml_read_pre_parse(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	arcade_preparse_scan &scan = *(arcade_preparse_scan *)sd->user;
	if (evt == XML_EVENT_START_NODE)
	{
		if (node->tag_type == TAG_CDATA) { evt = XML_EVENT_TEXT; text = node->tag; }
		else if (node->tag_type != TAG_FATHER && node->tag_type != TAG_SELF) return true;
	}
	else if (evt == XML_EVENT_END_NODE && node->tag_type != TAG_END && node->tag_type != TAG_SELF) return true;

	switch (evt)
	{
	case XML_EVENT_START_DOC:
		scan = {};
		rotation_dir = 0;
		break;
	case XML_EVENT_START_NODE:
	{
		arcade_scalar_kind kind = arcade_scalar_type(node->tag);
		if (kind == ARCADE_SCALAR_SETNAME)
		{
			for (int i = 0; i < node->n_attributes; ++i)
				if (!strcasecmp(node->attributes[i].name, "same_dir") && !strcmp(node->attributes[i].value, "1")) scan.samedir = 1;
		}
		if (kind != ARCADE_SCALAR_SETNAME && kind != ARCADE_SCALAR_ROTATION) kind = ARCADE_SCALAR_NONE;
		if (arcade_scalar_start(scan.owner, kind))
		{
			scan.used = 0;
			scan.setname[0] = 0;
			scan.rotation = {};
			scan.captured_same = scan.samedir;
		}
		break;
	}
	case XML_EVENT_TEXT:
		if (scan.owner.depth && !scan.owner.invalid)
		{
			if (scan.owner.kind == ARCADE_SCALAR_SETNAME)
			{
				if (!arcade_scalar_append(scan.setname, scan.used, text, false)) scan.owner.invalid = true;
			}
			else arcade_rotation_append(scan.rotation, text);
		}
		break;
	case XML_EVENT_END_NODE:
		if (arcade_scalar_close(scan.owner, arcade_scalar_type(node->tag)))
		{
			const bool valid = !scan.owner.invalid;
			if (valid && scan.owner.kind == ARCADE_SCALAR_SETNAME)
			{
				if (scan.used)
				{
					user_io_name_override(scan.setname, scan.captured_same);
					memcpy(arcade_setname, scan.setname, scan.used + 1);
				}
				scan.foundsetname = true;
			}
			else if (valid)
			{
				if (scan.rotation.nonempty)
				{
					is_vertical = scan.rotation.prefix_used == 8 && !strncasecmp(scan.rotation.prefix, "vertical", 8);
					rotation_dir = is_vertical ? (scan.rotation.seen_ccw ? 2 : 1) : 0;
				}
				scan.foundrotation = true;
			}
			scan.owner = {};
			if (valid && scan.foundsetname && scan.foundrotation) return false;
		}
		break;
	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		scan.owner = {};
		break;
	case XML_EVENT_END_DOC:
		scan.owner = {};
		break;
	default:
		break;
	}
	return true;
}

int arcade_send_rom(const char *xml)
{
	if (!xml) { arcade_path_fail(nullptr, EINVAL); arcade_nvm_reset(); arcade_path_info(ARCADE_PATH_INVALID, false); return 0; }
	std::string owned_xml;
	try { owned_xml.assign(xml); }
	catch (const std::bad_alloc &) { arcade_path_fail(nullptr, ENOMEM); arcade_nvm_reset(); arcade_path_info(ARCADE_PATH_UNAVAILABLE, false); return 0; }
	xml = owned_xml.c_str();
	const char *p = strrchr(xml, '/');
	p = p ? p + 1 : xml;
	snprintf(switches.name, sizeof(switches.name), "%s", p);
	const size_t name_length = strlen(switches.name);
	if (name_length >= 4 && !strcasecmp(switches.name + name_length - 4, ".mra"))
		strcpy(switches.name + name_length - 4, ".dip");

	arcade_nvm_reset();
	const bool nvm_name_ready = arcade_nvm_name(p, nvram_name, sizeof(nvram_name));
	char captured_root[kBigTextSize];
	arcade_path_error path_error = ARCADE_PATH_OK;
	const bool root_ready = arcade_path_copy_root(getRootDir(), captured_root, sizeof(captured_root), &path_error);
	if (nvm_name_ready && root_ready)
		nvram_path_ready = arcade_nvm_target(captured_root, nvram_name, nvram_path, sizeof(nvram_path));

	SAX_Callbacks sax;
	SAX_Callbacks_init(&sax);

	sax.all_event = xml_send_rom;

	if (!root_ready || !set_arcade_root(xml, captured_root, false, &path_error))
	{
		arcade_nvm_reset();
		arcade_path_info(path_error, false);
		return 0;
	}

	// create the structure we use for the XML parser
	struct arc_struct arc_info;
	arc_info.data = buffer_init(kBigTextSize);
	arc_info.error_msg[0] = 0;
	arc_info.validrom0 = 0;
	struct stat64 *st = getPathStat(xml);
	if (st) arc_info.file_size = (int)st->st_size;
	ProgressMessage(0, 0, 0, 0);

	// parse
	if (!XMLDoc_parse_file_SAX(xml, &sax, &arc_info)) arcade_nvm_fail();
	if (arc_info.validrom0 == 0 && strlen(arc_info.error_msg))
	{
		strcpy(arcade_error_msg, arc_info.error_msg);
		printf("arcade_send_rom: pretty error: [%s]\n", arcade_error_msg);
	}
	buffer_destroy(arc_info.data);

	// Write game ID using setname as serial
	if (arcade_setname[0])
	{
		char mra_path[kBigTextSize];
		snprintf(mra_path, sizeof(mra_path), "%s.mra", arcade_setname);
		user_io_write_gameid(mra_path, 0, arcade_setname);
	}

	switches.dip_cur = switches.dip_def;
	arcade_sw_load();
	switches.dip_saved = switches.dip_cur;
	arcade_sw_send();
	return 0;
}

void arcade_pre_parse(const char *xml)
{
	SAX_Callbacks sax;
	SAX_Callbacks_init(&sax);

	sax.all_event = xml_read_pre_parse;
	arcade_preparse_scan scan = {};
	XMLDoc_parse_file_SAX(xml, &sax, &scan);
}

bool arcade_is_vertical()
{
	return is_vertical;
}

int arcade_get_direction()
{
	return rotation_dir;
}

void arcade_check_error()
{
	if (arcade_error_msg[0] != 0) {
		printf("ERROR: [%s]\n", arcade_error_msg);
		Info(arcade_error_msg, 1000 * 5);
		arcade_error_msg[0] = 0;
		sleep(5+3);
	}
}

static const char *get_rbf_path(const char *rbfname, const char *root = nullptr, arcade_path_error *error = nullptr)
{
	static char path[kBigTextSize];
	char name[kBigTextSize], captured_root[kBigTextSize];
	if (!arcade_path_copy(rbfname, name, sizeof(name), error) ||
		!arcade_path_copy_root(root ? root : getRootDir(), captured_root, sizeof(captured_root), error) ||
		!arcade_path_parent(captured_root, name, path, sizeof(path), error)) return NULL;
	return path;
}

static bool arcade_rbf_match(const char *fragment, const char *entry, bool arcade_prefix)
{
	if (arcade_prefix)
	{
		if (strncasecmp(entry, "Arcade-", 7)) return false;
		entry += 7;
	}
	const size_t length = strlen(fragment);
	return !strncasecmp(fragment, entry, length) && (entry[length] == '.' || entry[length] == '_');
}

static const char *get_rbf(const char *xml, int arcade, const char *root = nullptr, arcade_path_error *error = nullptr)
{
	static char rbfname[kBigTextSize];
	if (error) *error = ARCADE_PATH_OK;
	char input[kBigTextSize], captured_root[kBigTextSize];
	if (!arcade_path_copy(xml, input, sizeof(input), error) ||
		(root && !arcade_path_copy_root(root, captured_root, sizeof(captured_root), error))) return NULL;

	rbfname[0] = 0;
	SAX_Callbacks sax;
	SAX_Callbacks_init(&sax);

	sax.all_event = xml_scan_rbf;
	arcade_rbf_scan scan = {};
	scan.output = rbfname;
	if (!XMLDoc_parse_file_SAX(input, &sax, &scan) || scan.invalid || scan.owner.depth || !rbfname[0]) return NULL;

	/* once we have the rbfname fragment from the MRA xml file
	 * search the arcade folder for the match */
	struct dirent *entry;
	DIR *dir = 0;

	char dirname[kBigTextSize];
	const char *filename;
	const char *prepared;
	if (arcade)
	{
		prepared = get_arcade_root(1, error);
		filename = rbfname;
	}
	else
	{
		prepared = get_rbf_path(rbfname, root ? captured_root : nullptr, error);
		filename = strrchr(rbfname, '/');
		if (filename) filename++;
		else filename = rbfname;
	}
	if (!prepared || !arcade_path_copy(prepared, dirname, sizeof(dirname), error)) return NULL;

	if (!(dir = opendir(dirname)))
	{
		printf("%s directory not found\n", dirname);
		return NULL;
	}

	int len;
	static char lastfound[256] = {};
	while ((entry = readdir(dir)) != NULL)
	{
		len = strlen(entry->d_name);
		if (entry->d_type != DT_DIR && len > 4 && !strcasecmp(entry->d_name+len-4,".rbf"))
		{
			//printf("entry name: %s\n",entry->d_name);

			if (arcade)
			{
				if (arcade_rbf_match(filename, entry->d_name, true))
				{
					if (!lastfound[0] || strcmp(lastfound, entry->d_name) < 0)
					{
						strcpy(lastfound, entry->d_name);
					}
				}
			}

			if (arcade_rbf_match(filename, entry->d_name, false))
			{
				if (!lastfound[0] || strcmp(lastfound, entry->d_name) < 0)
				{
					strcpy(lastfound, entry->d_name);
				}
			}
		}
	}

	char selected[kBigTextSize];
	const int ready = !lastfound[0] || arcade_path_join(dirname, lastfound, selected, sizeof(selected), error);
	const int path_errno = errno;
	closedir(dir);
	if (!ready) { errno = path_errno; return NULL; }
	if (lastfound[0]) memcpy(rbfname, selected, strlen(selected) + 1);

	return lastfound[0] ? rbfname : NULL;
}

int xml_load(const char *xml)
{
	MenuHide();
	static char path[kBigTextSize];
	char input[kBigTextSize], captured_root[kBigTextSize], selected[kBigTextSize];
	arcade_path_error error = ARCADE_PATH_OK;
	if (!xml || !xml[0]) arcade_path_fail(&error, EINVAL);
	else if (arcade_path_copy(xml, input, sizeof(input), &error) &&
		arcade_path_copy_root(getRootDir(), captured_root, sizeof(captured_root), &error))
		arcade_path_resolve(captured_root, input, selected, sizeof(selected), &error);
	if (error != ARCADE_PATH_OK) { arcade_path_info(error, false); return 0; }

	const size_t len = strlen(input);
	const int is_arcade = (len > 4) && !strcasecmp(input + len - 4, ".mra");
	if (is_arcade && !set_arcade_root(selected, captured_root, true, &error))
	{
		arcade_path_info(error, false);
		return 0;
	}
	memcpy(path, selected, strlen(selected) + 1);
	printf("xml_load [%s]\n", path);
	const char *rbf = get_rbf(path, is_arcade, captured_root, &error);

	if (rbf)
	{
		printf("XML: %s, RBF: %s\n", path, rbf);
		fpga_load_rbf(rbf, NULL, path);
	}
	else
	{
		if (error != ARCADE_PATH_OK) arcade_path_info(error, true);
		else Info("No rbf found!");
	}

	return 0;
}

static mgl_struct mgl = {};

static int scan_mgl(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	(void)sd;

	static int inside_mgl = 0;
	// Special-node payloads must not enter MGL element handlers.
	if (evt == XML_EVENT_START_NODE && node->tag_type != TAG_FATHER && node->tag_type != TAG_SELF)
		return true;
	if (evt == XML_EVENT_END_NODE && node->tag_type != TAG_END && node->tag_type != TAG_SELF)
		return true;

	switch (evt)
	{
	case XML_EVENT_START_DOC:
		inside_mgl = 0;
		break;

	case XML_EVENT_START_NODE:
		if (!strcasecmp(node->tag, "mistergamedescription")) inside_mgl = 1;
		else if (inside_mgl && mgl.count < (int)(sizeof(mgl.item) / sizeof(mgl.item[0])))
		{
			if (!strcasecmp(node->tag, "file"))
			{
				mgl.item[mgl.count] = {};
				mgl.item[mgl.count].action = MGL_ACTION_LOAD;
				bool invalid_path = false;

				for (int i = 0; i < node->n_attributes; i++)
				{
					if (!strcasecmp(node->attributes[i].name, "delay"))
					{
						mgl.item[mgl.count].delay = strtoul(node->attributes[i].value, NULL, 0);
						mgl.item[mgl.count].valid |= 0x1;
					}
					else if (!strcasecmp(node->attributes[i].name, "type"))
					{
						if (!strcasecmp(node->attributes[i].value, "s"))
						{
							mgl.item[mgl.count].type = 'S';
							mgl.item[mgl.count].valid |= 0x2;
						}
						else if (!strcasecmp(node->attributes[i].value, "f"))
						{
							mgl.item[mgl.count].type = 'F';
							mgl.item[mgl.count].valid |= 0x2;
						}
					}
					else if (!strcasecmp(node->attributes[i].name, "index"))
					{
						mgl.item[mgl.count].index = strtoul(node->attributes[i].value, NULL, 0);
						mgl.item[mgl.count].valid |= 0x4;
					}
					else if (!strcasecmp(node->attributes[i].name, "path"))
					{
						if (strlen(node->attributes[i].value) < sizeof(mgl.item[mgl.count].path))
						{
							snprintf(mgl.item[mgl.count].path, sizeof(mgl.item[mgl.count].path), "%s", node->attributes[i].value);
							mgl.item[mgl.count].valid |= 0x8;
						}
						else
						{
							printf("MGL file path does not fit; skipping action.\n");
							invalid_path = true;
						}
					}
				}

				printf("  action=load\n  delay=%d\n  type=%c\n  index=%d\n  path=%s\n  valid=%X\n\n", mgl.item[mgl.count].delay, mgl.item[mgl.count].type, mgl.item[mgl.count].index, mgl.item[mgl.count].path, mgl.item[mgl.count].valid);

				if (mgl.item[mgl.count].valid == 0xF && !invalid_path)
				{
					mgl.item[mgl.count].valid = 1;
					mgl.count++;
				}
				else
				{
					mgl.item[mgl.count].valid = 0;
				}
			}
			else if (!strcasecmp(node->tag, "reset"))
			{
				mgl.item[mgl.count] = {};
				mgl.item[mgl.count].action = MGL_ACTION_RESET;

				for (int i = 0; i < node->n_attributes; i++)
				{
					if (!strcasecmp(node->attributes[i].name, "delay"))
					{
						mgl.item[mgl.count].delay = strtoul(node->attributes[i].value, NULL, 0);
						mgl.item[mgl.count].valid = 1;
					}
					else if (!strcasecmp(node->attributes[i].name, "hold"))
					{
						mgl.item[mgl.count].hold = strtoul(node->attributes[i].value, NULL, 0);
					}
				}

				printf("  action=reset\n  delay=%d\n  hold=%d\n\n", mgl.item[mgl.count].delay, mgl.item[mgl.count].hold);
				if (mgl.item[mgl.count].valid) mgl.count++;
			}
		}
		break;

	case XML_EVENT_TEXT:
		break;

	case XML_EVENT_END_NODE:
		if (!strcasecmp(node->tag, "mistergamedescription")) inside_mgl = 0;
		break;

	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		break;
	default:
		break;
	}

	return true;
}

mgl_struct* mgl_parse(const char *xml)
{
	memset(&mgl, 0, sizeof(mgl));

	printf("MGL %s\n", xml);

	SAX_Callbacks sax;
	SAX_Callbacks_init(&sax);
	sax.all_event = scan_mgl;
	XMLDoc_parse_file_SAX(xml, &sax, 0);

	return &mgl;
}

mgl_struct* mgl_get()
{
	return &mgl;
}

static int checks = 0, failures = 0;
static void check(bool ok, const char *label)
{
    checks++;
    if (!ok) { failures++; std::fprintf(stderr, "FAIL: %s\n", label); }
}
static std::string path(const char *name) { return storage_root + "/" + name; }
static std::string nested_path(size_t size, char c = 'a')
{
    // Keep individual components short: historical FileOpenEx separately
    // copies a basename into261bytes without checking it. These cases test
    // the MGL total-path boundary without executing that unrelated defect.
    if (!size) return "";
    std::string result;
    for (size_t i = 0; i < (size - 1) / 2; i++) { result += c; result += '/'; }
    result += std::string(size - result.size(), c);
    return result;
}
static std::string escaped(const std::string &text)
{
    std::string result;
    for (char c : text) {
        if (c == '&') result += "&amp;";
        else if (c == '"') result += "&quot;";
        else if (c == '<') result += "&lt;";
        else result += c;
    }
    return result;
}
static std::string description(const std::string &body)
{ return "<mistergamedescription><rbf>TestCore</rbf>" + body + "</mistergamedescription>"; }
static std::string file(const std::string &name, char type = 'F', int index = 1)
{
    return "<file delay=\"0\" type=\"" + std::string(1, type) + "\" index=\"" + std::to_string(index)
        + "\" path=\"" + escaped(name) + "\"/>";
}
static mgl_struct *parse(const std::string &body)
{
    const std::string xml = description(body), filename = path("attributes.mgl");
    FILE *output = std::fopen(filename.c_str(), "wb");
    if (!output || std::fwrite(xml.data(), 1, xml.size(), output) != xml.size()) std::abort();
    std::fclose(output);
    auto *value = mgl_parse(filename.c_str());
    check(value == mgl_get() && !xml_errors, "actual SAX parser and getter must process the complete valid document");
    return value;
}
static void attempt(mgl_struct *parsed)
{
    initialize_script(); // actual user_io_init decision, with peripheral identity/clock fixtures
    script = *parsed;
    if (script.done) return;
    script.state = 2;
    cfg = cfg_t{};
    cfg.log_file_entry = 1;
    menustate = script.item[0].type == 'S' ? MENU_GENERIC_IMAGE_SELECTED : MENU_GENERIC_FILE_SELECTED;
    fs_Options = SCANO_DIR;
    ioctl_index = 1;
    store_name = false; opensave = 0; load_addr = 0; addon[0] = 0;
    special_core = 0; forced_gba = false; use_cheats = 0;
    std::strcpy(fs_pFileExt, script.item[0].type == 'S' ? "IMG" : "GBA");
    selected_case();
}
static void rejected(const std::string &name, char type)
{
    auto *value = parse(file(name, type, 9));
    check(decoded_path_bytes == name.size(), "actual XML reader must deliver the complete decoded path");
    check(value->count == 0, "a filename exceeding storage must not become an executable MGL action");
    timer_delay = 999;
    initialize_script();
    check(value->done == 1 && timer_delay == 999, "actual post-core decision must disable a script with no complete actions");
    const auto before = calls.size();
    const auto bytes = transfer_bytes.size();
    const std::string selected = Selected_F[1];
    const std::string disk = Selected_S[1];
    attempt(value);
    check(calls.size() == before && transfer_bytes.size() == bytes && std::string(Selected_F[1]) == selected
          && std::string(Selected_S[1]) == disk,
          "rejected-only script must cause no selected-case load, log, metadata or disk side effects");
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    storage_root = argv[1]; home = storage_root;
    auto *value = parse("<file delay=\"0x1\" type=\"f\" index=\"01\" path=\"Game &amp; One.gba\"/>"
                        "<file delay=\"2\" type=\"s\" index=\"0\" path=\"disk.img\"/>"
                        "<reset delay=\"3\" hold=\"4\"/>");
    check(value->count == 3, "ordinary F/S/reset actions must retain order and count");
    check(std::string(value->item[0].path) == "Game & One.gba" && value->item[0].type == 'F'
          && value->item[0].delay == 1 && value->item[0].index == 1,
          "decoded entities, case and base-zero numeric interpretation remain intact");
    check(value->item[1].type == 'S' && value->item[2].action == MGL_ACTION_RESET && value->item[2].hold == 4,
          "ordinary disk and explicit reset hold must remain intact");
    initialize_script();
    check(!value->done && timer_delay == 1000, "accepted script must retain actual initial delay scheduling");
    for (size_t size : {size_t(0), size_t(8), size_t(1022), size_t(1023)}) {
        const std::string original = nested_path(size);
        for (char type : {'F', 'S'}) {
            value = parse(file(original, type));
            check(decoded_path_bytes == size && value->count == 1 && value->item[0].valid == 1
                  && std::string(value->item[0].path) == original,
                  "every fitting path including the existing empty-path policy must remain complete");
        }
    }
    for (const std::string &name : {nested_path(1024), nested_path(2048, 'b'),
                                   nested_path(1022) + "\xC3\xA9"}) {
        rejected(name, 'F'); rejected(name, 'S');
    }
    value = parse(file(nested_path(1021) + "\xC3\xA9"));
    check(value->count == 1 && std::strlen(value->item[0].path) == 1023
          && std::string(value->item[0].path) == nested_path(1021) + "\xC3\xA9",
          "a fitting UTF8 codepoint must be retained exactly at the byte boundary");
    value = parse(file(nested_path(1023, '&')));
    check(value->count == 1 && decoded_path_bytes == 1023 && std::string(value->item[0].path) == nested_path(1023, '&'),
          "encoded XML length must not reject a fitting decoded path");
    rejected(nested_path(1024, '&'), 'F');
    value = parse("<file delay=\"0\" type=\"F\" index=\"1\" path=\"ordinary.gba\" note=\"" + std::string(4096, 'x') + "\"/>");
    check(value->count == 1 && std::string(value->item[0].path) == "ordinary.gba",
          "an unrelated long attribute must not reject a normal path");

    const std::string too_long = nested_path(1024);
    for (const std::string &invalid : {
            file(too_long, 'F', 9), file(too_long, 'S', 9),
            std::string("<file delay=\"7\" type=\"F\" index=\"9\"/>"),
            std::string("<reset hold=\"9\"/>")}) {
        value = parse(invalid + "<reset delay=\"0\"/>");
        check(value->count == 1 && value->item[0].action == MGL_ACTION_RESET && value->item[0].hold == 0
              && value->item[0].path[0] == 0 && value->item[0].type == 0,
              "a rejected action must not supply the following reset's omitted hold or filename");
        value = parse(invalid + file("ordinary.gba"));
        check(value->count == 1 && value->item[0].action == MGL_ACTION_LOAD
              && std::string(value->item[0].path) == "ordinary.gba" && value->item[0].index == 1,
              "a rejected action must leave room for the next complete file");
    }
    for (const std::string &paths : {
            "path=\"ordinary.gba\" path=\"" + too_long + "\"",
            "path=\"" + too_long + "\" path=\"ordinary.gba\""}) {
        value = parse("<file delay=\"0\" type=\"F\" index=\"1\" " + paths + "/>" + file("ordinary.gba"));
        check(value->count == 1 && std::string(value->item[0].path) == "ordinary.gba",
              "a second path attribute cannot reinstate an action containing an unrepresentable path");
    }
    for (const std::string &paths : {
            std::string("path=\"First.gba\" path=\"ordinary.gba\""),
            std::string("path=\"ordinary.gba\" path=\"Last.gba\"")}) {
        value = parse("<file delay=\"0\" type=\"F\" index=\"1\" " + paths + "/>");
        check(value->count == 1 && std::string(value->item[0].path)
              == (paths.find("Last.gba") == std::string::npos ? "ordinary.gba" : "Last.gba"),
              "existing last-value policy must remain for duplicate paths that both fit");
    }
    value = parse("<file delay=\"2147483648\" type=\"F\" index=\"2147483648\" path=\"ordinary.gba\"/>");
    check(value->count == 1 && value->item[0].delay < 0 && value->item[0].index < 0,
          "numeric narrowing policy remains outside this filename and recovery change");
    std::string many = file(too_long);
    for (int i = 0; i < 7; i++) many += file("File" + std::to_string(i) + ".gba");
    value = parse(many);
    check(value->count == 6 && std::string(value->item[0].path) == "File0.gba"
          && std::string(value->item[5].path) == "File5.gba",
          "rejected actions must not consume the six-action capacity");
    value = parse("");
    initialize_script();
    check(value->count == 0 && value->done, "empty script must remain disabled by actual post-core initialization");
    for (int mode = 0; mode < 3; mode++) {
        value = parse(file("ordinary.gba"));
        fixture_menu_core = mode == 0; fixture_st_core = mode == 1; fixture_archie_core = mode == 2;
        initialize_script();
        check(value->done, "Menu, ST and Archimedes exclusions must remain intact");
        fixture_menu_core = fixture_st_core = fixture_archie_core = false;
    }
    char buffer[1200] = {};
    check(FileLoad(path("prefix.txt").c_str(), buffer, sizeof(buffer)) == 1023,
          "complete ZIP path fixture must be exactly 1023 bytes");
    const std::string prefix = buffer, wanted = prefix + ".extra.gba";
    fileTYPE actual, absent;
    check(FileOpen(&actual, prefix.c_str(), 1) && actual.size == 31, "complete maximum-length member must exist");
    check(!FileOpen(&absent, wanted.c_str(), 1), "distinct longer requested member must be absent");
    FileClose(&actual);
    value = parse(file(prefix));
    check(value->count == 1 && std::string(value->item[0].path) == prefix,
          "maximum fitting existing member must retain its complete identity");
    const size_t before = transfer_bytes.size();
    attempt(value);
    check(transfer_return == 1 && transfer_bytes.size() == before + 31 && load_path == prefix
          && std::string(Selected_F[1]) == prefix && script.state == 3,
          "actual selected case and file/ZIP transfer must load the complete fitting member");
    rejected(wanted, 'F');
    value = parse(file(wanted) + file("ordinary.gba"));
    const size_t before_next = transfer_bytes.size();
    attempt(value);
    check(value->count == 1 && transfer_return == 1 && load_path == path("ordinary.gba")
          && transfer_bytes.size() == before_next + std::strlen("ORDINARY_SYNTHETIC_ROM"),
          "actual selected case must load the following complete file rather than a shortened prefix member");
    value = parse(file("Disk.img", 'S'));
    const auto before_disk = transfer_bytes.size();
    attempt(value);
    check(mount_result == 1 && last_mount_writable && std::string(Selected_S[1]) == path("Disk.img") && script.state == 3
          && transfer_bytes.size() == before_disk,
          "accepted S path must retain real writable-open image dispatch without ROM transmission");
    char disk_data[64] = {};
    check(FileLoad(path("Disk.img").c_str(), disk_data, sizeof(disk_data))
          == int(std::strlen("SYNTHETIC_WRITABLE_DISK")) && std::string(disk_data) == "SYNTHETIC_WRITABLE_DISK",
          "writable image fixture must retain its original bytes");
    mz_zip_reader_end(&last_zip_archive);
    if (last_zip_cfile) { std::fclose(last_zip_cfile); last_zip_cfile = nullptr; }
    std::printf("JSON {\"checks\":%d,\"failures\":%d}\n", checks, failures);
    return failures ? 1 : 0;
}

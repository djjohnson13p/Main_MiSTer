static std::string fault_mode, fault_basename;
static bool fault_active = false, fault_applied = false, fault_after_first = false;
static bool target_active = false;
static int shrink_size = 0, expected_data_calls = 0;
static int close_calls = 0, file_handles_closed = 0, unmap_calls = 0;
static int fpga_enables = 0, fpga_disables = 0;
static bool fpga_enabled = false, spi_balanced = true, download_active = false;
static std::vector<unsigned char> mapped_bytes;
static std::vector<int> read_counts, requested_counts, download_states;
static std::vector<uint32_t> before_crcs, after_crcs;
static std::vector<std::string> ordered_events;
static uint32_t fio_size = 0;
void EnableFpga() { spi_balanced &= !fpga_enabled; fpga_enabled = true; fpga_enables++; }
void DisableFpga() { spi_balanced &= fpga_enabled; fpga_enabled = false; fpga_disables++; }
void spi8(int) { spi_balanced &= fpga_enabled; }
void spi_w(int) { spi_balanced &= fpga_enabled; }
void spi_write(const uint8_t *, uint32_t, int) { spi_balanced &= fpga_enabled; }
void production_user_io_set_download(unsigned char, int);
void production_user_io_file_tx_data(const uint8_t *, uint32_t);

static void apply_fault(const std::string &name)
{
    if (!fault_active || fault_applied) return;
    ordered_events.push_back("truncate");
    if (truncate(name.c_str(), shrink_size)) std::abort();
    fault_applied = true;
}
static bool matches(fileTYPE *file)
{
    return fault_active && std::string(file->name) == fault_basename;
}
static bool truncate_aux_after_header=false; static std::string path_for_aux;
int trace_FileOpen(fileTYPE *file, const char *name, char mute = 0)
{
    const int result = FileOpen(file, name, mute);
    // Remove libc read-ahead only for the deliberate after-delivery race
    // fixture, so the next real fread observes the real file truncation.
    if (result && truncate_aux_after_header && std::string(file->name)=="bsx_bios.rom" && file->filp) std::setvbuf(file->filp,nullptr,_IONBF,0);
    if (result && matches(file) && fault_after_first && file->filp) std::setvbuf(file->filp, nullptr, _IONBF, 0);
    if (result && matches(file) && fault_mode == "aux-zero") apply_fault(name);
    return result;
}
int trace_FileClose(fileTYPE *file)
{
    int fd = -1;
    if (file->filp) fd = fileno(file->filp);
    else if (file->zip && mz_zip_get_cfile(&file->zip->archive)) fd = fileno(mz_zip_get_cfile(&file->zip->archive));
    close_calls++;
    const int result = FileClose(file);
    if (fd >= 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF) file_handles_closed++;
    ordered_events.push_back("close");
    return result;
}

static int helper_core=0, optional_calls=0, status_calls=0;
static bool map_failure=false, fail_alloc=false, fail_temp=false, fail_encode_read=false, fail_temp_write=false, fail_temp_seek=false;
static int seek_lba_calls=0, fail_lba_at=0;
static std::vector<uint64_t> msu_commands;
static void *trace_malloc(size_t size) {
    void *memory=fail_alloc ? nullptr : std::malloc(size);
    // Initialize only fixture allocations, so old positive-short parser reads
    // cannot inspect indeterminate bytes while still accepting the wrong span.
    if(memory) std::memset(memory,0xA5,size);
    return memory;
}
static FILE *trace_tmpfile() { return fail_temp ? nullptr : std::tmpfile(); }
static size_t trace_fread(void *data, size_t size, size_t count, FILE *file) {
    size_t result=(fail_encode_read && size*count==1) ? 0 : std::fread(data,size,count,file);
    // Deterministic boundary for the old parser's uninitialized-byte bug.
    // The production count stays zero; the new parser must stop regardless.
    if(!result && size*count==1) std::memset(data,0,1);
    return result;
}
static size_t trace_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    return fail_temp_write ? 0 : std::fwrite(data,size,count,file);
}
static int trace_fseek(FILE *file, long offset, int origin) {
    return fail_temp_seek ? -1 : std::fseek(file,offset,origin);
}
static int trace_FileSeekLBA(fileTYPE *file,uint32_t offset) {
    seek_lba_calls++;
    return seek_lba_calls==fail_lba_at ? 0 : FileSeek(file,(uint64_t)offset*512,SEEK_SET);
}
static int main_seek_count=0, main_seek_fail_at=0;
static bool truncate_spc_after_dsp=false;
static int trace_FileSeek(fileTYPE *file,__off64_t offset,int origin)
{
    main_seek_count++;
    return main_seek_count==main_seek_fail_at ? 0 : FileSeek(file,offset,origin);
}
static int trace_FileReadSec(fileTYPE *file,void *data)
{
    int result=FileReadSec(file,data);
    if(truncate_spc_after_dsp && file->offset==66048) {
        truncate(path_for_aux.c_str(),4000); truncate_spc_after_dsp=false;
    }
    return result;
}
// LATE_TRANSFER_SCENARIOS
static unsigned int proof_checks = 0, proof_failures = 0;
static void expected(bool ok, const char *message)
{
    proof_checks++; if (!ok) proof_failures++;
    std::fflush(stdout);
    std::printf("JSON {\"kind\":\"check\",\"label\":\"%s\",\"passed\":%s}\n", message, ok ? "true" : "false");
}
static int open_descriptor_count()
{
    int count=0;
    for(int fd=0;fd<256;fd++) if(fcntl(fd,F_GETFD)!=-1) count++;
    return count;
}
static void restore(const char *name,const std::vector<uint8_t> &bytes)
{
    FILE *file=std::fopen(path(name).c_str(),"wb");
    if(!file || std::fwrite(bytes.data(),1,bytes.size(),file)!=bytes.size()) std::abort();
    std::fclose(file);
}
static std::vector<uint8_t> contents(const char *name)
{
    int size=FileLoad(path(name).c_str(),nullptr,0);
    std::vector<uint8_t> result(size);
    if(size && FileLoad(path(name).c_str(),result.data(),size)!=size) std::abort();
    return result;
}
static int helper_cases()
{
    // The source algorithms execute intact. Fault boundaries use actual short
    // files or controlled allocation/libc failures; no delegated success stub.
    for(const char *name : {"Rom.sfc","Mirror.sfc","Copier.smc"}) {
        fileTYPE file; expected(FileOpen(&file,path(name).c_str(),1),"open real SNES positive fixture");
        std::vector<uint8_t> original=contents(name);
        uint8_t *header=snes_get_header(&file);
        expected(header && file.offset==0,"SNES header succeeds and rewinds a complete source");
        uint32_t size=0; uint8_t *rom=snes_get_mirrored_rom(&file,&size);
        const bool copier=original.size()&512;
        const size_t body=original.size()-(copier ? 512 : 0);
        size_t padded=1; while(padded<body) padded*=2;
        expected(rom && size==padded,"complete SNES mirror preserves padded output length");
        expected(rom && std::equal(original.begin()+(copier?512:0),original.end(),rom),"SNES mirror preserves actual body/copy-header removal");
        if(rom && body==49152) expected(std::equal(rom+32768,rom+49152,rom+49152),"non-power-of-two SNES trailing bank preserves mirroring bytes");
        std::free(rom);
    }
    {
        // Exact two-bank fixture lets the historical scan advance and finish
        // safely, while exercising the changed BIOS/cart tag bounds.
        fileTYPE file; expected(FileOpen(&file,path("Sufami.sfc").c_str(),1),"open synthetic exact two-bank Sufami fixture");
        const uint8_t *header=snes_get_header(&file);
        uint32_t address=0, size=0;
        if(header) { std::memcpy(&address,header+4,4); std::memcpy(&size,header+8,4); }
        expected(header && header[1]==0x24,"Sufami BIOS plus single-cart flags retain expected0x24");
        expected(header && address==1024*1024 && size==2*1024*1024 && file.offset==0,
                 "Sufami header retains second-bank address/full size and rewinds");
        expected(header && header[0]==0x39 && !(header[3]&1),"Sufami ROM/RAM fields and region retain expected mapping");
    }
    // A short full read is a helper error, even though it returns positive.
    for(int keep : {0,1024}) {
        const std::vector<uint8_t> original=contents("Rom.sfc");
        fileTYPE file; FileOpen(&file,path("Rom.sfc").c_str(),1);
        truncate(path("Rom.sfc").c_str(),keep);
        expected(snes_get_header(&file)==nullptr,"SNES header rejects zero/positive-short full reads");
        uint32_t out=123;
        expected(snes_get_mirrored_rom(&file,&out)==nullptr,"SNES mirror rejects zero/positive-short full reads");
        FileClose(&file); restore("Rom.sfc",original);
    }
    {
        fileTYPE file; FileOpen(&file,path("Rom.sfc").c_str(),1);
        fail_alloc=true; expected(!snes_get_header(&file),"SNES header allocation failure is truthful");
        uint32_t size=1; expected(!snes_get_mirrored_rom(&file,&size),"SNES mirror allocation failure is truthful"); fail_alloc=false;
        seek_lba_calls=0; fail_lba_at=1; expected(!snes_get_header(&file),"SNES header rejects initial seek failure");
        seek_lba_calls=0; fail_lba_at=2; expected(!snes_get_header(&file),"SNES header rejects final rewind failure"); fail_lba_at=0;
    }
#if HAVE_CHECKED_TRANSFER
    {
        fileTYPE file; FileOpen(&file,path("EmptyCopier.smc").c_str(),1); uint32_t size=123;
        expected(!snes_get_mirrored_rom(&file,&size),"copier-only ROM has no usable mirrored payload");
        expected(!snes_get_header(&file),"copier-only ROM has no usable header source");
    }
#endif // Legacy empty-body mirror makes no progress (separate timeout evidence).
#if HAVE_CHECKED_TRANSFER
    // Historical code's unchecked fixed tags can read outside small mallocs.
    // Execute these bounds controls only with the checked helper present;
    // historical failure proof above uses safe allocated full-size fixtures.
    { fileTYPE file; FileOpen(&file,path("Small.sfc").c_str(),1); expected(snes_get_header(&file)!=nullptr,"small complete raw SNES header has bounded optional tag probes"); }
#endif
    for(const char *name : {"Tape.uef","TapeGzip.uef","TapeArchive.zip/Raw.uef","TapeArchive.zip/Gzip.uef","HeaderOnly.uef","Mixed.uef"}) {
        fileTYPE file; FileOpen(&file,path(name).c_str(),1); transfer_bytes.clear();
        int descriptors=open_descriptor_count();
        expected(UEF_FileSend(&file,1)==1,"real raw/gzip/header-only/mixed UEF returns completed");
        expected(open_descriptor_count()==descriptors,"successful UEF closes its temporary file");
        if(!std::strcmp(name,"Tape.uef") || !std::strcmp(name,"TapeGzip.uef") || std::strstr(name,"TapeArchive.zip/"))
            expected(transfer_bytes==std::vector<uint8_t>({0x41,0x40}),"one-byte tape framing and final zero padding remain byte-exact");
        if(!std::strcmp(name,"HeaderOnly.uef")) expected(transfer_bytes.empty(),"valid header-only UEF is zero-output completion");
        if(!std::strcmp(name,"Mixed.uef")) expected(transfer_bytes==std::vector<uint8_t>({0xF0,0x41,0x50,0x50}),"carrier/gap/dummy plus ignored metadata/unknown chunks preserve encoded bytes");
    }
    for(const char *name : {"BadHeader.uef","BadChunk.uef","BadCarrier.uef","BadGzip.uef"}) {
        fileTYPE file; FileOpen(&file,path(name).c_str(),1); transfer_bytes.clear();
        int descriptors=open_descriptor_count();
        expected(UEF_FileSend(&file,1)==0,"UEF rejects invalid header/chunk/required-field/gzip");
        expected(transfer_bytes.empty(),"invalid UEF sends no encoded chunk");
        expected(open_descriptor_count()==descriptors,"failed UEF closes its temporary file");
    }
    for(int mode=0;mode<4;mode++) {
#if !HAVE_CHECKED_TRANSFER
        if(mode==0) continue; // Legacy code dereferences an unchecked NULL tmpfile.
#endif
        fileTYPE file; FileOpen(&file,path("Tape.uef").c_str(),1); transfer_bytes.clear();
        fail_temp=mode==0; fail_temp_write=mode==1; fail_temp_seek=mode==2; fail_encode_read=mode==3;
        int descriptors=open_descriptor_count();
        expected(UEF_FileSend(&file,1)==0,"UEF rejects temp allocation/write/seek/encoding-read failure");
        expected(transfer_bytes.empty(),"failed UEF current encoded chunk is not transmitted");
        expected(open_descriptor_count()==descriptors,"UEF injected failure has no temporary descriptor leak");
        fail_temp=fail_temp_write=fail_temp_seek=fail_encode_read=false;
    }
    {
        const std::vector<uint8_t> original=contents("Tape.uef");
        fileTYPE file; FileOpen(&file,path("Tape.uef").c_str(),1); transfer_bytes.clear();
        truncate(path("Tape.uef").c_str(),2);
        int descriptors=open_descriptor_count();
        expected(!UEF_FileSend(&file,1) && transfer_bytes.empty(),"raw UEF copy requires its entire advertised source");
        expected(open_descriptor_count()==descriptors,"short raw-copy path closes temporary file");
        FileClose(&file); restore("Tape.uef",original);
    }
    for(const char *name : {"Composite.rom","BadOffset.rom","SmallComposite.rom"}) {
#if !HAVE_CHECKED_TRANSFER
        if(!std::strcmp(name,"SmallComposite.rom")) continue; // Old stale header underflows into a multi-GiB loop.
#endif
        transfer_bytes.clear(); download_states.clear(); file_crc=0x12345678;
        UserIoFileTxResult result=production_user_io_file_tx_checked(path(name).c_str(),0,0,1,1,0);
        bool valid=!std::strcmp(name,"Composite.rom");
        expected(result==(valid?UserIoFileTxResult::CompletedHostPath:UserIoFileTxResult::RejectedBeforeDownload),"composite header/offset classification is bounded and truthful");
        expected(valid ? transfer_bytes==std::vector<uint8_t>({'D','A','T','A'}) : download_states.empty() && file_crc==0x12345678,
            "valid small composite sends payload; rejected composite preserves CRC/no download");
    }
    {
        bool supported=contents("sparse-supported.txt")==std::vector<uint8_t>({'1'});
        if(supported) {
            transfer_bytes.clear(); download_states.clear();
            expected(production_user_io_file_tx_checked(path("LargeComposite.rom").c_str(),0,0,1,1,0)==UserIoFileTxResult::CompletedHostPath
                     && transfer_bytes==std::vector<uint8_t>({'D','A','T','A'}),"large sparse composite container with small remaining payload completes");
#if HAVE_CHECKED_TRANSFER
            transfer_bytes.clear(); download_states.clear(); file_crc=0xAABBCCDD;
            expected(production_user_io_file_tx_checked(path("LargeComposite.rom").c_str(),0,0,1,0,0)==UserIoFileTxResult::RejectedBeforeDownload
                     && download_states.empty() && file_crc==0xAABBCCDD,"unsupported remaining ordinary payload rejects before download");
#endif
        }
        std::printf("JSON {\"kind\":\"sparse_capability\",\"supported\":%s}\n",supported?"true":"false");
    }
    transfer_bytes.clear(); mapped_bytes.clear();
    expected(production_user_io_file_tx_checked(path("Rom.sfc").c_str(),0,0,1,0,0xFFFFFFF0)==UserIoFileTxResult::CompletedHostPath
             && mapped_bytes.empty() && transfer_bytes==contents("Rom.sfc"),"wide address addition prevents wrapped mapped-memory range");
    helper_core=2; transfer_bytes.clear();
    expected(production_user_io_file_tx_checked(path("Tape.uef").c_str(),1,0,1,0,0)==UserIoFileTxResult::CompletedHostPath
             && transfer_bytes==std::vector<uint8_t>({0x41,0x40}),"checked transfer recognizes successful delegated UEF");
    transfer_bytes.clear();
    expected(production_user_io_file_tx_checked(path("BadChunk.uef").c_str(),1,0,1,0,0)==UserIoFileTxResult::IncompleteAfterStart
             && transfer_bytes.empty() && !file_crc,"checked transfer retires failed delegated UEF");
    helper_core=1; transfer_bytes.clear();
    std::vector<uint8_t> rom=contents("Rom.sfc");
    expected(production_user_io_file_tx_checked(path("Rom.sfc").c_str(),1,0,1,0,0)==UserIoFileTxResult::CompletedHostPath
             && transfer_bytes.size()==512+rom.size() && file_crc==mz_crc32(0,rom.data(),rom.size()),"checked SNES header/mirror succeeds with own primary CRC");
    transfer_bytes.clear();
    expected(production_user_io_file_tx_checked(path("Music.spc").c_str(),1,0,1,0,0)==UserIoFileTxResult::CompletedHostPath
             && transfer_bytes.size()==66048,"standard SPC accepts needed256-byte DSP tail and full64KiB window");
    transfer_bytes.clear(); msu_commands.clear(); map_failure=true;
    expected(production_user_io_file_tx_checked(path("Rom.sfc").c_str(),1,0,1,0,0)==UserIoFileTxResult::CompletedHostPath
             && !has_cd && file_crc==mz_crc32(0,rom.data(),rom.size()),"failed optional MSU preload does not enable CD or replace completed outer ROM CRC");
    map_failure=false; transfer_bytes.clear(); msu_commands.clear();
    expected(production_user_io_file_tx_checked(path("Rom.sfc").c_str(),1,0,1,0,0)==UserIoFileTxResult::CompletedHostPath
             && has_cd && file_crc==mz_crc32(0,rom.data(),rom.size()),"completed mapped MSU preload keeps outer ROM outcome and CRC");
    transfer_bytes.clear();
    expected(production_user_io_file_tx_checked(path("Game.bs").c_str(),1,0,1,0,0)==UserIoFileTxResult::CompletedHostPath
             && transfer_bytes.size()==512+32768+32768,"valid BSX auxiliary plus primary preserves complete output path");
    {
        const std::vector<uint8_t> bios=contents("bsx_bios.rom");
        truncate_aux_after_header=true; path_for_aux=path("bsx_bios.rom"); transfer_bytes.clear();
        expected(production_user_io_file_tx_checked(path("Game.bs").c_str(),1,0,1,0,0)==UserIoFileTxResult::IncompleteAfterStart
                 && transfer_bytes.size()==512 && !file_crc,"BSX auxiliary truncation after its header stops before failed stream or primary");
        restore("bsx_bios.rom",bios);
    }
    transfer_bytes.clear();
    expected(production_user_io_file_tx_checked(path("ShortMusic.spc").c_str(),1,0,1,0,0)==UserIoFileTxResult::IncompleteAfterStart
             && transfer_bytes.size()==256 && !file_crc,"SPC inadequate DSP span stops after already delivered header");
    for(int failure=1;failure<=2;failure++) {
        transfer_bytes.clear(); main_seek_count=0; main_seek_fail_at=failure;
        expected(production_user_io_file_tx_checked(path("Music.spc").c_str(),1,0,1,0,0)==UserIoFileTxResult::IncompleteAfterStart
                 && transfer_bytes.size()==(failure==1?256:512) && !file_crc,"SPC required seek failure retires before its next span");
        main_seek_fail_at=0;
    }
    {
        const std::vector<uint8_t> original=contents("Music.spc");
        truncate_spc_after_dsp=true; path_for_aux=path("Music.spc"); transfer_bytes.clear();
        expected(production_user_io_file_tx_checked(path("Music.spc").c_str(),1,0,1,0,0)==UserIoFileTxResult::IncompleteAfterStart
                 && transfer_bytes.size()==512 && !file_crc,"SPC short memory-window read sends no failed memory chunk");
        restore("Music.spc",original);
    }
    {
        const std::vector<uint8_t> original=contents("GameCopier.bs");
        fault_active=true; fault_after_first=true; fault_applied=false; fault_basename="GameCopier.bs";
        shrink_size=0; load_path=path("GameCopier.bs"); expected_data_calls=data_calls; transfer_bytes.clear();
        expected(production_user_io_file_tx_checked(path("GameCopier.bs").c_str(),1,0,1,0,0)==UserIoFileTxResult::IncompleteAfterStart
                 && transfer_bytes.size()==512 && !file_crc,"BSX incomplete copier-strip read stops before auxiliary/primary payload");
        fault_active=false; fault_after_first=false; restore("GameCopier.bs",original);
    }
    helper_core=0;
    fault_active=true; fault_after_first=false; fault_applied=false; fault_basename="Rom.eZ0"; shrink_size=0; load_path=path("Rom.eZ0");
    const std::vector<uint8_t> boot_rom=contents("Rom.eZ0");
    expected(!configured_boot(path("Rom.eZ0").c_str(),0,false),"configured slot-zero partial delivery must not set boot readiness");
    restore("Rom.eZ0",boot_rom); fault_applied=false;
    expected(!configured_boot(path("Rom.eZ0").c_str(),1,true),"configured CPC eZ0 partial delivery must not set boot readiness");
    restore("Rom.eZ0",boot_rom); fault_active=false;
    expected(configured_boot(path("Rom.eZ0").c_str(),0,false) && configured_boot(path("Rom.eZ0").c_str(),1,true),"completed slot-zero/CPC paths preserve boot readiness");
    expected(configured_attempts==4,"failed and completed configured loads retain attempted path recording");
    expected(configured_boot(path("NoSuchFile.rom").c_str(),0,false,true),"initial rejected critical field preserves earlier completed boot readiness");
    expected(configured_boot(path("Missing.eZZ").c_str(),1,true,true),"initial rejected CPC field preserves earlier completed boot readiness");
    for(const char *name : {"Rom.eZ0","Rom.eZZ"}) {
        for(int scenario=0;scenario<3;scenario++) {
            restore(name,boot_rom); fault_active=true; fault_applied=false; load_path=path(name);
            int slot=scenario==0?0:1; bool cpc=scenario==1;
            expected(configured_boot(path(name).c_str(),slot,cpc,true)==(scenario==2),
                     "later incomplete critical field clears readiness while noncritical field retains it");
        }
        restore(name,boot_rom); fault_active=false;
        expected(configured_boot(path(name).c_str(),0,false,true) && configured_boot(path(name).c_str(),1,true,true),
                 "later completed critical field retains earlier boot readiness");
        expected(configured_boot(path(name).c_str(),1,true,false),"completed eZ0/eZZ critical field sets initially false readiness");
    }
    std::printf("JSON {\"kind\":\"helpers\",\"checks\":%u,\"failures\":%u}\n",proof_checks,proof_failures);
    return proof_failures ? 1 : 0;
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    storage_root = argv[1]; home = storage_root;
    fault_mode = argv[2];
    if(fault_mode=="helpers") { setvbuf(stdout,nullptr,_IONBF,0); return helper_cases(); }
    // Seed actual static transfer buffer and active metadata through a real
    // accepted file, rather than writing production storage from the fixture.
    prepare("Old.gba"); selected_case();
    expected(transfer_return == 1 && file_crc == 0xFD7BB204 && std::string(game_docs_get_manual()).find("Old.pdf") != std::string::npos,
        "seed must establish actual accepted payload and active metadata before the fault");
    const std::string old_screenshot = last_filename, old_manual = game_docs_get_manual();
    const int old_doc_calls = doc_calls, old_cheat_calls = cheat_calls;
    const uint32_t old_crc = file_crc;
    transfer_bytes.clear(); read_counts.clear(); requested_counts.clear(); before_crcs.clear(); after_crcs.clear();
    download_states.clear(); ordered_events.clear();
    fpga_enables = fpga_disables = close_calls = file_handles_closed = unmap_calls = 0;
    expected_data_calls = data_calls; const int old_gameids = gameid_calls, old_states = state_calls, old_mounts = mount_calls;
    const char *name = "Short.gba";
    if (fault_mode == "partial-second" || fault_mode == "zero-second") name = "Partial.gba";
    else if (fault_mode.find("mapped") == 0) name = "Mapped.gba";
    else if (fault_mode == "zip-error") name = "Broken.zip/Failed.gba";
    else if (fault_mode == "valid") name = "New.gba";
    else if (fault_mode == "empty") name = "Empty.gba";
    else if (fault_mode == "missing") name = "NoSuchFile.gba";
    else if (fault_mode == "aux-zero") name = "GoombaTest.gb";
    prepare(name);
    std::vector<unsigned char> expected_payload;
    if (fault_mode != "zip-error" && fault_mode != "missing")
    {
        const int expected_size = FileLoad(path(name).c_str(), nullptr, 0);
        expected_payload.resize(expected_size > 0 ? expected_size : 0);
        if (expected_size > 0) expected(FileLoad(path(name).c_str(), expected_payload.data(), expected_size) == expected_size,
            "source reference must use an actual complete pre-fault file read");
    }
    fault_basename = fault_mode == "aux-zero" ? "goomba.rom" : (fault_mode == "zip-error" ? "Failed.gba" : name);
    fault_active = fault_mode != "valid" && fault_mode != "empty" && fault_mode != "missing" && fault_mode != "zip-error";
    fault_after_first = fault_mode == "partial-second" || fault_mode == "zero-second";
    shrink_size = fault_mode == "zero-first" || fault_mode == "zero-second" || fault_mode == "mapped-zero" || fault_mode == "aux-zero" ? 0 : 1024;
    if (fault_after_first) shrink_size += 4096;
    if (fault_mode.find("mapped") == 0) load_addr = 0x20000000;
    if (fault_mode == "aux-zero") forced_gba = true;
    target_active = true;
    selected_case();
    expected(spi_balanced && !fpga_enabled && fpga_enables == fpga_disables, "actual transfer SPI helpers must close every per-call chip-select transaction");
    expected(!download_active, "baseline transfer must eventually signal end of download");
    expected(fault_mode == "missing" || file_handles_closed > 0, "actual opened file or ZIP reader must close its OS file handle");
    const bool rejected=fault_mode=="missing";
    const bool valid=fault_mode=="valid" || fault_mode=="empty";
    expected(transfer_return==(valid ? 1 : 0),"legacy wrapper must accept only a completed host path");
    if(rejected) {
        expected(transfer_bytes.empty() && download_states.empty() && last_filename==old_screenshot && file_crc==old_crc
            && game_docs_get_manual()==old_manual && doc_calls==old_doc_calls && cheat_calls==old_cheat_calls,
            "initial rejection preserves active metadata and avoids download");
    } else if(valid) {
        expected(doc_calls==old_doc_calls+1 && cheat_calls==old_cheat_calls+1 && last_filename!=old_screenshot,
            "completed host path commits actual metadata");
        expected(gameid_calls==old_gameids+1 && state_calls==old_states+1 && mount_calls==old_mounts+1,
            "completed path retains success-only side effects");
        expected(transfer_bytes==expected_payload,"valid payload must match actual complete source bytes");
    } else {
        expected(!file_crc && !last_filename[0] && !game_docs_manual_available() && cheats.empty() && !cheat_zip[0],
            "incomplete host path invalidates CRC and clears unknown game identity");
        expected(doc_calls==old_doc_calls && cheat_calls==old_cheat_calls,"partial clear must not run asset initialization");
        expected(gameid_calls==old_gameids && mount_calls==old_mounts && state_calls==old_states+1,
            "incomplete skips success-only GAMEID/save but does not undo early savestate effects");
        expected(download_states==std::vector<int>({1,0}),"started failure has one start and one end signal");
        const size_t delivered=fault_after_first ? 4096 : 0;
        expected(transfer_bytes.size()==delivered,"failed SPI/aux chunk and all following data must not be sent");
        if(fault_after_first) expected(std::equal(transfer_bytes.begin(),transfer_bytes.end(),expected_payload.begin()),
            "an already delivered first chunk remains exactly the source prefix");
        if(fault_mode.find("mapped")==0) {
            expected(unmap_calls==1 && mapped_bytes.size()==4096 && mapped_bytes.back()==0xD3,
                "failed mapped read unmaps and retains untouched RAM tail without CRC");
            if(fault_mode=="mapped-short") expected(std::equal(mapped_bytes.begin(),mapped_bytes.begin()+1024,expected_payload.begin()),
                "partial direct mapped writes may already modify RAM and are not rolled back");
        }
        if(fault_mode=="aux-zero") expected(read_counts.size()==1 && read_counts[0]==0,
            "failed auxiliary read stops before primary delivery");
    }
    std::printf("JSON {\"kind\":\"result\",\"mode\":\"%s\",\"return\":%d,\"delivered_bytes\":%u,\"read_calls\":%u,\"crc\":%u,\"download_calls\":%u,\"spi_enables\":%d,\"spi_disables\":%d,\"closed_handles\":%d,\"unmaps\":%d,\"checks\":%u,\"failures\":%u}\n",
        fault_mode.c_str(), transfer_return, (unsigned)transfer_bytes.size(), (unsigned)read_counts.size(), file_crc,
        (unsigned)download_states.size(), fpga_enables, fpga_disables, file_handles_closed, unmap_calls, proof_checks, proof_failures);
    std::printf("JSON {\"kind\":\"resource_events\",\"events\":[");
    for (size_t i = 0; i < ordered_events.size(); i++) std::printf("%s\"%s\"", i ? "," : "", ordered_events[i].c_str());
    std::printf("]}\n");
    mz_zip_reader_end(&last_zip_archive);
    if (last_zip_cfile) { std::fclose(last_zip_cfile); last_zip_cfile = nullptr; }
    return proof_failures ? 1 : 0;
}

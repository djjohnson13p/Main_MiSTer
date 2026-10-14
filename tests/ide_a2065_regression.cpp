// Production extraction harness: synthetic SPI, shared memory and Ethernet.
// Host event ordering is observable; ARM fences and DDR3 atomicity are not.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "minimig_a2065_types.h"
#include "minimig_a2065_ddr3.h"

#define DBG(...) ((void)0)
struct ide_config { uint32_t bitoff; }; // Only the field read by extracted waits.
#include "ide_a2065_state.inc"

struct Store { unsigned long offset; uint64_t value; };
struct CheckBudgetReached {};
alignas(8) static uint8_t mailbox[DDR3_FLAT_WINDOW_SIZE];
static std::vector<Store> stores;
static unsigned checks, budget, misses, poll_calls, memory_reads, recv_calls, filter_calls;
static unsigned spi_phase;
static uint16_t spi_response, ready_request, other_request;
static uint32_t selected_offset;
static bool io_enabled, spi_balanced, ready_on_csr, fallback_byte;

static void set_word(unsigned long offset, uint64_t value)
{
    std::memcpy(mailbox + offset, &value, sizeof value);
}
static uint64_t get_word(unsigned long offset)
{
    uint64_t value;
    std::memcpy(&value, mailbox + offset, sizeof value);
    return value;
}
static void *observed_memcpy(void *destination, const void *source, size_t size)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(mailbox);
    const uintptr_t src = reinterpret_cast<uintptr_t>(source);
    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination);
    if (src >= base && src < base + sizeof mailbox) memory_reads++;
    void *result = std::memcpy(destination, source, size);
    if (dst >= base && dst < base + sizeof mailbox) {
        uint64_t value = 0;
        if (size != sizeof value) throw std::runtime_error("Unexpected mailbox store width");
        std::memcpy(&value, source, size);
        stores.push_back({static_cast<unsigned long>(dst - base), value});
    }
    return result;
}
static void observe_poll()
{
    poll_calls++;
    spi_balanced &= !io_enabled;
}
void EnableIO()
{
    if (checks >= budget) throw CheckBudgetReached{};
    spi_balanced &= !io_enabled;
    io_enabled = true;
    spi_phase = 0;
    checks++;
    const bool ready = ready_on_csr ? !(get_word(DDR3_CSR_OFF) & CSR0_RINT) : checks > misses;
    spi_response = other_request | (ready ? static_cast<uint16_t>(ready_request << selected_offset) : 0);
}
void DisableIO() { spi_balanced &= io_enabled; io_enabled = false; }
uint16_t spi_w(uint16_t command)
{
    spi_balanced &= io_enabled;
    if (!spi_phase++) {
        spi_balanced &= command == UIO_DMA_SDIO;
        return fallback_byte ? 0 : spi_response;
    }
    spi_balanced &= command == 0;
    return fallback_byte ? spi_response : 0;
}
static void chip_init() { throw std::runtime_error("Unexpected guest initialization"); }
int rings_rx_has_space() { return 1; }
int ethernet_recv_nb(uint8_t *, int) { recv_calls++; return 0; }
void gotfunc(const uint8_t *, int) { throw std::runtime_error("Unexpected RX frame"); }
void ethernet_clear_filter() { filter_calls++; }
void mac_get_real(uint8_t *mac) { std::memset(mac, 0, 6); }
int ethernet_set_mac_filter(const uint8_t *) { filter_calls++; return 1; }

#include "ide_a2065_production.inc"

static void reset_fixture(uint32_t offset = 0)
{
    std::memset(mailbox, 0, sizeof mailbox);
    std::memset((void *)csr, 0, sizeof csr);
    map = mailbox;
    card_up = 1;
    cmd_rd_idx = 11;
    set_word(DDR3_CMD_WPTR_OFF, cmd_rd_idx);
    mode = MODE_BPF;
    prom_active = rx_batching = 0;
    rap = am_initialized = 0;
    am_mode = 0;
    on_interrupt = on_interrupt_cb;
    on_transmit = nullptr;
    stores.clear();
    checks = misses = poll_calls = memory_reads = recv_calls = filter_calls = spi_phase = 0;
    budget = 64;
    ready_request = 5;
    other_request = 0;
    selected_offset = offset;
    spi_response = 0;
    io_enabled = ready_on_csr = fallback_byte = false;
    spi_balanced = true;
}
static void queue_ack(uint16_t initial_csr = 0x04F3, uint32_t read_index = 11)
{
    csr[0] = initial_csr;
    cmd_rd_idx = read_index;
    set_word(DDR3_CSR_OFF, initial_csr);
    set_word(DDR3_INT_OFF, 1);
    const unsigned long offset = DDR3_CMD_RING_OFF + (read_index & DDR3_CMD_RING_MASK) * DDR3_CMD_RING_STRIDE;
    set_word(offset, 1 | (static_cast<uint64_t>(0x0440) << DDR3_CMD_DATA_SHIFT));
    set_word(DDR3_CMD_WPTR_OFF, static_cast<uint32_t>(read_index + 1));
    ready_on_csr = true;
}
static bool store_order_valid()
{
    if (stores.empty() || stores.size() % 2) return false;
    for (size_t index = 0; index < stores.size(); index += 2) {
        if (stores[index].offset != DDR3_CSR_OFF || stores[index + 1].offset != DDR3_INT_OFF) return false;
    }
    return true;
}
static unsigned failures, cases;
static void report(const std::string &name, bool passed, bool exhausted = false, uint16_t returned = 0)
{
    cases++;
    if (!passed) failures++;
    std::printf("JSON {\"case\":\"%s\",\"passed\":%s,\"budget_exhausted\":%s,\"checks\":%u,"
                "\"poll_calls\":%u,\"returned_request\":%u,\"csr0\":%u,\"shadow_csr0\":%u,"
                "\"command_read_index\":%u,\"command_write_index\":%u,\"int_state\":%llu,"
                "\"memory_reads\":%u,\"recv_calls\":%u,\"stores\":%zu,\"csr_before_int\":%s,\"spi_balanced\":%s}\n",
                name.c_str(), passed ? "true" : "false", exhausted ? "true" : "false", checks,
                poll_calls, returned, static_cast<unsigned>(csr[0]),
                static_cast<unsigned>(get_word(DDR3_CSR_OFF) & 0xffff), cmd_rd_idx,
                static_cast<unsigned>(get_word(DDR3_CMD_WPTR_OFF)),
                static_cast<unsigned long long>(get_word(DDR3_INT_OFF)), memory_reads, recv_calls,
                stores.size(), store_order_valid() ? "true" : "false",
                (spi_balanced && !io_enabled) ? "true" : "false");
}
using Wait = uint16_t (*)(ide_config *);
static uint16_t run_wait(Wait wait, bool &exhausted)
{
    ide_config ide{selected_offset};
    try { return wait(&ide); }
    catch (const CheckBudgetReached &) { exhausted = true; return 0; }
}
static void wait_cases(const std::string &prefix, Wait wait)
{
    for (unsigned count : {0U, 15U, 16U, 32U}) {
        reset_fixture();
        misses = count;
        bool exhausted = false;
        const uint16_t req = run_wait(wait, exhausted);
        report(prefix + "-misses-" + std::to_string(count), !exhausted && req == 5 && checks == count + 1 &&
               poll_calls == count / 16 && spi_balanced && !io_enabled, exhausted, req);
    }
    reset_fixture(3);
    misses = 16;
    other_request = 7;
    bool exhausted = false;
    uint16_t req = run_wait(wait, exhausted);
    report(prefix + "-port-mask", !exhausted && req == 5 && checks == 17 && poll_calls == 1 && spi_balanced,
           exhausted, req);
    reset_fixture(3);
    fallback_byte = true;
    ready_request = 7;
    exhausted = false;
    req = run_wait(wait, exhausted);
    report(prefix + "-non5-byte-response", !exhausted && req == 7 && checks == 1 && poll_calls == 0 && spi_balanced,
           exhausted, req);
    reset_fixture();
    card_up = 0;
    misses = 32;
    exhausted = false;
    req = run_wait(wait, exhausted);
    report(prefix + "-card-off", !exhausted && req == 5 && poll_calls == 2 && memory_reads == 0 &&
           recv_calls == 0 && filter_calls == 0 && stores.empty() && spi_balanced, exhausted, req);
    reset_fixture();
    queue_ack();
    exhausted = false;
    req = run_wait(wait, exhausted);
    report(prefix + "-queued-csr0-ack", !exhausted && req == 5 && checks == 17 && poll_calls == 1 &&
           csr[0] == 0x0073 && get_word(DDR3_CSR_OFF) == 0x0073 && cmd_rd_idx == 12 &&
           get_word(DDR3_INT_OFF) == (uint64_t(12) << 32 | 2) && store_order_valid() && spi_balanced,
           exhausted, req);
}
int main()
{
    wait_cases("read", wait_from_read);
    wait_cases("write", wait_from_write);
    // Direct intact poll proves acknowledgement publication, callback ordering
    // with another interrupt cause left pending, and uint32 index wrapping.
    for (uint32_t index : {11U, UINT32_MAX}) {
        reset_fixture();
        queue_ack(0x06F3, index);
        a2065_poll();
        const uint32_t next = index + 1;
        report("poll-pending-tint-index-" + std::to_string(index), csr[0] == 0x02F3 &&
               get_word(DDR3_CSR_OFF) == 0x02F3 && cmd_rd_idx == next &&
               get_word(DDR3_INT_OFF) == (uint64_t(next) << 32 | 3) && stores.size() == 4 && store_order_valid());
    }
    for (uint32_t index : {0U, 1U, 0x80000000U, UINT32_MAX}) {
        for (unsigned flags = 0; flags < 4; flags++) {
            reset_fixture();
            cmd_rd_idx = index;
            csr[0] = static_cast<uint16_t>((flags & 1 ? CSR0_INTR : 0) | (flags & 2 ? CSR0_INEA : 0));
            csr[1] = 0x1357; csr[2] = 0x2468; csr[3] = 0x9abc;
            push_csr_shadow();
            update_int_state();
            const uint64_t expected_csr = uint64_t(csr[0]) | (uint64_t(csr[1]) << 16) |
                                          (uint64_t(csr[2]) << 32) | (uint64_t(csr[3]) << 48);
            report("packing-index-" + std::to_string(index) + "-flags-" + std::to_string(flags),
                   get_word(DDR3_CSR_OFF) == expected_csr &&
                   get_word(DDR3_INT_OFF) == (uint64_t(index) << 32 | 2 | (flags == 3 ? 1 : 0)) &&
                   stores.size() == 2 && store_order_valid());
        }
    }
    std::printf("IDE/A2065 regression: %u passed, %u failed; finite SPI budget, simulated peripherals.\n",
                cases - failures, failures);
    return failures ? 1 : 0;
}

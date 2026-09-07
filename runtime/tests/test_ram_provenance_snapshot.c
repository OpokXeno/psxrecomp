#include "ram_provenance.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

enum {
    TEST_RAM_SIZE = 64u,
    SNAP_HEADER_BYTES = 32u,
    SNAP_RECORD_BYTES = 40u,
};

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static int rejected_without_mutation(const uint8_t *wire, uint32_t size,
                                     uint32_t live_address,
                                     uint64_t live_receipt)
{
    RamProvenanceSnapshot *decoded = NULL;
    RamProvenanceSource source;

    CHECK(!ram_provenance_snapshot_decode(
        wire, size, TEST_RAM_SIZE, &decoded));
    CHECK(decoded == NULL);
    CHECK(ram_provenance_source_word(live_address, &source));
    CHECK(source.receipt == live_receipt);
    return 0;
}

int main(void)
{
    const uint32_t completed_address = 0u;
    const uint32_t partial_address = 4u;
    const uint32_t cpu_address = 8u;
    const uint32_t newer_address = 12u;
    const uint32_t cpu_value = UINT32_C(0x44332211);
    RamProvenanceSource completed = {
        .kind = RAM_PROVENANCE_SOURCE_MDEC_DMA1,
        .format = 3u,
    };
    RamProvenanceSource newer = {
        .kind = RAM_PROVENANCE_SOURCE_MDEC_DMA1,
        .format = 2u,
    };
    RamProvenanceSource restored;
    RamProvenanceSnapshot *decoded = NULL;
    uint8_t *wire;
    uint8_t *malformed;
    uint32_t size;
    uint32_t writer_pc;
    uint32_t return_address;
    uint64_t cpu_receipt;
    uint64_t saved_counter;

    CHECK(ram_provenance_init(TEST_RAM_SIZE));
    ram_provenance_set_cpu_tracking(true);
    completed.receipt = ram_provenance_publish_event();
    CHECK(completed.receipt != 0u);
    ram_provenance_note_source_word(completed_address, &completed);
    ram_provenance_note_source_word(partial_address, &completed);
    ram_provenance_invalidate_range(partial_address + 1u, 1u);
    ram_provenance_note_cpu_store(
        UINT32_C(0xac000000), cpu_address, cpu_value);
    ram_provenance_note_command_word(
        cpu_address, UINT32_C(0x20000000), UINT32_C(0x80012340),
        UINT32_C(0x80045678));
    saved_counter = ram_provenance_publish_event();

    size = ram_provenance_snapshot_bytes();
    CHECK(size >= SNAP_HEADER_BYTES + 3u * SNAP_RECORD_BYTES);
    wire = (uint8_t *)malloc(size);
    malformed = (uint8_t *)malloc(size);
    CHECK(wire != NULL && malformed != NULL);
    CHECK(ram_provenance_snapshot_write(wire, size));

    newer.receipt = ram_provenance_publish_event();
    ram_provenance_note_source_word(newer_address, &newer);
    CHECK(ram_provenance_source_word(newer_address, &restored));

    memcpy(malformed, wire, size);
    malformed[SNAP_HEADER_BYTES + 38u] = UINT8_C(0xff);
    CHECK(rejected_without_mutation(
        malformed, size, newer_address, newer.receipt) == 0);
    CHECK(rejected_without_mutation(
        wire, size - 1u, newer_address, newer.receipt) == 0);

    memcpy(malformed, wire, size);
    put_u32(malformed + 16u, UINT32_MAX);
    CHECK(rejected_without_mutation(
        malformed, size, newer_address, newer.receipt) == 0);

    memcpy(malformed, wire, size);
    put_u64(malformed + SNAP_HEADER_BYTES + 24u, UINT64_MAX);
    CHECK(rejected_without_mutation(
        malformed, size, newer_address, newer.receipt) == 0);

    CHECK(ram_provenance_snapshot_decode(
        wire, size, TEST_RAM_SIZE, &decoded));
    ram_provenance_snapshot_commit(decoded);
    decoded = NULL;
    CHECK(ram_provenance_source_word(completed_address, &restored));
    CHECK(restored.kind == completed.kind && restored.format == completed.format &&
          restored.receipt == completed.receipt);
    CHECK(!ram_provenance_source_word(partial_address, &restored));
    CHECK(!ram_provenance_source_word(newer_address, &restored));
    CHECK(ram_provenance_cpu_word_receipt(
        cpu_address, cpu_value, &cpu_receipt));
    CHECK(cpu_receipt != 0u && cpu_receipt <= saved_counter);
    CHECK(ram_provenance_last_writer(
        cpu_address, &writer_pc, &return_address));
    CHECK(writer_pc == UINT32_C(0x80012340) &&
          return_address == UINT32_C(0x80045678));
    CHECK(ram_provenance_publish_event() == saved_counter + 1u);

    free(malformed);
    free(wire);
    ram_provenance_set_cpu_tracking(false);
    return 0;
}

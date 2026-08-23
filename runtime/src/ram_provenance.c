#include "ram_provenance.h"

#include <stdlib.h>
#include <string.h>

typedef struct RamProvenanceEntry {
    uint32_t pc;
    uint32_t return_address;
    uint32_t cpu_value;
    uint64_t cpu_revision;
    uint8_t cpu_valid_bytes;
} RamProvenanceEntry;

static RamProvenanceEntry *main_ram_writers;
static size_t main_ram_writer_count;
static uint64_t cpu_revision;
static unsigned cpu_speculative_depth;
static bool cpu_tracking_enabled;
static bool cpu_reset_pending;

static bool main_ram_offset(uint32_t address, uint32_t *out_offset) {
    uint32_t physical = address & UINT32_C(0x1fffffff);
    const size_t main_ram_size = main_ram_writer_count * sizeof(uint32_t);

    if (out_offset == NULL || main_ram_size == 0u ||
        physical >= UINT32_C(0x00800000))
        return false;
    physical &= (uint32_t)(main_ram_size - 1u);
    *out_offset = physical;
    return true;
}

static uint64_t next_cpu_revision(void) {
    if (++cpu_revision == 0u) {
        if (main_ram_writers != NULL)
            memset(main_ram_writers, 0,
                   main_ram_writer_count * sizeof(*main_ram_writers));
        cpu_revision = 1u;
    }
    return cpu_revision;
}

static RamProvenanceEntry *writer_slot(uint32_t address) {
    if ((address & 3u) != 0u) return NULL;
    if ((size_t)(address >> 2u) < main_ram_writer_count)
        return &main_ram_writers[address >> 2u];
    return NULL;
}

bool ram_provenance_init(size_t main_ram_size) {
    const size_t required_count = main_ram_size / sizeof(uint32_t);

    if (required_count != main_ram_writer_count) {
        RamProvenanceEntry *replacement = required_count != 0u
            ? (RamProvenanceEntry *)calloc(
                  required_count, sizeof(*replacement))
            : NULL;

        if (required_count != 0u && replacement == NULL) return false;
        free(main_ram_writers);
        main_ram_writers = replacement;
        main_ram_writer_count = required_count;
    }
    ram_provenance_reset();
    return true;
}

void ram_provenance_reset(void) {
    if (cpu_speculative_depth != 0u) {
        cpu_reset_pending = true;
        return;
    }
    if (main_ram_writers != NULL)
        memset(main_ram_writers, 0,
               main_ram_writer_count * sizeof(*main_ram_writers));
    (void)next_cpu_revision();
}

void ram_provenance_set_cpu_tracking(bool enabled) {
    if (cpu_tracking_enabled == enabled) return;
    cpu_tracking_enabled = enabled;
    ram_provenance_reset();
}

void ram_provenance_invalidate_range(uint32_t address, uint32_t width) {
    uint32_t offset;
    uint64_t revision;

    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u || width == 0u ||
        !main_ram_offset(address, &offset))
        return;
    revision = next_cpu_revision();
    for (uint32_t byte = 0u; byte < width; ++byte) {
        const uint32_t current = offset + byte;
        RamProvenanceEntry *entry;
        uint8_t mask;

        if ((size_t)current >= main_ram_writer_count * sizeof(uint32_t)) break;
        entry = &main_ram_writers[current >> 2u];
        mask = (uint8_t)(1u << (current & 3u));
        entry->cpu_valid_bytes &= (uint8_t)~mask;
        entry->cpu_revision = revision;
    }
}

void ram_provenance_note_cpu_store(uint32_t instruction, uint32_t address,
                                   uint32_t value) {
    const uint32_t opcode = instruction >> 26u;
    uint32_t width;
    uint32_t offset;
    uint64_t revision;

    (void)value;
    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u) return;
    switch (opcode) {
    case 0x28u: width = 1u; break;
    case 0x29u: width = 2u; break;
    case 0x2bu:
    case 0x3au: width = 4u; break;
    default: return;
    }
    if (!main_ram_offset(address, &offset)) return;
    revision = next_cpu_revision();
    for (uint32_t byte = 0u; byte < width; ++byte) {
        const uint32_t current = offset + byte;
        RamProvenanceEntry *entry;
        const uint32_t shift = (current & 3u) * 8u;

        if ((size_t)current >= main_ram_writer_count * sizeof(uint32_t)) break;
        entry = &main_ram_writers[current >> 2u];
        entry->cpu_value =
            (entry->cpu_value & ~(UINT32_C(0xff) << shift)) |
            (((value >> (byte * 8u)) & UINT32_C(0xff)) << shift);
        entry->cpu_valid_bytes |= (uint8_t)(1u << (current & 3u));
        entry->cpu_revision = revision;
    }
}

bool ram_provenance_cpu_word_receipt(uint32_t address, uint32_t value,
                                     uint64_t *out_receipt) {
    uint32_t offset;
    RamProvenanceEntry *entry;

    (void)value;
    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u ||
        out_receipt == NULL || (address & 3u) != 0u ||
        !main_ram_offset(address, &offset))
        return false;
    entry = &main_ram_writers[offset >> 2u];
    if (entry->cpu_valid_bytes != UINT8_C(0x0f) || entry->cpu_value != value ||
        entry->cpu_revision == 0u)
        return false;
    *out_receipt = entry->cpu_revision;
    return true;
}

bool ram_provenance_word_revision(uint32_t address, uint64_t *out_revision) {
    uint32_t offset;

    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u ||
        out_revision == NULL || (address & 3u) != 0u ||
        !main_ram_offset(address, &offset))
        return false;
    *out_revision = main_ram_writers[offset >> 2u].cpu_revision;
    return true;
}

uint64_t ram_provenance_publish_event(void) {
    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u) return 0u;
    return next_cpu_revision();
}

void ram_provenance_speculative_begin(void) {
    ++cpu_speculative_depth;
}

void ram_provenance_speculative_end(void) {
    if (cpu_speculative_depth == 0u) return;
    if (--cpu_speculative_depth == 0u && cpu_reset_pending) {
        cpu_reset_pending = false;
        ram_provenance_reset();
    }
}

void ram_provenance_note_command_word(uint32_t address, uint32_t value,
                                      uint32_t writer_pc,
                                      uint32_t return_address) {
    const uint8_t opcode = (uint8_t)(value >> 24u);
    RamProvenanceEntry *entry;

    if (opcode < 0x20u || opcode > 0x7fu || writer_pc == 0u) return;
    entry = writer_slot(address);
    if (entry == NULL) return;
    entry->pc = writer_pc;
    entry->return_address = return_address;
}

bool ram_provenance_last_writer(uint32_t address, uint32_t *out_pc,
                                uint32_t *out_return_address) {
    RamProvenanceEntry *entry;

    if (out_pc == NULL || out_return_address == NULL) return false;
    entry = writer_slot(address);
    if (entry == NULL || entry->pc == 0u) return false;
    *out_pc = entry->pc;
    *out_return_address = entry->return_address;
    return true;
}

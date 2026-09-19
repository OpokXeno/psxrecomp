#include "ram_provenance.h"
#include "pst_wire.h"

#include <stdlib.h>
#include <string.h>

typedef struct RamProvenanceEntry {
    uint32_t pc;
    uint32_t return_address;
    uint32_t cpu_value;
    uint64_t cpu_revision;
    uint64_t source_receipt;
    uint32_t source_format;
    uint8_t cpu_valid_bytes;
    uint8_t source_valid_bytes;
    uint8_t source_kind;
} RamProvenanceEntry;

struct RamProvenanceSnapshot {
    RamProvenanceEntry *writers;
    size_t writer_count;
    uint64_t revision;
};

enum {
    RAM_PROVENANCE_SNAPSHOT_MAGIC = 0x56525052u, /* "RPRV" */
    RAM_PROVENANCE_SNAPSHOT_VERSION = 1u,
    RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES = 32u,
    RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES = 40u,
    RAM_PROVENANCE_MDEC_FORMAT_MAX = 3u,
};

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

    if ((main_ram_size & (sizeof(uint32_t) - 1u)) != 0u ||
        main_ram_size > UINT32_MAX)
        return false;

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
    const uint32_t available = (uint32_t)(main_ram_writer_count * sizeof(uint32_t) - offset);
    if (width > available) width = available;
    /* Metadata is word-granular. Clear the covered byte mask in one update,
     * retaining the same event revision for unaligned heads and tails. */
    while (width) {
        const uint32_t first = offset & 3u;
        const uint32_t count = width < 4u-first ? width : 4u-first;
        const uint8_t mask = (uint8_t)(((1u << count)-1u) << first);
        RamProvenanceEntry *entry = &main_ram_writers[offset >> 2u];
        entry->cpu_valid_bytes &= (uint8_t)~mask;
        entry->source_valid_bytes &= (uint8_t)~mask;
        if (entry->source_valid_bytes == 0u) {
            entry->source_kind = RAM_PROVENANCE_SOURCE_NONE;
            entry->source_format = 0u;
            entry->source_receipt = 0u;
        }
        entry->cpu_revision = revision;
        offset += count;
        width -= count;
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
    if (width == 4u && !(offset & 3u)) {
        RamProvenanceEntry *entry = &main_ram_writers[offset >> 2u];
        entry->cpu_value = value;
        entry->cpu_valid_bytes = UINT8_C(0x0f);
        entry->cpu_revision = revision;
        return;
    }
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

void ram_provenance_note_source_word(
        uint32_t address, const RamProvenanceSource *source) {
    uint32_t offset;
    RamProvenanceEntry *entry;

    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u ||
        source == NULL || source->kind <= RAM_PROVENANCE_SOURCE_NONE ||
        source->kind > RAM_PROVENANCE_SOURCE_MDEC_DMA1 ||
        source->format > RAM_PROVENANCE_MDEC_FORMAT_MAX ||
        source->receipt == 0u || source->receipt > cpu_revision ||
        (address & 3u) != 0u ||
        !main_ram_offset(address, &offset))
        return;
    entry = &main_ram_writers[offset >> 2u];
    entry->source_kind = (uint8_t)source->kind;
    entry->source_format = source->format;
    entry->source_receipt = source->receipt;
    entry->source_valid_bytes = UINT8_C(0x0f);
    entry->cpu_valid_bytes = 0u;
}

bool ram_provenance_source_word(
        uint32_t address, RamProvenanceSource *out_source) {
    uint32_t offset;
    RamProvenanceEntry *entry;

    if (!cpu_tracking_enabled || cpu_speculative_depth != 0u ||
        out_source == NULL || (address & 3u) != 0u ||
        !main_ram_offset(address, &offset))
        return false;
    entry = &main_ram_writers[offset >> 2u];
    if (entry->source_valid_bytes != UINT8_C(0x0f) ||
        entry->source_kind == RAM_PROVENANCE_SOURCE_NONE ||
        entry->source_receipt == 0u)
        return false;
    *out_source = (RamProvenanceSource){
        .kind = (RamProvenanceSourceKind)entry->source_kind,
        .format = entry->source_format,
        .receipt = entry->source_receipt,
    };
    return true;
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

static bool entry_has_state(const RamProvenanceEntry *entry) {
    return entry->pc != 0u || entry->return_address != 0u ||
           entry->cpu_value != 0u || entry->cpu_revision != 0u ||
           entry->source_receipt != 0u || entry->source_format != 0u ||
           entry->cpu_valid_bytes != 0u || entry->source_valid_bytes != 0u ||
           entry->source_kind != RAM_PROVENANCE_SOURCE_NONE;
}

static bool entry_is_valid(const RamProvenanceEntry *entry,
                           uint64_t revision) {
    if ((entry->cpu_valid_bytes & UINT8_C(0xf0)) != 0u ||
        (entry->source_valid_bytes & UINT8_C(0xf0)) != 0u ||
        (entry->cpu_valid_bytes & entry->source_valid_bytes) != 0u ||
        entry->cpu_revision > revision ||
        (entry->cpu_valid_bytes != 0u && entry->cpu_revision == 0u))
        return false;

    if (entry->source_valid_bytes == 0u) {
        return entry->source_kind == RAM_PROVENANCE_SOURCE_NONE &&
               entry->source_format == 0u && entry->source_receipt == 0u;
    }
    return entry->source_kind == RAM_PROVENANCE_SOURCE_MDEC_DMA1 &&
           entry->source_format <= RAM_PROVENANCE_MDEC_FORMAT_MAX &&
           entry->source_receipt != 0u &&
           entry->source_receipt <= revision;
}

static bool write_entry(PstW *wire, uint32_t index,
                        const RamProvenanceEntry *entry) {
    /* Same little-endian wire record, with one bounds check/copy on LE hosts.
     * Explicit words exclude host-struct padding and preserve the reserved zero. */
    const uint32_t words[] = {
        index, entry->pc, entry->return_address, entry->cpu_value,
        (uint32_t)entry->cpu_revision, (uint32_t)(entry->cpu_revision >> 32u),
        (uint32_t)entry->source_receipt, (uint32_t)(entry->source_receipt >> 32u),
        entry->source_format,
        (uint32_t)entry->cpu_valid_bytes |
            ((uint32_t)entry->source_valid_bytes << 8u) |
            ((uint32_t)entry->source_kind << 16u),
    };
    return pst_w_pod(wire, words, sizeof(words), sizeof(words[0]));
}

uint32_t ram_provenance_snapshot_bytes(void) {
    size_t record_count = 0u;

    if (cpu_revision == 0u || cpu_speculative_depth != 0u || cpu_reset_pending ||
        (main_ram_writer_count != 0u && main_ram_writers == NULL))
        return 0u;
    for (size_t index = 0u; index < main_ram_writer_count; ++index) {
        if (!entry_is_valid(&main_ram_writers[index], cpu_revision))
            return 0u;
        if (entry_has_state(&main_ram_writers[index])) ++record_count;
    }
    if (record_count >
        (UINT32_MAX - RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES) /
            RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES)
        return 0u;
    return RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES +
           (uint32_t)record_count * RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES;
}

uint32_t ram_provenance_snapshot_capacity(void) {
    if (main_ram_writer_count >
        (UINT32_MAX - RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES) /
            RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES)
        return 0u;
    return RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES +
        (uint32_t)main_ram_writer_count * RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES;
}

static bool snapshot_write(const RamProvenanceSnapshot *snapshot,
                           uint8_t *out, uint32_t size, uint32_t *out_size) {
    const size_t writer_count = snapshot->writer_count;
    const RamProvenanceEntry *writers = snapshot->writers;
    const size_t ram_size = writer_count * sizeof(uint32_t);
    const uint64_t revision = snapshot->revision;
    uint32_t record_count;
    PstW wire;

    if (out == NULL || revision == 0u ||
        (writer_count != 0u && writers == NULL) ||
        size < RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES ||
        (size - RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES) % RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES != 0u ||
        ram_size > UINT32_MAX || writer_count > UINT32_MAX)
        return false;
    record_count =
        (size - RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES) /
        RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES;
    if (record_count > writer_count) return false;
    pst_w_init(&wire, out, size);
    if (!pst_w_u32(&wire, RAM_PROVENANCE_SNAPSHOT_MAGIC) ||
        !pst_w_u32(&wire, RAM_PROVENANCE_SNAPSHOT_VERSION) ||
        !pst_w_u32(&wire, (uint32_t)ram_size) ||
        !pst_w_u32(&wire, (uint32_t)writer_count) ||
        !pst_w_u32(&wire, record_count) ||
        !pst_w_u32(&wire, RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES) ||
        !pst_w_u64(&wire, revision))
        return false;
    /* Validate while serializing instead of repeating snapshot_bytes' full
     * scan. Bounded writes and the final size check enforce the exact count. */
    for (size_t index = 0u; index < writer_count; ++index) {
        if (!entry_is_valid(&writers[index], revision)) return false;
        if (entry_has_state(&writers[index]) &&
             !write_entry(&wire, (uint32_t)index,
                          &writers[index]))
            return false;
    }
    if (out_size) {
        /* The capacity is not the wire size. Finalize the sparse count only
         * after every entry has passed the same validation as exact writes. */
        PstW count_wire;
        pst_w_init(&count_wire, out + 16u, sizeof(uint32_t));
        if (!pst_w_u32(&count_wire, (uint32_t)(wire.written -
                RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES) /
                RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES))
            return false;
        *out_size = (uint32_t)wire.written;
        return true;
    }
    return wire.written == size;
}

bool ram_provenance_snapshot_write(uint8_t *out, uint32_t size) {
    const RamProvenanceSnapshot snapshot = {
        main_ram_writers, main_ram_writer_count, cpu_revision
    };
    if (cpu_speculative_depth != 0u || cpu_reset_pending) return false;
    return snapshot_write(&snapshot, out, size, NULL);
}

bool ram_provenance_snapshot_capture(uint8_t *out, uint32_t capacity,
                                     uint32_t *out_size) {
    if (!out_size) return false;
    *out_size = 0u;
    const RamProvenanceSnapshot snapshot = {
        main_ram_writers, main_ram_writer_count, cpu_revision
    };
    if (cpu_speculative_depth != 0u || cpu_reset_pending) return false;
    return snapshot_write(&snapshot, out, capacity, out_size);
}

RamProvenanceSnapshot *ram_provenance_snapshot_prepare(void) {
    if(main_ram_writer_count>SIZE_MAX/sizeof(*main_ram_writers))return NULL;
    RamProvenanceSnapshot *snapshot=calloc(1,sizeof(*snapshot));
    if(!snapshot)return NULL;
    const size_t bytes=main_ram_writer_count*sizeof(*main_ram_writers);
    snapshot->writers=bytes?malloc(bytes):NULL;
    if(bytes&&!snapshot->writers){free(snapshot);return NULL;}
    snapshot->writer_count=main_ram_writer_count;
    /* This is storage, not a captured revision. Prefault owned pages now;
     * clone still overwrites every entry before anything can encode it. */
    volatile unsigned char *pages=(volatile unsigned char *)snapshot->writers;
    for(size_t i=0;i<bytes;i+=4096u)pages[i]=0;
    return snapshot;
}

RamProvenanceSnapshot *ram_provenance_snapshot_clone(RamProvenanceSnapshot *reuse) {
    RamProvenanceSnapshot *snapshot = reuse;
    if (cpu_revision == 0u || cpu_speculative_depth != 0u || cpu_reset_pending ||
        (main_ram_writer_count != 0u && main_ram_writers == NULL) ||
        main_ram_writer_count > SIZE_MAX / sizeof(*main_ram_writers)) {
        ram_provenance_snapshot_free(snapshot);
        return NULL;
    }
    if (!snapshot) snapshot = (RamProvenanceSnapshot *)calloc(1u, sizeof(*snapshot));
    if (!snapshot) return NULL;
    const size_t bytes = main_ram_writer_count * sizeof(*main_ram_writers);
    if (snapshot->writer_count != main_ram_writer_count) {
        RamProvenanceEntry *writers = bytes ? (RamProvenanceEntry *)malloc(bytes) : NULL;
        if (bytes && !writers) {
            ram_provenance_snapshot_free(snapshot);
            return NULL;
        }
        free(snapshot->writers);
        snapshot->writers = writers;
    }
    snapshot->writer_count = main_ram_writer_count;
    snapshot->revision = cpu_revision;
    if (bytes) memcpy(snapshot->writers, main_ram_writers, bytes);
    return snapshot;
}

bool ram_provenance_snapshot_encode(const RamProvenanceSnapshot *snapshot,
                                    uint8_t *out, uint32_t capacity,
                                    uint32_t *out_size) {
    if (!snapshot || !out_size) return false;
    *out_size = 0u;
    return snapshot_write(snapshot, out, capacity, out_size);
}

bool ram_provenance_snapshot_decode(const uint8_t *data, uint32_t size,
                                    size_t expected_ram_size,
                                    RamProvenanceSnapshot **out_snapshot) {
    RamProvenanceSnapshot *snapshot = NULL;
    uint32_t magic, version, ram_size, word_count, record_count, record_bytes;
    uint64_t revision;
    uint64_t expected_size;
    uint32_t previous_index = 0u;
    PstR wire;

    if (out_snapshot == NULL) return false;
    *out_snapshot = NULL;
    if (data == NULL || size < RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES ||
        expected_ram_size > UINT32_MAX ||
        (expected_ram_size & (sizeof(uint32_t) - 1u)) != 0u)
        return false;
    pst_r_init(&wire, data, size);
    if (!pst_r_u32(&wire, &magic) || !pst_r_u32(&wire, &version) ||
        !pst_r_u32(&wire, &ram_size) || !pst_r_u32(&wire, &word_count) ||
        !pst_r_u32(&wire, &record_count) ||
        !pst_r_u32(&wire, &record_bytes) ||
        !pst_r_u64(&wire, &revision))
        return false;
    expected_size = (uint64_t)RAM_PROVENANCE_SNAPSHOT_HEADER_BYTES +
        (uint64_t)record_count * RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES;
    if (magic != RAM_PROVENANCE_SNAPSHOT_MAGIC ||
        version != RAM_PROVENANCE_SNAPSHOT_VERSION || revision == 0u ||
        ram_size != expected_ram_size ||
        word_count != expected_ram_size / sizeof(uint32_t) ||
        word_count != main_ram_writer_count || record_count > word_count ||
        record_bytes != RAM_PROVENANCE_SNAPSHOT_RECORD_BYTES ||
        expected_size != size)
        return false;

    snapshot = (RamProvenanceSnapshot *)calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) return false;
    snapshot->writers = word_count != 0u
        ? (RamProvenanceEntry *)calloc(word_count, sizeof(*snapshot->writers))
        : NULL;
    if (word_count != 0u && snapshot->writers == NULL) {
        free(snapshot);
        return false;
    }
    snapshot->writer_count = word_count;
    snapshot->revision = revision;

    for (uint32_t record = 0u; record < record_count; ++record) {
        RamProvenanceEntry entry = {0};
        uint32_t index;
        uint8_t reserved;

        if (!pst_r_u32(&wire, &index) || !pst_r_u32(&wire, &entry.pc) ||
            !pst_r_u32(&wire, &entry.return_address) ||
            !pst_r_u32(&wire, &entry.cpu_value) ||
            !pst_r_u64(&wire, &entry.cpu_revision) ||
            !pst_r_u64(&wire, &entry.source_receipt) ||
            !pst_r_u32(&wire, &entry.source_format) ||
            !pst_r_u8(&wire, &entry.cpu_valid_bytes) ||
            !pst_r_u8(&wire, &entry.source_valid_bytes) ||
            !pst_r_u8(&wire, &entry.source_kind) ||
            !pst_r_u8(&wire, &reserved) || reserved != 0u ||
            index >= word_count || (record != 0u && index <= previous_index) ||
            !entry_has_state(&entry) || !entry_is_valid(&entry, revision)) {
            ram_provenance_snapshot_free(snapshot);
            return false;
        }
        snapshot->writers[index] = entry;
        previous_index = index;
    }
    if (wire.p != wire.end) {
        ram_provenance_snapshot_free(snapshot);
        return false;
    }
    *out_snapshot = snapshot;
    return true;
}

void ram_provenance_snapshot_commit(RamProvenanceSnapshot *snapshot) {
    RamProvenanceEntry *old_writers;

    if (snapshot == NULL || snapshot->writer_count != main_ram_writer_count)
        return;
    old_writers = main_ram_writers;
    main_ram_writers = snapshot->writers;
    cpu_revision = snapshot->revision;
    cpu_speculative_depth = 0u;
    cpu_reset_pending = false;
    snapshot->writers = NULL;
    free(old_writers);
    free(snapshot);
}

void ram_provenance_snapshot_free(RamProvenanceSnapshot *snapshot) {
    if (snapshot == NULL) return;
    free(snapshot->writers);
    free(snapshot);
}

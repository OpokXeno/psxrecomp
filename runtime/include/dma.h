/* dma.h — PS1 DMA controller simulation (Phase 3).
 *
 * 7 DMA channels:
 *   Ch0: MDEC in       0x1F801080
 *   Ch1: MDEC out      0x1F801090
 *   Ch2: GPU           0x1F8010A0
 *   Ch3: CDROM         0x1F8010B0
 *   Ch4: SPU           0x1F8010C0
 *   Ch5: PIO           0x1F8010D0
 *   Ch6: OTC           0x1F8010E0
 *
 * Global:
 *   DPCR: 0x1F8010F0   (DMA control — enable bits per channel)
 *   DICR: 0x1F8010F4   (DMA interrupt control)
 */

#ifndef PSXRECOMP_DMA_H
#define PSXRECOMP_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     dma_init(void);
uint32_t dma_read(uint32_t addr);
void     dma_write(uint32_t addr, uint32_t val);
void     dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask);
void     dma_advance(uint32_t cycles);
/* Cycle-budgeted precise event slicing: guest CPU cycles until a DELIVERABLE
 * DMA IRQ (bit3 unmasked in i_mask). UINT32_MAX if none. */
uint32_t dma_cycles_to_irq(uint32_t i_mask);
/* Mask-blind scheduler boundary: next DMA word movement or delayed completion,
 * not merely the final IRQ. RAM-producing DMA must become visible at its
 * per-word due cycle even when guest code polls ordinary RAM instead of DICR. */
uint32_t dma_cycles_to_internal_event(void);
/* Strict deliverability form for wait-loop elision: unlike the conservative
 * scheduler bound, ignores channel completions whose DICR IRQ is disabled. */
uint32_t dma_cycles_to_deliverable_irq(uint32_t i_mask);
uint32_t dma_get_dicr(void);
uint32_t dma_get_dpcr(void);
int      dma_cdrom_transfer_active(void);

typedef struct DMAChannelDebugState {
    uint32_t madr;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t active;
    uint32_t remaining_words;
    uint32_t cycles_accum;
} DMAChannelDebugState;

typedef struct DMADebugState {
    uint32_t dpcr;
    uint32_t dicr;
    DMAChannelDebugState channels[7];
} DMADebugState;

typedef struct DMATraceEntry {
    uint64_t seq;
    uint32_t frame;
    uint32_t kind;
    uint32_t channel;
    uint32_t total_words;
    uint32_t addr;
    uint32_t val;
    uint32_t mask;
    uint32_t madr;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t dpcr;
    uint32_t dicr_before;
    uint32_t dicr_after;
    uint32_t i_stat_before;
    uint32_t i_stat_after;
    uint32_t func;
    uint32_t pc;
} DMATraceEntry;

#define DMA_TRACE_CAP (1 << 14)
#define DMA_CDROM_HISTORY_CAP (1 << 13)
#define DMA_CDROM_HISTORY_WORDS 16

/* Read-only ordering-table census. Each linked-list execution records the
 * exact head and every node before the node's GP0 words are submitted. The
 * GPU ring supplies the command payloads keyed by their source addresses. */
#define DMA_OT_TRACE_LIST_CAP (1u << 12)
#define DMA_OT_TRACE_NODE_CAP (1u << 17)

typedef enum DMAOtTraceMode {
    DMA_OT_TRACE_ORIGINAL = 0,
    DMA_OT_TRACE_JOURNAL = 1,
} DMAOtTraceMode;

typedef struct DMAOtTraceList {
    uint64_t seq;
    uint32_t frame;
    uint32_t start_addr;
    uint64_t node_start_seq;
    uint32_t node_count;
    uint32_t actual_words;
    uint32_t func;
    uint32_t pc;
    uint32_t ra;
    uint32_t mode;
    uint32_t status;
} DMAOtTraceList;

typedef struct DMAOtTraceNode {
    uint64_t seq;
    uint64_t list_seq;
    uint32_t frame;
    uint32_t node_addr;
    uint32_t next_node_addr;
    uint32_t packet_words;
    uint32_t final_ordinal;
} DMAOtTraceNode;

typedef struct DMACDROMHistoryEntry {
    uint64_t seq;
    uint32_t frame_start;
    uint32_t frame_end;
    uint32_t start_addr;
    uint32_t final_addr;
    uint32_t requested_words;
    uint32_t moved_words;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t dpcr;
    uint32_t dicr_start;
    uint32_t dicr_end;
    uint32_t i_stat_start;
    uint32_t i_stat_end;
    uint32_t func;
    uint32_t pc;
    int lba;
    int sector_size;
    int sector_read_pos_start;
    int sector_read_pos_end;
    uint8_t mode;
    uint8_t sector_available_start;
    uint8_t sector_available_end;
    uint8_t completed;
    uint8_t first_count;
    uint8_t last_count;
    uint32_t first_words[DMA_CDROM_HISTORY_WORDS];
    uint32_t last_words[DMA_CDROM_HISTORY_WORDS];
} DMACDROMHistoryEntry;

uint64_t dma_debug_get_trace(const DMATraceEntry** out_entries);
void dma_debug_clear_trace(void);
void dma_debug_get_state(DMADebugState* out);
uint64_t dma_debug_get_cdrom_history(const DMACDROMHistoryEntry** out_entries);
void dma_debug_clear_cdrom_history(void);
uint64_t dma_debug_get_ot_list_total(void);
uint64_t dma_debug_get_ot_node_total(void);
int dma_debug_get_ot_list(uint64_t seq, DMAOtTraceList *out);
int dma_debug_get_ot_node(uint64_t seq, DMAOtTraceNode *out);
void dma_debug_clear_ot_trace(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_DMA_H */

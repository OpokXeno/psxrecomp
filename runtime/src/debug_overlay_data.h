#ifndef PSX_DEBUG_OVERLAY_DATA_H
#define PSX_DEBUG_OVERLAY_DATA_H

/* Data tables for the developer debug overlay: fields, characters, events,
 * flags, verified addresses and the RAM watch map, all loaded from
 * <exe dir>/debug_overlay/data/ (staged next to the binary at build time
 * by the PSX_DEBUG_OVERLAY CMake gate). Only compiled in debug builds. */

#ifdef PSX_DEBUG_OVERLAY

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int id;
    const char *name;
} DbgField;

typedef struct {
    int id;
    const char *name;
    const char *alias;
} DbgCharacter;

typedef struct {
    int var;
    int value;
} DbgEventVarWrite;

typedef struct {
    int id;
    const char *name;
    int mapId;
    int entryPoint;
    bool verified;
    const DbgEventVarWrite *varWrites;
    int numVarWrites;
} DbgEvent;

typedef struct {
    int var;
    const char *name;
} DbgFlagVar;

typedef struct {
    const char *name;
    uint32_t value;
    const char *type;
    int size;
    bool verified;
} DbgAddr;

typedef struct {
    const char *name;
    uint32_t addr;
    const char *type;
    int size;
    const char *note;
    const char *region;
} DbgWatch;

/* Load every table from <data_dir> (usually "<base path>/debug_overlay/data").
 * Returns true when at least one table loaded. Missing files leave their
 * table empty; dbg_data_*_missing() tells the UI to show a placeholder. */
bool dbg_data_load_all(const char *data_dir);

bool dbg_data_fields_missing(void);
bool dbg_data_characters_missing(void);
bool dbg_data_events_missing(void);
bool dbg_data_flags_missing(void);
bool dbg_data_addrs_missing(void);
bool dbg_data_ram_map_missing(void);

const DbgField *dbg_data_fields(int *count);
const DbgCharacter *dbg_data_characters(int *count);
const DbgCharacter *dbg_data_gears(int *count);
const DbgEvent *dbg_data_events(int *count);
const DbgFlagVar *dbg_data_flag_vars(int *count);
const DbgAddr *dbg_data_addrs(int *count);
const DbgWatch *dbg_data_watches(int *count);

const char *dbg_data_field_name(int id);       /* NULL when unknown */
const char *dbg_data_character_name(int id);   /* NULL when unknown */
const DbgAddr *dbg_data_addr_by_name(const char *name); /* NULL when unknown */

#endif /* PSX_DEBUG_OVERLAY */
#endif /* PSX_DEBUG_OVERLAY_DATA_H */

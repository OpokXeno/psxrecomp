/* XML data loader for the developer debug overlay (see debug_overlay_data.h).
 * pugixml-backed; tables are parsed once at overlay init into static vectors. */

#ifdef PSX_DEBUG_OVERLAY

#include "debug_overlay_data.h"
#include "third_party/pugixml/pugixml.hpp"

#include <vector>
#include <deque>
#include <string>
#include <cstring>
#include <cstdio>

namespace {

/* Strings must outlive the tables, so every parsed attribute is owned here.
 * std::deque keeps element addresses stable across push_back (no realloc
 * invalidation, unlike std::vector). */
struct StringPool {
    std::deque<std::string> pool;
    const char *dup(const char *s) {
        pool.emplace_back(s ? s : "");
        return pool.back().c_str();
    }
};

struct Tables {
    StringPool strings;
    std::vector<DbgField> fields;
    std::vector<DbgCharacter> characters;
    std::vector<DbgCharacter> gears;
    std::vector<DbgEvent> events;
    std::vector<DbgEventVarWrite> eventVarWrites; /* backing store for events */
    std::vector<DbgFlagVar> flagVars;
    std::vector<DbgAddr> addrs;
    std::vector<DbgWatch> watches;
    bool fieldsMissing = true;
    bool charactersMissing = true;
    bool eventsMissing = true;
    bool flagsMissing = true;
    bool addrsMissing = true;
    bool ramMapMissing = true;
};

Tables g_t;
std::vector<int> g_eventVarWriteStart; /* start index per event in eventVarWrites */

bool load_doc(pugi::xml_document &doc, const std::string &path)
{
    pugi::xml_parse_result r = doc.load_file(path.c_str());
    return (bool)r;
}

void load_fields(Tables &t, const std::string &dir)
{
    pugi::xml_document doc;
    if (!load_doc(doc, dir + "/fields.xml")) return;
    pugi::xml_node root = doc.child("fields");
    if (!root) return;
    for (pugi::xml_node f : root.children("field")) {
        DbgField e;
        e.id = f.attribute("id").as_int(-1);
        if (e.id < 0) continue;
        e.name = t.strings.dup(f.attribute("name").value());
        t.fields.push_back(e);
    }
    t.fieldsMissing = t.fields.empty();
}

void load_characters(Tables &t, const std::string &dir)
{
    pugi::xml_document doc;
    if (!load_doc(doc, dir + "/characters.xml")) return;
    pugi::xml_node root = doc.child("data");
    if (!root) return;
    for (pugi::xml_node c : root.child("characters").children("character")) {
        DbgCharacter e;
        e.id = c.attribute("id").as_int(-1);
        if (e.id < 0) continue;
        e.name = t.strings.dup(c.attribute("name").value());
        e.alias = t.strings.dup(c.attribute("alias").value());
        t.characters.push_back(e);
    }
    for (pugi::xml_node g : root.child("gears").children("gear")) {
        DbgCharacter e;
        e.id = g.attribute("id").as_int(-1);
        if (e.id < 0) continue;
        const char *n = g.attribute("name").value();
        e.name = t.strings.dup(n);
        e.alias = e.name;
        t.gears.push_back(e);
    }
    t.charactersMissing = t.characters.empty() && t.gears.empty();
}

void load_events(Tables &t, const std::string &dir)
{
    pugi::xml_document doc;
    if (!load_doc(doc, dir + "/events.xml")) return;
    pugi::xml_node root = doc.child("events");
    if (!root) return;
    g_eventVarWriteStart.clear();
    for (pugi::xml_node ev : root.children("event")) {
        DbgEvent e;
        e.id = ev.attribute("id").as_int(-1);
        if (e.id < 0) continue;
        e.name = t.strings.dup(ev.attribute("name").value());
        e.mapId = ev.attribute("mapId").as_int(-1);
        e.entryPoint = ev.attribute("entryPoint").as_int(0);
        e.verified = std::strcmp(ev.attribute("status").value(), "verified") == 0;
        g_eventVarWriteStart.push_back((int)t.eventVarWrites.size());
        int n = 0;
        for (pugi::xml_node vw : ev.children("varWrite")) {
            DbgEventVarWrite w;
            w.var = vw.attribute("var").as_int(0);
            w.value = vw.attribute("value").as_int(0);
            t.eventVarWrites.push_back(w);
            n++;
        }
        e.numVarWrites = n;
        t.events.push_back(e);
    }
    /* Fix up pointers into the backing store (stable now that parsing is done). */
    for (size_t i = 0; i < t.events.size(); i++) {
        t.events[i].varWrites = t.eventVarWrites.data() + g_eventVarWriteStart[i];
    }
    t.eventsMissing = t.events.empty();
}

void load_flags(Tables &t, const std::string &dir)
{
    pugi::xml_document doc;
    if (!load_doc(doc, dir + "/flags.xml")) return;
    pugi::xml_node root = doc.child("flags");
    if (!root) return;
    for (pugi::xml_node fv : root.children("flagVar")) {
        DbgFlagVar e;
        e.var = fv.attribute("var").as_int(-1);
        if (e.var < 0) continue;
        e.name = t.strings.dup(fv.attribute("name").value());
        t.flagVars.push_back(e);
    }
    t.flagsMissing = t.flagVars.empty();
}

void load_addrs(Tables &t, const std::string &dir)
{
    pugi::xml_document doc;
    if (!load_doc(doc, dir + "/addrs.xml")) return;
    pugi::xml_node root = doc.child("addresses");
    if (!root) return;
    for (pugi::xml_node a : root.children("addr")) {
        DbgAddr e;
        e.name = t.strings.dup(a.attribute("name").value());
        e.value = (uint32_t)a.attribute("value").as_ullong(0);
        e.type = t.strings.dup(a.attribute("type").value());
        e.size = a.attribute("size").as_int(0);
        e.verified = std::strcmp(a.attribute("status").value(), "verified") == 0;
        t.addrs.push_back(e);
    }
    t.addrsMissing = t.addrs.empty();
}

void load_ram_map(Tables &t, const std::string &dir)
{
    pugi::xml_document doc;
    if (!load_doc(doc, dir + "/ram_map.xml")) return;
    pugi::xml_node root = doc.child("ramMap");
    if (!root) return;
    for (pugi::xml_node region : root.children("region")) {
        const char *rname = region.attribute("name").value();
        for (pugi::xml_node w : region.children("watch")) {
            DbgWatch e;
            e.name = t.strings.dup(w.attribute("name").value());
            e.addr = (uint32_t)w.attribute("addr").as_ullong(0);
            e.type = t.strings.dup(w.attribute("type").value());
            e.size = w.attribute("size").as_int(0);
            e.note = t.strings.dup(w.attribute("note").value());
            e.region = t.strings.dup(rname);
            t.watches.push_back(e);
        }
    }
    t.ramMapMissing = t.watches.empty();
}

} /* namespace */

bool dbg_data_load_all(const char *data_dir)
{
    g_t = Tables(); /* reset (idempotent reload) */
    std::string dir = data_dir ? data_dir : "debug_overlay/data";
    load_fields(g_t, dir);
    load_characters(g_t, dir);
    load_events(g_t, dir);
    load_flags(g_t, dir);
    load_addrs(g_t, dir);
    load_ram_map(g_t, dir);
    return !(g_t.fieldsMissing && g_t.charactersMissing && g_t.eventsMissing &&
             g_t.flagsMissing && g_t.addrsMissing && g_t.ramMapMissing);
}

bool dbg_data_fields_missing(void)     { return g_t.fieldsMissing; }
bool dbg_data_characters_missing(void) { return g_t.charactersMissing; }
bool dbg_data_events_missing(void)     { return g_t.eventsMissing; }
bool dbg_data_flags_missing(void)      { return g_t.flagsMissing; }
bool dbg_data_addrs_missing(void)      { return g_t.addrsMissing; }
bool dbg_data_ram_map_missing(void)    { return g_t.ramMapMissing; }

const DbgField *dbg_data_fields(int *count)           { *count = (int)g_t.fields.size(); return g_t.fields.data(); }
const DbgCharacter *dbg_data_characters(int *count)   { *count = (int)g_t.characters.size(); return g_t.characters.data(); }
const DbgCharacter *dbg_data_gears(int *count)        { *count = (int)g_t.gears.size(); return g_t.gears.data(); }
const DbgEvent *dbg_data_events(int *count)           { *count = (int)g_t.events.size(); return g_t.events.data(); }
const DbgFlagVar *dbg_data_flag_vars(int *count)      { *count = (int)g_t.flagVars.size(); return g_t.flagVars.data(); }
const DbgAddr *dbg_data_addrs(int *count)             { *count = (int)g_t.addrs.size(); return g_t.addrs.data(); }
const DbgWatch *dbg_data_watches(int *count)          { *count = (int)g_t.watches.size(); return g_t.watches.data(); }

const char *dbg_data_field_name(int id)
{
    for (const DbgField &f : g_t.fields)
        if (f.id == id) return f.name;
    return nullptr;
}

const char *dbg_data_character_name(int id)
{
    for (const DbgCharacter &c : g_t.characters)
        if (c.id == id) return c.name;
    return nullptr;
}

const DbgAddr *dbg_data_addr_by_name(const char *name)
{
    if (!name) return nullptr;
    for (const DbgAddr &a : g_t.addrs)
        if (std::strcmp(a.name, name) == 0) return &a;
    return nullptr;
}

#endif /* PSX_DEBUG_OVERLAY */

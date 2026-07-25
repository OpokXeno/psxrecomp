/* Standalone XML-loader harness ported from /tmp/xml-harness/harness.cpp.
 *
 * The original harness was a one-off proof that the debug_overlay_data.cpp
 * XML loader correctly parses every staged file (fields.xml, characters.xml,
 * events.xml, flags.xml, addrs.xml, ram_map.xml) and returns the expected
 * counts / spot-checks. This port puts the same assertions into the repo's
 * test suite so a future regression in the loader is caught at build time.
 *
 * Standalone, no SDL2 / no runtime / no ImGui. Compiles with
 *    g++ -std=c++17 -DPSX_DEBUG_OVERLAY=1 \
 *        -I psxrecomp/runtime/src \
 *        test_debug_overlay_data.cpp \
 *        psxrecomp/runtime/src/debug_overlay_data.cpp \
 *        psxrecomp/runtime/src/third_party/pugixml/pugixml.cpp \
 *        -o /tmp/test_debug_overlay_data
 * and is driven by test_debug_overlay_data.sh which runs the binary and
 * checks the exit code.
 *
 * Usage:  ./test_debug_overlay_data [data_dir]
 *   data_dir defaults to ../.. (the repo root, where debug_overlay/data
 *   lives — staged by runtime.cmake POST_BUILD). The test harness passes
 *   an explicit path so a different layout can be exercised.
 */

#define PSX_DEBUG_OVERLAY 1
#include "debug_overlay_data.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    const char *dir = (argc > 1 && argv[1] && *argv[1])
                        ? argv[1]
                        : "../..";  /* repo root, where debug_overlay/data lives */
    std::string path = std::string(dir) + "/debug_overlay/data";
    if (!dbg_data_load_all(path.c_str())) {
        std::printf("FAIL: no tables loaded from %s\n", path.c_str());
        return 1;
    }
    int nf = 0, nc = 0, ng = 0, ne = 0, nfv = 0, na = 0, nw = 0;
    const DbgField *f = dbg_data_fields(&nf);
    dbg_data_characters(&nc);
    dbg_data_gears(&ng);
    const DbgEvent *ev = dbg_data_events(&ne);
    dbg_data_flag_vars(&nfv);
    dbg_data_addrs(&na);
    dbg_data_watches(&nw);
    std::printf("fields=%d chars=%d gears=%d events=%d flagVars=%d addrs=%d watches=%d\n",
                nf, nc, ng, ne, nfv, na, nw);

    if (nf != 730) { std::printf("FAIL: fields count %d != 730\n", nf);  return 1; }
    if (nc != 11)  { std::printf("FAIL: chars count %d != 11\n", nc);    return 1; }

    const char *n1   = dbg_data_field_name(1);
    const char *n22  = dbg_data_field_name(22);
    const char *n490 = dbg_data_field_name(490);
    std::printf("field 1='%s' 22='%s' 490='%s'\n",
                n1 ? n1 : "?", n22 ? n22 : "?", n490 ? n490 : "?");
    if (!n1 || std::strstr(n1, "Lahan") == nullptr) {
        std::printf("FAIL: field 1 name does not contain 'Lahan'\n");
        return 1;
    }

    const char *c0 = dbg_data_character_name(0);
    std::printf("char 0='%s'\n", c0 ? c0 : "?");
    if (!c0 || std::strcmp(c0, "Fei") != 0) {
        std::printf("FAIL: char 0 != 'Fei'\n");
        return 1;
    }

    const DbgAddr *a = dbg_data_addr_by_name("fieldMapNumber");
    std::printf("addr fieldMapNumber=%s0x%X verified=%d\n",
                a ? "" : "(missing) ", a ? a->value : 0, a ? a->verified : 0);
    if (!a || a->value != 0x8004F34C || !a->verified) {
        std::printf("FAIL: addr fieldMapNumber lookup\n");
        return 1;
    }

    int ver = 0;
    for (int i = 0; i < ne; i++) if (ev[i].verified) ver++;
    std::printf("events verified=%d unverified=%d\n", ver, ne - ver);

    bool dup = false;
    for (int i = 0; i < nf; i++)
        for (int j = i + 1; j < nf; j++)
            if (f[i].id == f[j].id) dup = true;
    std::printf("dup field ids: %s\n", dup ? "YES (FAIL)" : "no");
    if (dup) {
        std::printf("FAIL: duplicate field ids\n");
        return 1;
    }

    std::printf("missing flags: fields=%d chars=%d events=%d flags=%d addrs=%d rammap=%d\n",
                dbg_data_fields_missing(), dbg_data_characters_missing(),
                dbg_data_events_missing(), dbg_data_flags_missing(),
                dbg_data_addrs_missing(), dbg_data_ram_map_missing());
    std::printf("HARNESS PASS\n");
    return 0;
}

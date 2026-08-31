/* Instruction counter for the benchmark suite.
 *
 * QEMU is not cycle-accurate and the microbit model's Cortex-M0 has no DWT
 * cycle counter, so there is nothing on the guest to read. What QEMU can
 * give exactly is *how many instructions executed*, and that is a fair
 * comparison on its own terms: the JIT-emitted Thumb and the C kernels run
 * on the same emulated core, so anything the model omits, it omits from
 * both sides equally.
 *
 * Regions are delimited by the addresses of ordinary no-inline marker
 * functions in the guest image (bench/bench_marks.h), whose addresses the
 * host driver reads out of the ELF with nm and passes here. Registering a
 * callback only at those two addresses means the mechanism costs nothing
 * anywhere else, and works identically for a compiled C kernel and for code
 * the JIT emitted at run time — the markers bracket the Executor::run call,
 * not anything inside the translated program.
 *
 * Build with plugin/build.sh. The header is vendored (QEMU stable-7.2, API
 * version 1) and the version is asserted below, so a QEMU whose plugin ABI
 * moved refuses to load this rather than reporting a wrong number.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu-plugin.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_REGIONS 32
#define NAME_MAX_LEN 48

struct Region
{
    char name[NAME_MAX_LEN];
    uint64_t enterAddr;
    uint64_t exitAddr;

    /* Instruction count at the last enter, and the accumulated total. A
     * region entered twice accumulates both excursions; `entries` is what
     * lets the driver turn that into a per-excursion figure. */
    uint64_t mark;
    uint64_t total;
    uint64_t entries;
    int open;
};

static struct Region g_regions[MAX_REGIONS];
static int g_regionCount;

/* The inline-incremented counter. Every executed instruction bumps it, with
 * no callback and no branch — this is the one thing that has to be cheap. */
static uint64_t g_insns;

static char g_outPath[512];

static struct Region *regionAt(uint64_t vaddr, int *isEnter)
{
    for(int i = 0; i < g_regionCount; i++)
    {
        if(g_regions[i].enterAddr == vaddr) { *isEnter = 1; return &g_regions[i]; }
        if(g_regions[i].exitAddr == vaddr) { *isEnter = 0; return &g_regions[i]; }
    }

    return NULL;
}

static void onEnter(unsigned int vcpu, void *udata)
{
    struct Region *r = udata;
    (void)vcpu;

    r->mark = g_insns;
    r->open = 1;
}

static void onExit(unsigned int vcpu, void *udata)
{
    struct Region *r = udata;
    (void)vcpu;

    if(!r->open) return; /* an exit with no matching enter measures nothing */

    r->total += g_insns - r->mark;
    r->entries++;
    r->open = 0;
}

static void onBlockTranslate(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    const size_t n = qemu_plugin_tb_n_insns(tb);
    (void)id;

    for(size_t i = 0; i < n; i++)
    {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &g_insns, 1);

        int isEnter = 0;
        struct Region *r = regionAt(qemu_plugin_insn_vaddr(insn), &isEnter);

        if(r != NULL)
        {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, isEnter ? onEnter : onExit,
                QEMU_PLUGIN_CB_NO_REGS, r);
        }
    }
}

static void onExitPlugin(qemu_plugin_id_t id, void *p)
{
    (void)id;
    (void)p;

    FILE *out = g_outPath[0] != '\0' ? fopen(g_outPath, "w") : NULL;
    char line[256];

    snprintf(line, sizeof(line), "TOTAL_INSNS %" PRIu64 "\n", g_insns);
    if(out) fputs(line, out); else qemu_plugin_outs(line);

    for(int i = 0; i < g_regionCount; i++)
    {
        const struct Region *r = &g_regions[i];

        snprintf(line, sizeof(line), "REGION %.47s insns=%" PRIu64 " entries=%" PRIu64 "%s\n",
            r->name, r->total, r->entries, r->open ? " UNCLOSED" : "");

        if(out) fputs(line, out); else qemu_plugin_outs(line);
    }

    if(out) fclose(out);
}

/* `region=<name>:<enterHexAddr>:<exitHexAddr>`, repeatable, plus an
 * optional `out=<path>`. Addresses come from nm, so they carry the Thumb
 * bit; it is cleared here, because that bit is a branch-target convention
 * and never part of an instruction's own address. */
static int parseRegion(const char *value)
{
    if(g_regionCount >= MAX_REGIONS) return -1;

    const char *firstColon = strchr(value, ':');
    if(firstColon == NULL) return -1;

    const char *secondColon = strchr(firstColon + 1, ':');
    if(secondColon == NULL) return -1;

    const size_t nameLen = (size_t)(firstColon - value);
    if(nameLen == 0 || nameLen >= NAME_MAX_LEN) return -1;

    struct Region *r = &g_regions[g_regionCount];
    memcpy(r->name, value, nameLen);
    r->name[nameLen] = '\0';

    r->enterAddr = strtoull(firstColon + 1, NULL, 16) & ~1ull;
    r->exitAddr = strtoull(secondColon + 1, NULL, 16) & ~1ull;

    if(r->enterAddr == 0 || r->exitAddr == 0) return -1;

    g_regionCount++;
    return 0;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
    int argc, char **argv)
{
    (void)info;

    for(int i = 0; i < argc; i++)
    {
        if(strncmp(argv[i], "region=", 7) == 0)
        {
            if(parseRegion(argv[i] + 7) != 0)
            {
                fprintf(stderr, "bench_plugin: bad region spec '%s'\n", argv[i]);
                return -1;
            }
        }
        else if(strncmp(argv[i], "out=", 4) == 0)
        {
            snprintf(g_outPath, sizeof(g_outPath), "%s", argv[i] + 4);
        }
        else
        {
            fprintf(stderr, "bench_plugin: unknown argument '%s'\n", argv[i]);
            return -1;
        }
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, onBlockTranslate);
    qemu_plugin_register_atexit_cb(id, onExitPlugin, NULL);

    return 0;
}

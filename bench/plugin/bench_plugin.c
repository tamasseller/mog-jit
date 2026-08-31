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

    /* Counts at the last enter, and the accumulated totals. A region
     * entered twice accumulates both excursions; `entries` is what lets the
     * driver turn that into a per-excursion figure. */
    uint64_t mark;
    uint64_t total;
    uint64_t cycleMark;
    uint64_t cycleTotal;
    uint64_t entries;
    int open;
};

static struct Region g_regions[MAX_REGIONS];
static int g_regionCount;

/* The inline-incremented counter. Every executed instruction bumps it, with
 * no callback and no branch — this is the one thing that has to be cheap. */
static uint64_t g_insns;

/* The weighted counter, when a cycle model is asked for. Each instruction
 * carries its own static cost; the taken-branch penalty is added per block
 * transfer, since taken-ness is not a static property. */
static uint64_t g_cycles;
static int g_cycleModel;

/* Cortex-M0 permits both a 1-cycle and a 32-cycle multiplier and the choice
 * is implementation defined, so it is a knob rather than a constant. It
 * matters for nothing measured so far — no workload here has reached a MUL
 * — but a cycle figure that quietly assumed one would be wrong by 31 cycles
 * an instruction the moment one did. */
static uint64_t g_mulCycles = 1;

/* Per-block: where the block would fall through to, and whether its last
 * instruction is a plain branch. Together these turn "the next block did not
 * start where this one ended" into the taken-branch pipeline refill, without
 * any of it having to be inferred statically. */
struct BlockExit
{
    uint64_t startAddr;
    uint64_t fallThrough;
    int lastIsSimpleBranch;
};

static uint64_t g_prevFallThrough;
static int g_prevWasSimpleBranch;

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

/* Cortex-M0 instruction timings, ARM DDI 0432C table 3-1.
 *
 * A Cortex-M0 has no cache, no branch prediction and no store buffer, and
 * runs from zero-wait-state memory on this part, so its timing really is a
 * static table plus one dynamic term — the pipeline refill on a taken
 * branch. That is the whole reason a model is worth building here and would
 * not be on a bigger core.
 *
 * `size` distinguishes the one 32-bit encoding that matters, BL. Anything
 * unrecognised is 1, which is right for the whole ALU/move/shift bulk.
 */
static uint64_t thumbCycles(const uint8_t *bytes, size_t size, int *isSimpleBranch)
{
    *isSimpleBranch = 0;

    if(size != 2) return 4; /* BL, the only 32-bit encoding ARMv6-M has */

    const uint16_t h = (uint16_t)(bytes[0] | (bytes[1] << 8));

    /* Load/store: register offset, immediate, SP-relative, PC-literal. */
    if((h >> 12) == 0x5) return 2;
    if((h >> 13) == 0x3) return 2;
    if((h >> 12) == 0x8) return 2;
    if((h >> 12) == 0x9) return 2;
    if((h >> 11) == 0x9) return 2; /* LDR Rt, [PC, #imm] */

    /* PUSH/POP/LDM/STM: 1 + one per register, and POP{...,PC} pays the
     * refill on top because it is a branch. */
    if((h >> 9) == 0x5a || (h >> 9) == 0x5e) /* PUSH / POP */
    {
        uint64_t n = (h >> 8) & 1; /* LR for PUSH, PC for POP */
        for(int i = 0; i < 8; i++) n += (h >> i) & 1;

        const int popsPc = ((h >> 9) == 0x5e) && ((h >> 8) & 1);
        return 1 + n + (popsPc ? 2 : 0);
    }

    if((h >> 12) == 0xc) /* LDM / STM */
    {
        uint64_t n = 0;
        for(int i = 0; i < 8; i++) n += (h >> i) & 1;
        return 1 + n;
    }

    if((h >> 9) == 0x21 && ((h >> 6) & 0xf) == 0xd) return g_mulCycles; /* MULS */

    /* BX / BLX: always a transfer, refill already in the 3. */
    if((h >> 7) == 0x8e || (h >> 7) == 0x8f) return 3;

    /* B<cond> — SVC (0xdf) and UDF (0xde) share the top nibble — and
     * unconditional B. Costed at 1 here; the +2 refill is added by the
     * block-transfer callback only when the branch was actually taken. */
    if((h >> 12) == 0xd && ((h >> 8) & 0xf) < 0xe) { *isSimpleBranch = 1; return 1; }
    if((h >> 11) == 0x1c) { *isSimpleBranch = 1; return 1; }

    return 1;
}

/* Fires on entry to every translated block. If the previous block did not
 * fall through into this one, control transferred — and if that previous
 * block ended in a plain branch, the transfer cost a pipeline refill the
 * static table deliberately left out.
 *
 * A conditional branch that was not taken lands the next block exactly at
 * the fall-through address and so costs nothing extra, which is what makes
 * this exact rather than a heuristic: taken-ness is read off where control
 * actually went, never guessed. BX/BLX/BL/POP{PC} carry their refill in
 * their own static cost, so they are not marked as simple branches and are
 * not charged twice. */
static void onBlockEnter(unsigned int vcpu, void *udata)
{
    const struct BlockExit *block = udata;
    (void)vcpu;

    if(g_prevWasSimpleBranch && g_prevFallThrough != block->startAddr)
    {
        g_cycles += 2;
    }

    g_prevFallThrough = block->fallThrough;
    g_prevWasSimpleBranch = block->lastIsSimpleBranch;
}

static void onEnter(unsigned int vcpu, void *udata)
{
    struct Region *r = udata;
    (void)vcpu;

    r->mark = g_insns;
    r->cycleMark = g_cycles;
    r->open = 1;
}

static void onExit(unsigned int vcpu, void *udata)
{
    struct Region *r = udata;
    (void)vcpu;

    if(!r->open) return; /* an exit with no matching enter measures nothing */

    r->total += g_insns - r->mark;
    r->cycleTotal += g_cycles - r->cycleMark;
    r->entries++;
    r->open = 0;
}

static void onBlockTranslate(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    const size_t n = qemu_plugin_tb_n_insns(tb);
    (void)id;

    uint64_t fallThrough = qemu_plugin_tb_vaddr(tb);
    int lastIsSimpleBranch = 0;

    for(size_t i = 0; i < n; i++)
    {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        const size_t size = qemu_plugin_insn_size(insn);

        qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &g_insns, 1);

        if(g_cycleModel)
        {
            const uint64_t cost = thumbCycles(qemu_plugin_insn_data(insn), size,
                &lastIsSimpleBranch);

            qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64,
                &g_cycles, cost);
        }

        fallThrough = qemu_plugin_insn_vaddr(insn) + size;

        int isEnter = 0;
        struct Region *r = regionAt(qemu_plugin_insn_vaddr(insn), &isEnter);

        if(r != NULL)
        {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, isEnter ? onEnter : onExit,
                QEMU_PLUGIN_CB_NO_REGS, r);
        }
    }

    if(g_cycleModel)
    {
        /* Leaked deliberately: it has to outlive this call for as long as
         * the block can be executed, which is the plugin's whole lifetime,
         * and QEMU offers no block-flush hook to free it on. */
        struct BlockExit *block = malloc(sizeof(*block));
        if(block == NULL) return;

        block->startAddr = qemu_plugin_tb_vaddr(tb);
        block->fallThrough = fallThrough;
        block->lastIsSimpleBranch = lastIsSimpleBranch;

        qemu_plugin_register_vcpu_tb_exec_cb(tb, onBlockEnter, QEMU_PLUGIN_CB_NO_REGS, block);
    }
}

static void onExitPlugin(qemu_plugin_id_t id, void *p)
{
    (void)id;
    (void)p;

    FILE *out = g_outPath[0] != '\0' ? fopen(g_outPath, "w") : NULL;
    char line[256];

    snprintf(line, sizeof(line), "TOTAL_INSNS %" PRIu64 " TOTAL_CYCLES %" PRIu64
        " MUL_CYCLES %" PRIu64 "\n", g_insns, g_cycles, g_mulCycles);
    if(out) fputs(line, out); else qemu_plugin_outs(line);

    for(int i = 0; i < g_regionCount; i++)
    {
        const struct Region *r = &g_regions[i];

        snprintf(line, sizeof(line),
            "REGION %.47s insns=%" PRIu64 " cycles=%" PRIu64 " entries=%" PRIu64 "%s\n",
            r->name, r->total, r->cycleTotal, r->entries, r->open ? " UNCLOSED" : "");

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
        else if(strcmp(argv[i], "cycles=on") == 0)
        {
            g_cycleModel = 1;
        }
        else if(strncmp(argv[i], "mul=", 4) == 0)
        {
            g_mulCycles = strtoull(argv[i] + 4, NULL, 10);
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

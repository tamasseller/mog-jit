// jit-armv6m/fuzz — the compiler-side half of the fuzzing setup: gate every
// candidate through the validator (oracle_server.ts, over a Unix domain
// socket) and, for anything it approves, run it through the real
// translateProc() pipeline under ASan/UBSan. A crash here is real
// "runtime+compiler stability" bug by construction, since the input was
// already validator-approved -- the JIT is never supposed to see anything
// else in production.
//
// v1 scope: catches compiler/JIT crashes on validator-approved input, and
// treats Assembler::fail()'s resource-bail path as the OTHER acceptable
// outcome (matching "runs correctly, or bails with a proper diagnostic").
// It does NOT yet execute the emitted Thumb code to compare against the
// oracle's reference-VM result (refVmAcc/refVmOk/refVmTrapCode below) --
// that needs an ARM execution oracle (Unicorn Engine is the natural
// choice; not installed in this environment). The plumbing to fetch that
// reference result is already wired up; only the "execute buf[] and
// compare" step is missing. Search for "TODO(execute)" below.
//
// Entry point is the standard `LLVMFuzzerTestOneInput(data, size)` shape
// so this drops straight into libFuzzer or AFL++'s persistent mode once
// either is installed (neither is available in this environment: no
// clang, no afl-*). Until then, main() below is a small, dumb
// mutation-based driver against the exact same function -- no coverage
// guidance, just enough to start finding bugs today.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csetjmp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "translate_proc.h"
#include "decode_instr.h"
#include "runtime.h"

using namespace jitc;

// ── host-only resource-bail escape (mirrors test/host/host_runtime_support.cpp,
//    minus the 1test MOCK bookkeeping this harness has no use for) ─────────
static jmp_buf g_resourceEscape;

extern "C" [[noreturn]] void runtimeBail(Runtime *, uint32_t)
{
    longjmp(g_resourceEscape, 1);
}

extern const uint32_t trampolineAddr = 0xDEADBEEFu;

// ── oracle client ───────────────────────────────────────────────────────

struct OracleResponse
{
    uint8_t valid;
    uint8_t stage;
    uint8_t refVmRan;
    uint8_t refVmOk;
    int32_t refVmAcc;
    int32_t refVmTrapCode;
    uint32_t refVmSteps;
};

static const char *g_sockPath = "/tmp/ppl-jit-oracle.sock";

// One connection for the whole run, not one per test case: the oracle
// server's own framing is already a length-prefixed request/response loop
// over a single socket, and connect()/close() per execution dominated the
// harness's throughput (~170 exec/s with, several thousand without).
// Reset to -1 on any I/O error so the next call redials -- an oracle
// restart mid-campaign then costs one lost execution, not the run.
static int g_oracleFd = -1;

static bool oracleConnect()
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sockPath, sizeof(addr.sun_path) - 1);

    if(connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return false;
    }
    g_oracleFd = fd;
    return true;
}

static void oracleDrop()
{
    if(g_oracleFd >= 0) close(g_oracleFd);
    g_oracleFd = -1;
}

static bool queryOracle(const uint8_t *data, size_t size, OracleResponse &out)
{
    if(g_oracleFd < 0 && !oracleConnect()) return false;
    const int fd = g_oracleFd;

    uint32_t len = (uint32_t)size;
    if(write(fd, &len, 4) != 4)
    {
        oracleDrop();
        return false;
    }
    size_t sent = 0;
    while(sent < size)
    {
        ssize_t n = write(fd, data + sent, size - sent);
        if(n <= 0) { oracleDrop(); return false; }
        sent += (size_t)n;
    }

    uint8_t resp[16];
    size_t got = 0;
    while(got < sizeof(resp))
    {
        ssize_t n = read(fd, resp + got, sizeof(resp) - got);
        if(n <= 0) { oracleDrop(); return false; }
        got += (size_t)n;
    }

    out.valid = resp[0];
    out.stage = resp[1];
    out.refVmRan = resp[2];
    out.refVmOk = resp[3];
    memcpy(&out.refVmAcc, resp + 4, 4);
    memcpy(&out.refVmTrapCode, resp + 8, 4);
    memcpy(&out.refVmSteps, resp + 12, 4);
    return true;
}

// ── the code arena ───────────────────────────────────────────────────
//
// Ordinary static storage, not a hand-placed low mapping: this driver is
// built -m32 (build.sh), so every address already fits the bare uint32_t
// the Assembler and every Runtime arena method address the arena through.
// Being ASan-instrumented storage is what a fixed mmap wouldn't give --
// an emitted halfword landing past arenaEnd is caught here rather than
// scribbling on a neighbouring page.
//
// Sized for pass 1, where the point is translating a procedure under no
// capacity pressure at all. Pass 2 asks for a far smaller slice of it,
// because that pass's point is the opposite: eviction, compaction, and
// the literal pool running out of reach.
static constexpr uint32_t ARENA_CAPACITY = 65536u;
static constexpr uint32_t PRESSURE_ARENA_CAP = 8192u;

alignas(8) static uint8_t g_arena[ARENA_CAPACITY];

static uint32_t arenaBase()
{
    return (uint32_t)(uintptr_t)g_arena;
}

// The arena size this input gets. Scaled to the program rather than a
// fixed constant, and drawn from the input's own bytes rather than an
// execution counter so a crash stays reproducible from the saved file
// alone.
//
// Absolute sizes don't work here: eviction and compaction only ever run
// when an in-progress translation exhausts an arena that some *other*
// procedure is already resident in, which is a narrow band around the
// program's own compiled size -- measured with probe_arena.cpp, a 3-
// procedure seed evicts between roughly 48 and 96 bytes of arena and never
// again above that. A constant tuned for one program size leaves every
// other size either bailing immediately or never under pressure at all.
//
// The estimate only has to be the right order of magnitude: blocks.h
// prices an ordinary instruction's worst case at 16 bytes and a call
// sequence at 64, so 8 bytes of code per byte of bytecode is a reasonable
// middle. The four multipliers then straddle it -- a quarter (constant
// eviction, frequent RESOURCE_ERROR), a half and 1x (real compaction), 4x
// (roomy, so the no-pressure path is covered too).
static uint32_t arenaSizeFor(const uint8_t *data, size_t size, uint32_t bodyOffset)
{
    uint32_t h = 2166136261u;
    for(size_t i = 0; i < size; i++) { h ^= data[i]; h *= 16777619u; }

    const uint32_t estimate = 8u * (uint32_t)(size - bodyOffset);
    static const uint32_t quarters[4] = {1, 2, 4, 16};
    uint32_t chosen = estimate / 4u * quarters[h & 3u];

    if(chosen < 32u) chosen = 32u;
    if(chosen > PRESSURE_ARENA_CAP) chosen = PRESSURE_ARENA_CAP;
    return chosen & ~3u;
}

// oracle_server.ts's own REALISTIC_MAX_PROC_COUNT — the storage buffer in
// LLVMFuzzerTestOneInput is sized off it.
static constexpr uint32_t ORACLE_MAX_PROC_COUNT = 16;

static bool g_lastWasValid = false;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    g_lastWasValid = false;

    if(size == 0 || size > 4096) return 0;

    OracleResponse oracle{};
    if(!queryOracle(data, size, oracle))
    {
        fprintf(stderr, "fuzz: oracle unreachable -- start oracle_server.ts first\n");
        _exit(2);
    }

    if(oracle.stage == 4)
    {
        fprintf(stderr, "fuzz: ORACLE STAGE 4 -- reference VM threw on a validator-approved program\n");
        abort();
    }

    if(!oracle.valid) return 0; // not validator-approved: out of scope by design

    g_lastWasValid = true;

    uint32_t pos = 0;
    const uint32_t maxCallDepth = jitc::decodeLeb128(data, pos, pos);
    const uint32_t totalDepth = jitc::decodeLeb128(data, pos, pos);
    const uint32_t procCount = jitc::decodeLeb128(data, pos, pos);
    const uint32_t bodyOffset = pos;
    (void)maxCallDepth;
    (void)totalDepth;

    if(procCount == 0 || procCount > ORACLE_MAX_PROC_COUNT) return 0;
    static_assert(sizeof(Runtime) + (ORACLE_MAX_PROC_COUNT + 1) * sizeof(ProcSlot) <= 512,
        "grow this buffer if Runtime/ProcSlot grow, or if the oracle's procCount cap rises");
    alignas(8) uint8_t storage[512] = {};
    CodeArena arena = CodeArena::region(arenaBase(), ARENA_CAPACITY, /*stackLimit=*/0);
    Runtime &rt = *new(storage) Runtime(procCount, arena);
    if(rt.loadProgram(data, (uint32_t)size, bodyOffset) != 0)
    {
        return 0; // the JIT's own static ceiling rejected it -- graceful, not a bug
    }

    // ── pass 1: one procedure at a time, under no arena pressure ───────
    //
    // Every procedure, not just the entry one: a CALL site's own
    // translation reads the *callee's* slot (argCount, for the argument
    // shuffle and the dispatch-table offset), so the interesting
    // interaction is between procedures, and the callee is only ever
    // reached by translating it in its own right. This is also what the
    // real runtime does over an execution's lifetime, one dispatch at a
    // time.
    //
    // Re-initialised per procedure because there is no detached Assembler
    // any more -- every translation emits into a real Runtime's arena, and
    // a bail leaves that arena's bookkeeping mid-update. Throwing the
    // whole Runtime away between procedures is what keeps one procedure's
    // bail from hiding every procedure after it.
    memset(g_arena, 0, ARENA_CAPACITY);

    for(uint32_t i = 0; i < procCount; i++)
    {
        arena = CodeArena::region(arenaBase(), ARENA_CAPACITY, /*stackLimit=*/0);
        new(storage) Runtime(procCount, arena);
        rt.loadProgram(data, (uint32_t)size, bodyOffset); // already known to succeed

        if(setjmp(g_resourceEscape) == 0)
        {
            translateProc(i, rt, /*lruTick=*/1);
            // TODO(execute): feed the emitted halfwords at
            // rt.slot(i).codePtr through an ARM execution oracle (Unicorn
            // Engine) with rt.slot(i)'s needsLRSave() ABI, and compare the
            // result against oracle.refVmAcc/refVmOk/refVmTrapCode when
            // oracle.refVmRan -- that closes the loop this harness is
            // named for. Not wired up: Unicorn isn't installed here.
        }
        // else: Assembler::fail() -> runtimeBail() -> here. The JIT bailed
        // with RESOURCE_ERROR instead of crashing -- the other acceptable
        // outcome for a validator-approved program, not a finding.
    }

    // ── pass 2: the same procedures against a deliberately tight arena ──
    //
    // Pass 1 hands every translation more room than it can use, which
    // leaves the whole runtime half of the arena untouched:
    // findEvictionVictim, evict's compaction memmove and codePtr slides,
    // and the literal pool under genuine capacity pressure. This pass
    // drives exactly that, as close to what the real dispatch path does as
    // a host build can get: compile a slot only when it is cold, one
    // procedure at a time, with the LRU tick advancing so
    // findEvictionVictim's age comparison means something.
    //
    // Several rounds, not one: eviction only ever happens once the arena
    // is already full, so round 1 populates and later rounds are where a
    // procedure evicted out from under an earlier round gets recompiled
    // on top of a compacted arena.
    const uint32_t arenaSize = arenaSizeFor(data, size, bodyOffset);
    memset(g_arena, 0, arenaSize);

    arena = CodeArena::region(arenaBase(), arenaSize, /*stackLimit=*/0);
    new(storage) Runtime(procCount, arena);
    if(rt.loadProgram(data, (uint32_t)size, bodyOffset) != 0)
    {
        return 0;
    }

    // One escape for the whole pass, not one per procedure: a bail leaves
    // Runtime's arena bookkeeping mid-update, and production treats that
    // as the end of the whole excursion (RESOURCE_ERROR out of
    // enterProgram), never as something to continue from.
    if(setjmp(g_resourceEscape) == 0)
    {
        uint32_t lruTick = 1;
        for(uint32_t round = 0; round < 4; round++)
        {
            for(uint32_t i = 0; i < procCount; i++)
            {
                if(rt.isResident(i)) continue; // a dispatch only ever lands on a cold slot

                translateProc(i, rt, lruTick++);
            }
        }
    }

    return 0;
}

// AFL++'s own persistent-mode entry point (afl-cc/afl-gcc-fast define
// __AFL_COMPILER; a plain g++/clang build never sees this block at all).
// Shared-memory testcase delivery (__AFL_FUZZ_TESTCASE_BUF), no fork per
// run -- this is AFL++'s own documented from-scratch harness shape
// (utils/persistent_mode/persistent_demo.c in the afl++ source tree), not
// something specific to this project. last_input.bin's own repro-saving
// is skipped here: AFL++ already saves every crashing input to its own
// output directory (out/default/crashes/), and writing a file every
// iteration would tank exactly the throughput persistent mode exists for.
#ifdef __AFL_COMPILER
#include <unistd.h>
__AFL_FUZZ_INIT();

int main(void)
{
#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
#endif
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while(__AFL_LOOP(10000))
    {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        if(len < 1) continue;
        LLVMFuzzerTestOneInput(buf, (size_t)len);
    }
    return 0;
}

// No libFuzzer/AFL++ in this environment (no clang, no afl-*) -- this is a
// tiny, dumb mutation loop against the exact same entry point above, so
// swapping in real coverage-guided fuzzing later is a rebuild against
// -fsanitize=fuzzer or afl-clang-fast, not a rewrite of anything here.
#elif !defined(PPL_FUZZ_LIBFUZZER_BUILD)
#include <cstdlib>
#include <ctime>
#include <vector>
#include <string>
#include <fstream>
#include <dirent.h>

// Upper bound on the feedback corpus below; random replacement past it.
static constexpr size_t CORPUS_MAX = 4096;

// Largest input the mutation loop will build. Matched to
// LLVMFuzzerTestOneInput's 4096-byte ceiling: the seed corpus includes
// programs deliberately sized past the translator's compiled-size guards (a
// 2.4KB LOOP body whose back-edge can't encode, a 1.2KB run of literal-pool
// constants), and a smaller cap silently drops every insert and splice
// touching one of those -- so they could only ever shrink, never be
// explored around.
static constexpr size_t INPUT_MAX = 4096;

static std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv)
{
    std::vector<std::vector<uint8_t>> corpus;
    if(argc > 1)
    {
        DIR *d = opendir(argv[1]);
        if(d)
        {
            for(dirent *e; (e = readdir(d)) != nullptr;)
            {
                if(e->d_name[0] == '.') continue;
                corpus.push_back(readFile(std::string(argv[1]) + "/" + e->d_name));
            }
            closedir(d);
        }
    }
    if(argc > 2) g_sockPath = argv[2];
    if(corpus.empty()) corpus.push_back({0x00, 0x6b, 0x25, 0x64}); // argCount=0, CONST(37);RETURN

    // time() alone gave every worker of a parallel campaign the same seed:
    // N workers started in the same second explored one identical sequence
    // and "found" one identical crash N times over. XOR in the pid.
    srand((unsigned)time(nullptr) ^ ((unsigned)getpid() * 2654435761u));

    // Every distinct byte the seed corpus uses — effectively the ISA's own
    // opcode/immediate alphabet, without restating isa-core.md's table
    // here. A uniformly random byte lands on a decodable opcode well under
    // half the time; drawing from this instead keeps far more mutants
    // decodable, and it costs one pass over the seeds.
    std::vector<uint8_t> alphabet;
    {
        bool seen[256] = {};
        for(const auto &c : corpus) for(uint8_t b : c) seen[b] = true;
        for(int b = 0; b < 256; b++) if(seen[b]) alphabet.push_back((uint8_t)b);
    }

    // Where to export validator-approved programs for the execution oracle
    // (qemu_exec/). Off unless set, since a long campaign writing every
    // approved input to disk is a lot of small files.
    const char *corpusOut = getenv("PPL_FUZZ_CORPUS_OUT");
    if(corpusOut != nullptr) fprintf(stderr, "fuzz: exporting approved programs to %s\n", corpusOut);

    uint64_t execs = 0, approved = 0;
    for(;;)
    {
        std::vector<uint8_t> input = corpus[(size_t)rand() % corpus.size()];
        int mutations = 1 + rand() % 8;
        for(int i = 0; i < mutations && !input.empty(); i++)
        {
            size_t pos = (size_t)rand() % input.size();
            switch(rand() % 5)
            {
                case 0: input[pos] = (uint8_t)rand(); break;
                case 1: if(input.size() < INPUT_MAX) input.insert(input.begin() + (long)pos, (uint8_t)rand()); break;
                case 2: if(input.size() > 1) input.erase(input.begin() + (long)pos); break;
                case 3: input[pos] = alphabet[(size_t)rand() % alphabet.size()]; break;
                default:
                {
                    // Splice: graft a run out of another corpus entry. The
                    // one mutation that can reach a structure bigger than
                    // any single entry's (a deeper nest, a longer case
                    // list) without rediscovering its bytes one flip at a
                    // time — and, with the corpus feedback below, the one
                    // that compounds.
                    const auto &donor = corpus[(size_t)rand() % corpus.size()];
                    if(donor.empty() || input.size() >= INPUT_MAX) break;
                    size_t from = (size_t)rand() % donor.size();
                    size_t len = 1 + (size_t)rand() % (donor.size() - from);
                    if(input.size() + len > INPUT_MAX) len = INPUT_MAX - input.size();
                    input.insert(input.begin() + (long)pos, donor.begin() + (long)from, donor.begin() + (long)(from + len));
                    break;
                }
            }
        }
        // Written *before* running it: if this input crashes, the file left
        // behind on disk is the repro, not whatever ran after it.
        {
            std::ofstream f("last_input.bin", std::ios::binary | std::ios::trunc);
            f.write((const char *)input.data(), (std::streamsize)input.size());
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());

        // Corpus feedback. With no coverage instrumentation, validator
        // approval is the only signal available — and it's the one that
        // matters most here, since a blind mutant is almost always
        // rejected by decodeBody/validateProgram and never reaches
        // translateProc at all. Keeping the approved ones turns the loop
        // from "mutate five fixed seeds forever" into a search that
        // actually accumulates structure. Bounded with random
        // replacement, so a long run neither grows without limit nor
        // freezes its corpus.
        if(g_lastWasValid)
        {
            approved++;
            if(corpus.size() < CORPUS_MAX) corpus.push_back(input);
            else corpus[(size_t)rand() % corpus.size()] = input;

            // Export for qemu_exec/, the execution oracle. This loop can
            // only ever find crashes and clean bails; whether the emitted
            // Thumb computes the *right answer* takes actually running it,
            // which happens out of process on qemu-system-arm against a
            // corpus of programs like these. Named by content hash, so
            // re-exporting the same program is idempotent and parallel
            // workers writing to one directory don't collide.
            if(corpusOut != nullptr)
            {
                uint32_t h = 2166136261u;
                for(uint8_t b : input) { h ^= b; h *= 16777619u; }
                char path[512];
                snprintf(path, sizeof(path), "%s/%08x_%zu.bin", corpusOut, h, input.size());
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write((const char *)input.data(), (std::streamsize)input.size());
            }
        }

        if(++execs % 20000 == 0)
            fprintf(stderr, "fuzz: %llu executions, %llu validator-approved, corpus %zu\n",
                (unsigned long long)execs, (unsigned long long)approved, corpus.size());
    }
}
#endif

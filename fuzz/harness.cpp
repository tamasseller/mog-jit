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
#include <csetjmp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "proc.h"
#include "assembler.h"
#include "translate_proc.h"
#include "decode_instr.h"
#include "runtime_internal.h"

using namespace jitc;

// ── host-only resource-bail escape (mirrors test/host/host_runtime_support.cpp,
//    minus the 1test MOCK bookkeeping this harness has no use for) ─────────
static jmp_buf g_resourceEscape;

extern "C" [[noreturn]] void runtimeBail(Runtime *, uint32_t)
{
    longjmp(g_resourceEscape, 1);
}

// Runtime::init()/setCodePtr() reference this (real dispatch-trampoline
// address in production); this harness never dispatches through it, only
// reads/writes ProcSlot::codePtr, so any distinct sentinel value works —
// same stand-in test_runtime_arena.cpp already uses for the same reason.
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

static bool queryOracle(const uint8_t *data, size_t size, OracleResponse &out)
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

    uint32_t len = (uint32_t)size;
    if(write(fd, &len, 4) != 4)
    {
        close(fd);
        return false;
    }
    size_t sent = 0;
    while(sent < size)
    {
        ssize_t n = write(fd, data + sent, size - sent);
        if(n <= 0) { close(fd); return false; }
        sent += (size_t)n;
    }

    uint8_t resp[16];
    size_t got = 0;
    while(got < sizeof(resp))
    {
        ssize_t n = read(fd, resp + got, sizeof(resp) - got);
        if(n <= 0) { close(fd); return false; }
        got += (size_t)n;
    }
    close(fd);

    out.valid = resp[0];
    out.stage = resp[1];
    out.refVmRan = resp[2];
    out.refVmOk = resp[3];
    memcpy(&out.refVmAcc, resp + 4, 4);
    memcpy(&out.refVmTrapCode, resp + 8, 4);
    memcpy(&out.refVmSteps, resp + 12, 4);
    return true;
}

// ── fuzz target ─────────────────────────────────────────────────────────
//
// Input format: LEB128(argCount) followed by the raw body bytes of one
// procedure (packages/machine/src/bytecode.ts's decodeBody format) -- the
// same bytes oracle_server.ts's decodeBody/validateProgram/run already
// operate on, so a byte-for-byte identical buffer reaches both sides. One
// procedure only: validateProgram's call-graph-acyclicity check (isa-core
// §8.2) rejects any CALL a single procedure could make (it'd have to call
// itself), so this loses no reachable coverage versus a real multi-proc
// whole-program encoding.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if(size == 0 || size > 4096) return 0;

    OracleResponse oracle{};
    if(!queryOracle(data, size, oracle))
    {
        fprintf(stderr, "fuzz: oracle unreachable -- start oracle_server.ts first\n");
        _exit(2);
    }
    if(!oracle.valid) return 0; // not validator-approved: out of scope by design

    uint32_t bodyOffset = 0;
    uint32_t argCount = jitc::decodeLeb128(data, 0, bodyOffset); // same bytes the oracle just parsed successfully

    // storageBytesFor(1) isn't a constant expression (Runtime::init()'s own
    // argument is fixed at 1, but the function itself isn't constexpr) --
    // g++ tolerates initializing an array sized by it as a GNU VLA
    // extension, clang doesn't. A fixed, generously-sized buffer sidesteps
    // the question: it only ever needs to hold one Runtime header plus one
    // ProcSlot (well under 256 bytes either way), never exactly that size.
    static_assert(sizeof(Runtime) + 2 * sizeof(ProcSlot) <= 256, "grow this buffer if Runtime/ProcSlot grow");
    alignas(8) uint8_t storage[256] = {};
    Runtime &rt = *reinterpret_cast<Runtime *>(storage);
    // codeArenaBase/codeArenaSize/stackLimit are dummy values -- this
    // harness never touches arena allocation, only the static per-proc
    // directory init() also builds (argCount/bodyBytes/needsLRSave, via
    // the real scanProcBody, not a hand-rolled substitute).
    bool scanOk = rt.init(data, (uint32_t)size, /*bodyOffset=*/0, /*procCount=*/1,
        /*codeArenaBase=*/0x10000, /*codeArenaSize=*/0x10000, /*stackLimit=*/0, /*arenaOverlapsStack=*/0);
    if(!scanOk) return 0; // JIT's own static ceiling (arg/body-size limits) rejected it -- graceful, not a bug

    // rt.slot(0).bodyPtr is truncated to 32 bits (a real-hardware address
    // assumption that doesn't hold on this host) -- use the real pointer
    // we already have instead, same way test_translate_proc.cpp's
    // FakeRuntime bypasses it.
    Proc proc{argCount, data + bodyOffset, rt.slot(0).bodyBytes()};

    static uint16_t buf[8192];
    Assembler a(buf, sizeof(buf) / sizeof(buf[0]));

    if(setjmp(g_resourceEscape) == 0)
    {
        translateProc(proc, 0, a, rt);
        // TODO(execute): feed buf[]/halfwordCount through an ARM execution
        // oracle (Unicorn Engine) with rt.slot(0).needsLRSave()'s ABI, and
        // compare the result against oracle.refVmAcc/refVmOk/refVmTrapCode
        // when oracle.refVmRan -- that closes the loop this harness is
        // named for. Not wired up: Unicorn isn't installed here.
    }
    else
    {
        // Assembler::fail() -> runtimeBail() -> here: the JIT bailed with
        // RESOURCE_ERROR instead of crashing -- the other acceptable
        // outcome for a validator-approved program, not a finding.
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

    srand((unsigned)time(nullptr));
    uint64_t execs = 0;
    for(;;)
    {
        std::vector<uint8_t> input = corpus[(size_t)rand() % corpus.size()];
        int mutations = 1 + rand() % 8;
        for(int i = 0; i < mutations && !input.empty(); i++)
        {
            size_t pos = (size_t)rand() % input.size();
            switch(rand() % 3)
            {
                case 0: input[pos] = (uint8_t)rand(); break;
                case 1: if(input.size() < 512) input.insert(input.begin() + (long)pos, (uint8_t)rand()); break;
                case 2: if(input.size() > 1) input.erase(input.begin() + (long)pos); break;
            }
        }
        // Written *before* running it: if this input crashes, the file left
        // behind on disk is the repro, not whatever ran after it.
        {
            std::ofstream f("last_input.bin", std::ios::binary | std::ios::trunc);
            f.write((const char *)input.data(), (std::streamsize)input.size());
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
        if(++execs % 5000 == 0) fprintf(stderr, "fuzz: %llu executions\n", (unsigned long long)execs);
    }
}
#endif

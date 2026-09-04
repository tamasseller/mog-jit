#include "semihost.h"

// ARM semihosting operation numbers (ARM DUI 0203, "Semihosting"), the
// subset this runner uses.
static constexpr uint32_t SYS_WRITE0 = 0x04;
static constexpr uint32_t SYS_EXIT_EXTENDED = 0x20;
static constexpr uint32_t ADP_STOPPED_APPLICATION_EXIT = 0x20026;

static inline uint32_t semihostingCall(uint32_t op, void *arg)
{
    register uint32_t r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void semihostWrite0(const char *s)
{
    semihostingCall(SYS_WRITE0, (void *)s);
}

[[noreturn]] void semihostExit(int code)
{
    static uint32_t block[2];
    block[0] = ADP_STOPPED_APPLICATION_EXIT;
    block[1] = (uint32_t)code;
    semihostingCall(SYS_EXIT_EXTENDED, block);
    for(;;)
    {
        // A host that ignored the exit request still shouldn't fall
        // through into whatever follows in memory.
    }
}

void semihostWriteTagged(const char *prefix, uint32_t v)
{
    char buf[24];
    uint32_t i = 0;
    for(; prefix[i]; i++) buf[i] = prefix[i];
    for(int shift = 28; shift >= 0; shift -= 4)
    {
        uint32_t nibble = (v >> shift) & 0xF;
        buf[i++] = nibble < 10 ? (char)('0' + nibble) : (char)('a' + nibble - 10);
    }
    buf[i++] = '\n';
    buf[i] = '\0';
    semihostWrite0(buf);
}

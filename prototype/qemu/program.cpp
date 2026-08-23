
#include <stdint.h>

extern "C" {
void semihosting_exit(int code);
void write_hex_result(uint32_t v);
}

__attribute__((section(".text.jitcode")))
static const uint16_t code[] = { 0x2705, 0x4638, 0x2f00, 0xdd02, 0x1e7f, 0x4638, 0xe7fa, 0x4638, 0x4770 };

int main(void)
{
    register unsigned int result;
    asm volatile(
        "mov r0, %1\n"
        "blx %2\n"
        "mov %0, r0\n"
        : "=r"(result)
        : "r"(0), "r"((uint32_t)((uintptr_t)code | 1))
        : "r0", "r3", "lr", "cc"
    );
    write_hex_result(result);
    semihosting_exit(0);
    return 0;
}

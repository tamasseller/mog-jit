#ifndef JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_
#define JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_

/* The conventions runtime.S and the C++ side both have to agree on, so this
 * header is included from both. dispatch_abi.cpp is where the agreements that
 * can be checked are asserted. */

#define DISPATCH_SENTINEL_OFFSET 16      /* dispatchBase - this = &slots[0] (the sentinel) */

#define CALL_RECORD_BYTES 4
#define CALL_RECORD_BOOT 0xffff          /* == packRecord(-1, 0) — asserted in dispatch_abi.cpp */

/* Which of the three ways an excursion ended, as the tag half of its result. */
#define LANDING_SUCCESS 0u        /* value = the entry procedure's own result */
#define LANDING_TRAP 1u           /* value = the bytecode TRAP's own code, at any call depth */
#define LANDING_RESOURCE_ERROR 2u /* value = one of resource_codes.h's own RESOURCE_* codes */

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "runtime.h"
#include "entry_args.h"
#include "stack_budget.h"

extern "C" {
extern void translatorTrampoline(void); /* runtime.S */
extern const uint16_t callHelper[];              /* runtime.S */
extern const uint16_t returnHelperFromLr[];      /* runtime.S */
extern const uint16_t returnHelperFromStack[];   /* runtime.S */
extern const uint16_t clzHelper[];               /* runtime.S */
extern const uint16_t revbitsHelper[];           /* runtime.S */
extern const uint16_t brTableJumpHelper[];       /* runtime.S */
extern const uint16_t returnHelperFromStackReclaim[]; /* runtime.S */
extern const uint16_t trapHelper[];              /* runtime.S */
extern const uint16_t extThunkHelper[];          /* runtime.S */
extern uint64_t enterDispatch(void* dispatchTable, Runtime *runtime, const EntryArgs *entryArgs); /* runtime.S */
}

#endif /* __ASSEMBLER__ */

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_ */

#ifndef JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_
#define JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_

#include <stdint.h>
#include "runtime.h"
#include "entry_args.h"

extern "C" {
extern void translatorTrampoline(void); /* runtime.S */
extern const uint16_t callHelper[];              /* runtime.S */
extern const uint16_t returnHelperFromLr[];      /* runtime.S */
extern const uint16_t returnHelperFromStack[];   /* runtime.S */
extern const uint16_t returnHelperTail[];        /* runtime.S */
extern const uint16_t clzHelper[];               /* runtime.S */
extern const uint16_t revbitsHelper[];           /* runtime.S */
extern const uint16_t brTableJumpHelper[];       /* runtime.S */
extern const uint16_t returnHelperFromStackReclaim[]; /* runtime.S */
extern const uint16_t trapHelper[];              /* runtime.S */
extern const uint16_t extThunkHelper[];          /* runtime.S */
extern uint64_t enterDispatch(void* dispatchTable, Runtime *runtime, const EntryArgs *entryArgs); /* runtime.S */
}

#define CALL_RECORD_BYTES 4

#define EXT_THUNK_STACK_BYTES 12

#define ENTER_DISPATCH_FIXED_BYTES 36

#define ENTER_PROGRAM_CORE_FRAME_BYTES 88

#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (28 + 200 + 120 + 96)

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_ */

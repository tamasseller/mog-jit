// This bare-metal target (-nostartfiles -specs=nosys.specs) never exits in
// the ordinary sense — semihostingExit halts the whole CPU/QEMU, so static
// destructors never actually need to run. But g++ still emits a
// registration call for every static object with a non-trivial destructor
// (1test's own TestHandle/TestOutput are both virtual-destructor types),
// and without a working __cxa_atexit/__dso_handle that call is an
// unresolved symbol. Newlib's real __cxa_atexit is available through
// -specs=nosys.specs, but it manages its handler list via malloc, which
// chains into _sbrk and a linker-provided `end` symbol this target's own
// linker.ld has no reason to define. Trivial stubs sidestep that whole
// chain — nothing here is ever meant to run anyway.
extern "C" void *__dso_handle;
void *__dso_handle = nullptr;

extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

// A class with any virtual functions and a non-pure virtual destructor
// gets a "deleting destructor" thunk under the Itanium C++ ABI (some
// caller might someday delete it polymorphically) — 1test's own
// TestHandle/TestOutput are both such classes, even though nothing in
// this program ever actually calls delete on one. That thunk references
// global operator delete unconditionally, and libstdc++'s real one calls
// free(), reintroducing the same malloc/_sbrk/`end` chain the __cxa_atexit
// stub above exists to avoid. These are never actually called.
void operator delete(void *) noexcept
{
}

void operator delete(void *, unsigned int) noexcept
{
}

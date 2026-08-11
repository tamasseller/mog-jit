// This image never actually deletes anything (no heap, no dynamic
// objects) — these stubs exist only to satisfy the vtable-generated
// deleting-destructor reference every polymorphic class's virtual
// destructor pulls in (SemihostingOutput/TestOutput here), without
// dragging in libsupc++'s real operator delete and, transitively,
// newlib's malloc/_sbrk — which would need a real heap region this
// project's fixed-arena design deliberately doesn't have.

void operator delete(void *, unsigned int) noexcept {}
void operator delete(void *) noexcept {}

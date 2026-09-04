// One-shot replay driver: read a single file, feed it to
// LLVMFuzzerTestOneInput once, exit. Built against harness.cpp compiled
// with -DPPL_FUZZ_LIBFUZZER_BUILD (skips harness.cpp's own dumb-driver
// main() so this one is the only main() in the link).
#include <cstdint>
#include <cstdio>
#include <vector>
#include <fstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: %s <input-file>\n", argv[0]); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> input((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    fprintf(stderr, "replaying %zu bytes from %s\n", input.size(), argv[1]);
    LLVMFuzzerTestOneInput(input.data(), input.size());
    fprintf(stderr, "no crash\n");
    return 0;
}

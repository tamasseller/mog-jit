# Thin orchestrator only — each artifact's own Makefile (test/host,
# test/qemu) is the real "one Makefile per artifact" build unit, per
# ultimate-makefile's own model (docs/design.md's scaffolding plan).

.PHONY: test test-host test-qemu test-compiler-host test-compiler-qemu clean

test: test-host test-qemu test-compiler-host test-compiler-qemu

test-host:
	$(MAKE) -C test/host check

test-qemu:
	$(MAKE) -C test/qemu qemu-test

test-compiler-host:
	$(MAKE) -C compiler/test/host check

test-compiler-qemu:
	$(MAKE) -C compiler/test/qemu qemu-test

clean:
	$(MAKE) -C test/host clean
	$(MAKE) -C test/qemu clean
	$(MAKE) -C compiler/test/host clean
	$(MAKE) -C compiler/test/qemu clean

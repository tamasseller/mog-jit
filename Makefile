# Thin orchestrator only — each artifact's own Makefile (test/host,
# test/qemu) is the real "one Makefile per artifact" build unit, per
# ultimate-makefile's own model (docs/jit-armv6m.md's scaffolding plan).

.PHONY: test test-host test-qemu clean

test: test-host test-qemu

test-host:
	$(MAKE) -C test/host check

test-qemu:
	$(MAKE) -C test/qemu qemu-test

clean:
	$(MAKE) -C test/host clean
	$(MAKE) -C test/qemu clean

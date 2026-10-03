# Convenience wrapper so `make <target>` works from the repo root.
# All real build/sign/install logic lives in driver/Makefile.

.PHONY: all sign install uninstall reload clean

all sign install uninstall reload clean:
	$(MAKE) -C driver $@

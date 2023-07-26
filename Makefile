MK_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
INSTALL_DIR := $(MK_DIR)/bin

all: schbench

3rd-party/schbench/schbench.c:
	(cd $(MK_DIR)/3rd-party/; git clone --depth 1 https://kernel.googlesource.com/pub/scm/linux/kernel/git/mason/schbench)

schbench: 3rd-party/schbench/schbench.c
	(cd $(MK_DIR)/3rd-party/schbench && make)
	cp $$(find $(MK_DIR)/3rd-party/schbench -type f -executable -print | grep -v .git) $(INSTALL_DIR)

clean:
	(cd $(MK_DIR)/3rd-party/schbench && make clean)

.PHONY: all schbench clean

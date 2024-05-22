MK_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
INSTALL_DIR := $(MK_DIR)/bin

all: schbench gbench

micro-bench/schbench/schbench.c:
	(cd $(MK_DIR)/micro-bench/; git clone --depth 1 https://kernel.googlesource.com/pub/scm/linux/kernel/git/mason/schbench)

schbench: micro-bench/schbench/schbench.c
	(cd $(MK_DIR)/micro-bench/schbench && make)
	cp $$(find $(MK_DIR)/micro-bench/schbench -type f -executable -print | grep -v '\.git/') $(INSTALL_DIR)

gbench: micro-bench/gbench/gbench.c
	(cd $(MK_DIR)/micro-bench/gbench && make)
	cp $$(find $(MK_DIR)/micro-bench/gbench -type f -executable -print) $(INSTALL_DIR)

clean:
	(cd $(MK_DIR)/micro-bench/gbench && make clean)
	(cd $(MK_DIR)/micro-bench/schbench && make clean && rm -rf $(MK_DIR)/micro-bench/schbench)

.PHONY: all schbench gbench clean

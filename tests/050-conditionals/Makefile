MODE = debug
FEAT = yes
.PHONY: all
all:
ifeq ($(MODE),debug)
	@echo mode-is-debug
else
	@echo mode-is-not-debug
endif
ifdef FEAT
	@echo feat-defined
endif
ifndef MISSING
	@echo missing-undefined
endif
ifneq ($(MODE),release)
	@echo not-release
endif

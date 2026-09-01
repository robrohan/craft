A = base
B := $(A)-simple
A = changed
C ?= fromcond
C ?= ignored
D = one
D += two
override E = ov
E = ignored
.PHONY: all
all:
	@echo A=$(A)
	@echo B=$(B)
	@echo C=$(C)
	@echo D=$(D)
	@echo E=$(E)

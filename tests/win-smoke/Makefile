# Windows smoke test: every recipe here runs under cmd.exe as-is
# (craft's default SHELL on Windows).  Driven by .github/workflows/ci.yml.
NAME = app

.PHONY: all show boom

all: main.obj util.obj
	@echo link main.obj util.obj into $(NAME)

main.obj:
	@echo compile main.c

util.obj:
	@echo compile util.c

show:
	@echo name=$(NAME) target=$@

boom:
	@echo starting
	@exit 1
	@echo never reached

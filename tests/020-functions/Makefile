L = foo.c bar.c baz.c
.PHONY: all
all:
	@echo sub=$(subst .c,.o,$(L))
	@echo pat=$(patsubst %.c,%.o,$(L))
	@echo filt=$(filter %.c,$(L) readme.txt)
	@echo fout=$(filter-out bar.c,$(L))
	@echo sort=$(sort banana apple cherry apple)
	@echo words=$(words $(L)) word2=$(word 2,$(L))
	@echo dir=$(dir a/b/c.c ./x.c)
	@echo notdir=$(notdir a/b/c.c x.c)
	@echo suffix=$(suffix a/b/c.tar.gz d.c e)
	@echo base=$(basename a/b/c.tar.gz d.c)
	@echo addpre=$(addprefix inc/,a b c)
	@echo strip=[$(strip   spaced   out   )]
	@echo if=$(if ,yes,no) $(if x,yes,no)

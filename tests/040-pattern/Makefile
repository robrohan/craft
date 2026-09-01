OBJS = a.o b.o
prog: $(OBJS)
	@echo link $^ '=>' $@
	@echo made > $@
%.o: %.c
	@echo cc $< '=>' $@
	@echo made > $@

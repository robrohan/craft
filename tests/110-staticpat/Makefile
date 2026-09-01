OBJS = a.o b.o c.o
all: $(OBJS)
$(OBJS): %.o: %.c
	@echo "cc $< -> $@ (stem $*)"
	@echo x > $@

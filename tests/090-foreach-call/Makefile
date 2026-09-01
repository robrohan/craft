DIRS = src lib test
reverse = $(2) $(1)
.PHONY: all
all:
	@echo $(foreach d,$(DIRS),[$(d)])
	@echo $(call reverse,first,second)
	@echo $(foreach n,1 2 3,n$(n))

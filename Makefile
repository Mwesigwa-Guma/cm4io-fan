KERNELRELEASE ?= `uname -r`
subdirs := emc17xx emc181x pac1934
.PHONY: $(subdirs)

all: $(subdirs)
clean: $(subdirs)
modules_install: $(subdirs)
$(subdirs):
	make -C $@ $(MAKECMDGOALS) KERNELRELEASE=$(KERNELRELEASE)
CONFIG_MODULE_SIG=n
obj-m += emc2301.o

.PHONY: build clean load unload

build:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build modules M=$(PWD)

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build clean M=$(PWD)

load:
	sudo insmod emc2301.ko

unload:
	-sudo rmmod emc2301

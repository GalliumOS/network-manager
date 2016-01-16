
obj-m += fake-rfkill.o

fake-rfkill:
	make -C /lib/modules/$(shell uname -r)/build KBUILD_SRC=/lib/modules/$(shell uname -r)/build M=$(shell pwd)/debian/tests

clean-rfkill:
	make -C /lib/modules/$(shell uname -r)/build KBUILD_SRC=/lib/modules/$(shell uname -r)/build M=$(shell pwd)/debian/tests clean

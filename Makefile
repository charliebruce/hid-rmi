MODULE_NAME := hid-rmi

LINUXINCLUDE := -I$(PWD)/include $(LINUXINCLUDE)

obj-m			+= $(MODULE_NAME).o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install: $(MODULE_NAME).ko $(MODULE_NAME).mod.c
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules_install
	modprobe -r $(MODULE_NAME)
	cp ${MODULE_NAME}.ko /lib/modules/$(shell uname -r)/kernel/drivers/hid/
# BREAKS KEYBOARD FIXME	/bin/bash install.sh ${MODULE_NAME} hid-generic
uninstall:
	/bin/bash restore.sh $(MODULE_NAME)
	modprobe -r $(MODULE_NAME)
	rm /lib/modules/$(shell uname -r)/kernel/drivers/hid/${MODULE_NAME}.ko
clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
useit:
	modprobe $(MODULE_NAME)
	sudo echo "0003:1532:011D.0003" > /sys/bus/hid/drivers/hid-generic/unbind
	sudo echo "0003:1532:011D.0003" > /sys/bus/hid/drivers/hid-rmi/bind

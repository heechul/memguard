# Default module object
MEMGUARD_OBJ := memguard.o

# Check if the 'basic' target is being invoked
# This relies on the MAKEFLAGS variable which holds command line options
ifeq ($(MAKECMDGOALS),basic)
    MEMGUARD_OBJ := memguard-basic.o
endif

obj-m := $(MEMGUARD_OBJ)

KVERSION = $(shell uname -r)
BLDDIR = /lib/modules/$(KVERSION)/build

.PHONY: all basic clean

all:
	$(MAKE) -C $(BLDDIR) M=$(PWD) modules

basic:
	$(MAKE) -C $(BLDDIR) M=$(PWD) modules MEMGUARD_OBJ=$(MEMGUARD_OBJ)

clean:
	$(MAKE) -C $(BLDDIR) M=$(PWD) clean
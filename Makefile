obj-m = memguard.o

KVERSION = $(shell uname -r)
BLDDIR= /lib/modules/$(KVERSION)/build
# $(HOME)/linux.trees.git
# 3.2.0rc3-custom
# 3.2.0-rc3-custom
all: bench
	make -C $(BLDDIR) M=$(PWD) modules

clean:
	make -C $(BLDDIR) M=$(PWD) clean
	rm hrt thr fps fps-filter
	rm *~

bench: hrt thr fps-filter fps
hrt: hrt.c
	$(CC) -O2 -o hrt hrt.c -lrt
thr: thr.c
	$(CC) -O2 -o thr thr.c -lrt
fps: fps.c
	$(CC) -O2 -o $@ $< -lrt
fps-filter: fps-filter.cpp
	$(CXX) -O2 -o $@ $< -lrt

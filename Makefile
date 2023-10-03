obj-m += prcinj.o

KDIR = /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(shell pwd) HOSTCC=x86_64-linux-gnu-gcc-12 CC=x86_64-linux-gnu-gcc-12 modules

clean:
	make -C $(KDIR) M=$(shell pwd) clean

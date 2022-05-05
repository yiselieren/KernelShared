obj-m	+= shmdrv.o
shmdrv  := shmdrv.o
LINUX_KERNEL	?= /lib/modules/$(shell uname -r)/build
PWD				:= $(shell pwd)

all: usertest1 usertest2 kernel

kernel:
	make -C $(LINUX_KERNEL) M=$(PWD) modules

clean:
	make -C $(LINUX_KERNEL) M=$(PWD) clean
	rm -f usertest usertest1 usertest2

usertes1: usertest1.c shmdrv.h
	gcc -g -o usertest -W -Werror -Wall usertest1.c
usertest2: usertest2.c shmdrv.h
	gcc -g -o usertest2 -W -Werror -Wall usertest2.c -lpthread

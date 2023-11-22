CC	:= gcc
CFLAGS	:= -Wall -g -MD -O0  -fno-pie -I ../ -mincoming-stack-boundary=3
LDFLAGS := -lpthread -ldune 
MODULE_LIB := /lib/modules/$(shell uname -r)/build

tests_files := hello test timetest test_sandbox  mprotect fork # hugepages hugepages_mod.ko

obj-m += hugepages_mod.o

all: $(tests_files)

hello: hello.o ../libdune/libdune.a
	$(CC) $(CFLAGS)  $(<) -L../libdune $(LDFLAGS) -o $(@)

mprotect: mprotect.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

fork: fork.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

timetest: timetest.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

test: test.o ../libdune/libdune.a
	$(CC) $(CFLAGS)  $(<) -L../libdune $(LDFLAGS) -o $(@)

test_sandbox: test_sandbox.o
	$(CC) $(CFLAGS) -static $(<) -pthread -o $(@)

hugepages: hugepages.o ../libdune/libdune.a
	$(CC) $(CFLAGS) -static $(<) -L../libdune $(LDFLAGS) -o $(@)

hugepages_mod.ko: hugepages_mod.c
	make -C $(MODULE_LIB) M=$(PWD) CFLAGS= LDFLAGS= modules

clean:
	rm -f *.o test *.d $(tests_files)
#	 make -C $(MODULE_LIB) M=$(PWD) clean

-include *.d

format:
	clang-format -i $(wildcard *.[ch])

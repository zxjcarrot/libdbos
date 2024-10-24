CC	:= gcc
CFLAGS	:= -Wall -g3 -MD -O0  -no-pie -I ../ -mincoming-stack-boundary=3
LDFLAGS := -lpthread -ldune  -no-pie
MODULE_LIB := /lib/modules/$(shell uname -r)/build

tests_files := hello test timetest test_sandbox posted_ipi mprotect fork test_ipi # hugepages hugepages_mod.ko

obj-m += hugepages_mod.o

all: $(tests_files)

hello: hello.o ../libdune/libdune.a
	$(CC) $(CFLAGS)  $(<) -L../libdune $(LDFLAGS) -o $(@)

test_ipi: test_ipi.o ../libdune/libdune.a
	$(CC) $(CFLAGS)  $(<) -L../libdune $(LDFLAGS) -o $(@)

mprotect: mprotect.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

fork: fork.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

timetest: timetest.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

test: test.o ../libdune/libdune.a
	$(CC) $(CFLAGS)  $(<) -L../libdune $(LDFLAGS) -o $(@)

posted_ipi: posted_ipi.o
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

test_sandbox: test_sandbox.o
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

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

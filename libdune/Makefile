CC      = gcc
CFLAGS  = -Wall -g -O3 -MD -mincoming-stack-boundary=3 -no-pie
LDFLAGS = -static -L. -ldune -lpthread
OBJ     = entry.o dune.o vsyscall.o elf.o vm.o util.o page.o procmap.o debug.o apic.o ipi_call.o cpu.o fast_spawn.o
NOFPU_OBJ = trap.o

$(NOFPU_OBJ): EXTRA_FLAGS := -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -msoft-float

all: libdune.a

libdune.a: $(OBJ) $(NOFPU_OBJ)
	$(AR) crD $(@) $(OBJ) $(NOFPU_OBJ)

clean:
	rm -f *.o test *.d libdune.a

-include *.d

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) -o $@ -c $<

format:
	clang-format -i $(wildcard *.[ch])

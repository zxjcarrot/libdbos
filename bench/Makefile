CC	:= gcc
CPP	:= g++
CFLAGS	:= -DNDEBUG -Wall -g -MD -O3 -I../
CPPFLAGS	:=  -DNDEBUG -Wall -g -MD -O3 -I../
LDFLAGS := -ldune -lpthread

bench_files := bench_dune bench_dune_ring bench_linux posted_ipi posted_ipi_send posted_ipi_receive posted_ipi_ping_pong bench_fork_cow bench_tabby_cow bench_ipi

all: $(bench_files)

bench_dune: bench_dune.o ../libdune/libdune.a
	  $(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

bench_dune_ring: bench_dune_ring.o ../libdune/libdune.a
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

bench_linux: bench_linux.o
	$(CC) $(CFLAGS) $(<) -o $(@)

bench_ipi: bench_ipi.o
	$(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)
	
bench_fork_cow: bench_fork_cow.o
	$(CPP) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

bench_tabby_cow: bench_tabby_cow.o 
	$(CPP) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

posted_ipi: posted_ipi.o ../libdune/libdune.a
	  $(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

posted_ipi_send: posted_ipi_send.o ../libdune/libdune.a
	  $(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

posted_ipi_receive: posted_ipi_receive.o ../libdune/libdune.a
	  $(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

posted_ipi_ping_pong: posted_ipi_ping_pong.o ../libdune/libdune.a
	  $(CC) $(CFLAGS) $(<) -L../libdune $(LDFLAGS) -o $(@)

clean:
	rm -f *.o test *.d $(bench_files)

-include *.d

format:
	clang-format -i $(wildcard *.[ch])

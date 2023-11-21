#define _GNU_SOURCE

#include <malloc.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>

#include "dune.h"
#include "cpu-x86.h"

#define XAPIC_EOI_OFFSET 0xB0
#define APIC_EOI_ACK 0x0

typedef uint8_t u8;
typedef uint32_t u32;

#define NUM_CORES 128
#define POSTED_INTR_VECTOR 0xF2

#define APIC_ID_MSR 0x802
#define APIC_ICR_MSR 0x830
#define APIC_EOI_MSR 0x80B

#define EOI_ACK 0x0

#define APIC_DEST_PHYSICAL 0x00000
#define APIC_DM_FIXED 0x00000
#define APIC_EOI 0xB0
#define APIC_EOI_ACK 0x0
#define APIC_ICR 0x300
#define APIC_ICR2 0x310
#define APIC_ICR_BUSY 0x01000
#define SET_APIC_DEST_FIELD(x) ((x) << 24)

static int *apic_routing;

typedef struct posted_interrupt_desc {
    u32 vectors[8]; /* posted interrupt vectors */
    u32 extra[8]; /* outstanding notification indicator and extra space the VMM can use */
} __attribute__((aligned(64))) posted_interrupt_desc;

static inline void *posted_interrupt_desc_entry_for_core(uint32_t core) {
	return (void *)(POSTED_INTR_DESCS_BASE + (core * PAGE_SIZE));
}

static inline void apic_write(u32 reg, u32 v)
{
	volatile u32 *addr = (volatile u32 *)(APIC_BASE + reg);
	asm volatile("movl %0, %P1" : "=r" (v), "=m" (*addr) : "i" (0), "0" (v), "m" (*addr));
}

static inline uint32_t apic_read(uint32_t reg)
{
	return *((volatile uint32_t *)(APIC_BASE + reg));
}

uint32_t dune_apic_id() {
    uint32_t apic_id = apic_read(0x20);
    printf("raw apic_id 0x%x\n", apic_id);
    apic_id >>= 24;
    apic_id &= 0xFF;
    return apic_id;
}

void dune_setup_apic() {
    size_t size = sizeof(int) * get_nprocs();
    apic_routing = malloc(size);
    memset(apic_routing, -1, size);
    asm("mfence" ::: "memory");
    printf("setup_apic for %lu processors\n", size / sizeof(int));
}

void dune_apic_init_rt_entry() {
    unsigned int core_id, numa_node;
    syscall(SYS_getcpu, &core_id, &numa_node, NULL);
    apic_routing[core_id] = dune_apic_id();
    asm("mfence" ::: "memory");
    printf("dune_apic_init_rt_entry core_id %u set to apic_id %u\n", core_id, apic_routing[core_id]);
}

uint32_t dune_apic_id_for_cpu(uint32_t cpu, bool *error) {
        if (cpu >= NUM_CORES || apic_routing[cpu] == -1) {
            if (error) *error = true;
            return 0;
        }
        return apic_routing[cpu];
}

static inline unsigned int __prepare_ICR(unsigned int shortcut, int vector, unsigned int dest)
{
	return shortcut | dest | APIC_DM_FIXED | vector;
}

static inline int __prepare_ICR2(unsigned int mask)
{
	return SET_APIC_DEST_FIELD(mask);
}

static inline void __xapic_wait_icr_idle(void)
{
	while (apic_read(APIC_ICR) & APIC_ICR_BUSY);
}

static inline void __default_send_IPI_dest_field(unsigned int mask, int vector, unsigned int dest)
{
	__xapic_wait_icr_idle();
	apic_write(APIC_ICR2, __prepare_ICR2(mask));
	apic_write(APIC_ICR, __prepare_ICR(0, vector, dest));
}

static inline void apic_send_ipi(int apicid, int vector)
{
	__default_send_IPI_dest_field(apicid, vector, APIC_DEST_PHYSICAL);
}

void dune_apic_eoi() {
        wrmsrl(APIC_EOI_MSR, EOI_ACK);
}

#define LOCK_PREFIX_HERE \
		".section .smp_locks,\"a\"\n"	\
		".balign 4\n"			\
		".long 671f - .\n" /* offset */	\
		".previous\n"			\
		"671:"

#define LOCK_PREFIX LOCK_PREFIX_HERE "\n\tlock; "

/*
 * These have to be done with inline assembly: that way the bit-setting
 * is guaranteed to be atomic. All bit operations return 0 if the bit
 * was cleared before the operation and != 0 if it was not.
 *
 * bit 0 is the LSB of addr; bit 32 is the LSB of (addr+1).
 */

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 1)
/* Technically wrong, but this avoids compilation errors on some gcc
   versions. */
#define BITOP_ADDR(x) "=m" (*(volatile long *) (x))
#else
#define BITOP_ADDR(x) "+m" (*(volatile long *) (x))
#endif

#define ADDR				BITOP_ADDR(addr)

static inline int test_and_set_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	asm volatile(LOCK_PREFIX "bts %2,%1\n\t"
		     "sbb %0,%0" : "=r" (oldbit), ADDR : "Ir" (nr) : "memory");

	return oldbit;
}

void dune_apic_send_posted_ipi(u8 vector, u32 destination_core) {
    posted_interrupt_desc *desc;
    desc = posted_interrupt_desc_entry_for_core(destination_core);


    //first set the posted-interrupt request
    if (test_and_set_bit(vector, (unsigned long *)desc->vectors)) {
        //bit already set, so the interrupt is already pending (and
        //the outstanding notification bit is 1)
        return;
    }
    
    //set the outstanding notification bit to 1
    if (test_and_set_bit(0, (unsigned long *)desc->extra)) {
        //bit already set, so there is an interrupt(s) already pending
        return;
    }
    
    //now send the posted interrupt vector to the destination
    bool error = false;
    u32 destination_apic_id = dune_apic_id_for_cpu(destination_core, &error);
    if (error) return;
    apic_send_ipi(destination_apic_id, POSTED_INTR_VECTOR);
}

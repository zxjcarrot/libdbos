#define _GNU_SOURCE

#include <linux/mm.h>
//#include <asm/ipi.h>

#include "dune.h"

#define XAPIC_EOI_OFFSET 0xB0
#define APIC_EOI_ACK 0x0

//TODO: Get highest APIC ID in system
static int num_rt_entries = 128;
static int apic_routing[128];

#define BY_APIC_TYPE(x, x2) if (x2apic_enabled()) { x2; } else { x; }
#include <asm/hw_irq.h>
#include <asm/apic.h>
#include <asm/smp.h>
#include <asm/apicdef.h>
#ifndef SET_APIC_DEST_FIELD
#define SET_APIC_DEST_FIELD(x) ((x) << 24)
#endif 

/*
 * the following functions deal with sending IPIs between CPUs.
 *
 * We use 'broadcast', CPU->CPU IPIs and self-IPIs too.
 */

static inline unsigned int __prepare_ICR(unsigned int shortcut, int vector,
					 unsigned int dest)
{
	unsigned int icr = shortcut | dest;

	switch (vector) {
	default:
		icr |= APIC_DM_FIXED | vector;
		break;
	case NMI_VECTOR:
		icr |= APIC_DM_NMI;
		break;
	}
	return icr;
}

static inline int __prepare_ICR2(unsigned int mask)
{
	return SET_APIC_DEST_FIELD(mask);
}

static inline void __xapic_wait_icr_idle(void)
{
	while (native_apic_mem_read(APIC_ICR) & APIC_ICR_BUSY)
		cpu_relax();
}

static inline void
__default_send_IPI_shortcut(unsigned int shortcut, int vector, unsigned int dest)
{
	/*
	 * Subtle. In the case of the 'never do double writes' workaround
	 * we have to lock out interrupts to be safe.  As we don't care
	 * of the value read we use an atomic rmw access to avoid costly
	 * cli/sti.  Otherwise we use an even cheaper single atomic write
	 * to the APIC.
	 */
	unsigned int cfg;

	/*
	 * Wait for idle.
	 */
	__xapic_wait_icr_idle();

	/*
	 * No need to touch the target chip field
	 */
	cfg = __prepare_ICR(shortcut, vector, dest);

	/*
	 * Send the IPI. The write to APIC_ICR fires this off.
	 */
	native_apic_mem_write(APIC_ICR, cfg);
}

/*
 * This is used to send an IPI with no shorthand notation (the destination is
 * specified in bits 56 to 63 of the ICR).
 */
static inline void
 __default_send_IPI_dest_field(unsigned int mask, int vector, unsigned int dest)
{
	unsigned long cfg;

	/*
	 * Wait for idle.
	 */
	if (unlikely(vector == NMI_VECTOR))
		safe_apic_wait_icr_idle();
	else
		__xapic_wait_icr_idle();

	/*
	 * prepare target chip field
	 */
	cfg = __prepare_ICR2(mask);
	native_apic_mem_write(APIC_ICR2, cfg);

	/*
	 * program the ICR
	 */
	cfg = __prepare_ICR(0, vector, dest);

	/*
	 * Send the IPI. The write to APIC_ICR fires this off.
	 */
	native_apic_mem_write(APIC_ICR, cfg);
}

static inline void x2apic_wrmsr_fence(void)
{
	asm volatile("mfence" : : : "memory");
}


u32 apic_id(void) {
	return read_apic_id();
}

void apic_init(void) {
	memset(apic_routing, -1, sizeof(int) * num_rt_entries);
	asm("mfence" ::: "memory");
	if (x2apic_enabled()) {
		printk(KERN_INFO "vmx: x2apic enabled");
	}
}

void apic_init_rt_entry(void) {
	apic_routing[apic_id()] = raw_smp_processor_id();
	asm("mfence" ::: "memory");
}

u32 apic_get_cpu_id_for_apic(u32 apic, bool *error) {
	if (apic >= num_rt_entries) {
		if (error) *error = true;
		return 0;
	}
	return apic_routing[apic];
}

/* apic_write_x
 * Writes to the xAPIC's memory-mapped registrers.
 *
 * [reg] is the offset to write to within the memory region reserved
 * by the xAPIC.
 * [v] is the value to write.
 */
static inline void apic_write_x(u32 reg, u32 v)
{
	volatile u32 *addr = (volatile u32 *)(APIC_BASE + reg);
	asm volatile("movl %0, %P1" : "=r" (v), "=m" (*addr) : "i" (0), "0" (v), "m" (*addr));
}

/* apic_send_ipi_x2
 * Send an IPI to another local APIC. This function only supports x2APIC, not xAPIC.
 *
 * [vector] is the vector of the interrupt to send.
 * [destination_apic_id] is the ID of the local APIC that will receive the IPI.
 */
static void apic_send_ipi_x2(u8 vector, u32 destination_apic_id) {
	u32 low;
	low = __prepare_ICR(0, vector, APIC_DEST_PHYSICAL);
	x2apic_wrmsr_fence();
	wrmsrl(APIC_BASE_MSR + (APIC_ICR >> 4), ((__u64) destination_apic_id) << 32 | low);
}

/* apic_send_ipi_x
 * Send an IPI to another local APIC. This function only supports xAPIC, not x2APIC.
 *
 * [vector] is the vector of the interrupt to send.
 * [destination_apic_id] is the ID of the local APIC that will receive the IPI.
 */
static void apic_send_ipi_x(u8 vector, u8 destination_apic_id) {
	__default_send_IPI_dest_field(destination_apic_id, vector, APIC_DEST_PHYSICAL);
}

/* apic_send_ipi
 * Send an IPI to another local APIC. Determines whether the computer is equipped
 * with xAPICs or x2APICs and chooses the correct delivery method.
 *
 * [vector] is the vector of the interrupt to send.
 * [destination_apic_id] is the ID of the local APIC that will receive the IPI.
 */
void apic_send_ipi(u8 vector, u32 destination_apic_id) {
	BY_APIC_TYPE(apic_send_ipi_x(vector, (u8)destination_apic_id),
		     apic_send_ipi_x2(vector, destination_apic_id))
}

/* apic_write_eoi
 * Acknowledges receipt of an interrupt to the local APIC by writing an acknowledgment to
 * the local APIC's EOI register. Determines whether the computer is equipped with xAPICs 
 * or x2APICs and writes the acknowledgment accordingly.
 *
 * [vector] is the vector of the interrupt to send.
 * [destination_apic_id] is the ID of the local APIC that will receive the IPI.
 */
void apic_write_eoi(void) {
	BY_APIC_TYPE(apic_write_x(XAPIC_EOI_OFFSET, APIC_EOI_ACK),
		     wrmsrl(APIC_BASE_MSR + (APIC_EOI >> 4), APIC_EOI_ACK))
}

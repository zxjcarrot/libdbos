/**
 * core.c - the Dune core
 *
 * Dune allows ordinary Linux processes to access the full collection of x86
 * hardware protection and isolation mechanisms (e.g. paging, segmentation)
 * through hardware virtualization. Unlike traditional virtual machines,
 * Dune processes can make ordinary POSIX system calls and, with the exception
 * of access to privileged hardware features, are treated like normal Linux
 * processes.
 *
 * FIXME: Currently only Intel VMX is supported.
 *
 * Authors:
 *   Adam Belay   <abelay@stanford.edu>
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/perf_event.h>
#include <asm/uaccess.h>

#include "dune.h"
#include "vmx.h"
#include "preempttrap.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A driver for Dune");

/* Callbacks for perf tool.  We intentionally make a wrong assumption that we
 * are always in the kernel mode because perf cannot profile user applications
 * on guest.
 * Callbacks are registered and unregistered along with Dune module.
 */
static int dune_is_in_guest(void)
{
	return __this_cpu_read(local_vcpu) != NULL;
}

static int dune_is_user_mode(void)
{
	return 0;
}

static __attribute__((unused)) void dune_dump_config(struct dune_config *conf)
{
	printk(KERN_INFO "dune: --- Begin Config Dump ---\n");
	printk(KERN_INFO "dune: RET %lld STATUS %lld\n", conf->ret, conf->status);
	printk(KERN_INFO "dune: RFLAGS 0x%016llx CR3 0x%016llx RIP 0x%016llx\n", conf->rflags, conf->cr3, conf->rip);
	printk(KERN_INFO "dune: RAX 0x%016llx RCX 0x%016llx\n", conf->rax, conf->rcx);
	printk(KERN_INFO "dune: RDX 0x%016llx RBX 0x%016llx\n", conf->rdx, conf->rbx);
	printk(KERN_INFO "dune: RSP 0x%016llx RBP 0x%016llx\n", conf->rsp, conf->rbp);
	printk(KERN_INFO "dune: RSI 0x%016llx RDI 0x%016llx\n", conf->rsi, conf->rdi);
	printk(KERN_INFO "dune: R8  0x%016llx R9  0x%016llx\n", conf->r8, conf->r9);
	printk(KERN_INFO "dune: R10 0x%016llx R11 0x%016llx\n", conf->r10, conf->r11);
	printk(KERN_INFO "dune: R12 0x%016llx R13 0x%016llx\n", conf->r12, conf->r13);
	printk(KERN_INFO "dune: R14 0x%016llx R15 0x%016llx\n", conf->r14, conf->r15);
	printk(KERN_INFO "dune: --- End Config Dump ---\n");
}

static unsigned long dune_get_guest_ip(void)
{
	unsigned long long ip = 0;
	if (__this_cpu_read(local_vcpu))
		ip = vmcs_readl(GUEST_RIP);
	return ip;
}

static struct perf_guest_info_callbacks dune_guest_cbs = {
	.is_in_guest = dune_is_in_guest,
	.is_user_mode = dune_is_user_mode,
	.get_guest_ip = dune_get_guest_ip,
};

static int dune_enter(struct dune_config *conf, int64_t *ret)
{
	return vmx_launch(conf, ret);
}

static long dune_dev_ioctl(struct file *filp, unsigned int ioctl,
						   unsigned long arg)
{
	long r = -EINVAL;
	struct dune_config conf;
	//struct dune_config conf_old;
	//struct dune_config conf_old2;
	struct dune_layout layout;

	switch (ioctl) {
	case DUNE_ENTER:
		// r = copy_from_user(&conf_old, (int __user *)arg,
		// 				   sizeof(struct dune_config));
		// if (r) {
		// 	printk(KERN_INFO "copy_from_user 1 failed\n");
		// 	r = -EIO;
		// 	goto out;
		// }
		r = copy_from_user(&conf, (int __user *)arg,
						   sizeof(struct dune_config));
		if (r) {
			printk(KERN_INFO "copy_from_user 2 failed\n");
			r = -EIO;
			goto out;
		}

		r = dune_enter(&conf, &conf.ret);
		if (r)
			break;

		// printk(KERN_INFO "&conf (user) = %lx before copy\n", (uintptr_t)arg);
		// dune_dump_config(&conf_old);
		r = copy_to_user((int __user *)arg, &conf, sizeof(struct dune_config));
		// printk(KERN_INFO "&conf (user) = %lx, copy_to_user %lu bytes, r=%ld \n", (uintptr_t)arg, sizeof(struct dune_config), r);
		// dune_dump_config(&conf);
		if (r) {
			printk(KERN_INFO "copy_to_user 1 failed\n");
			r = -EIO;
			goto out;
		}

		// r = copy_from_user(&conf_old2, (int __user *)arg,
		// 				   sizeof(struct dune_config));
		// if (r) {
		// 	printk(KERN_INFO "copy_from_user 3 failed\n");
		// 	r = -EIO;
		// 	goto out;
		// }
		// dune_dump_config(&conf_old2);
		break;

	case DUNE_GET_SYSCALL:
		rdmsrl(MSR_LSTAR, r);
		printk(KERN_INFO "R %lx\n", (unsigned long)r);
		break;

	case DUNE_GET_LAYOUT:
		layout.phys_limit = (1UL << boot_cpu_data.x86_phys_bits);
		layout.base_map = LG_ALIGN(current->mm->mmap_base) - GPA_MAP_SIZE;
		layout.base_stack = LG_ALIGN(current->mm->start_stack) - GPA_STACK_SIZE;
		r = copy_to_user((void __user *)arg, &layout,
						 sizeof(struct dune_layout));
		if (r) {
			r = -EIO;
			goto out;
		}
		break;

	case DUNE_TRAP_ENABLE:
		printk(KERN_INFO "DUNE_TRAP_ENABLE\n");
		r = dune_trap_enable(arg);
		break;

	case DUNE_TRAP_DISABLE:
		printk(KERN_INFO "DUNE_TRAP_DISABLE\n");
		r = dune_trap_disable(arg);
		break;

	default:
		return -ENOTTY;
	}

out:
	return r;
}

static int dune_dev_release(struct inode *inode, struct file *file)
{
	vmx_cleanup();
	return 0;
}

static const struct file_operations dune_chardev_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dune_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = dune_dev_ioctl,
#endif
	.llseek = noop_llseek,
	.release = dune_dev_release,
};

static struct miscdevice dune_dev = {
	DUNE_MINOR,
	"dune",
	&dune_chardev_ops,
};

static int __init dune_init(void)
{
	int r;
	perf_register_guest_info_callbacks(&dune_guest_cbs);

	printk(KERN_ERR "Dune module loaded\n");

	if ((r = vmx_init())) {
		printk(KERN_ERR "dune: failed to initialize vmx\n");
		return r;
	}

	r = misc_register(&dune_dev);
	if (r) {
		printk(KERN_ERR "dune: misc device register failed\n");
		vmx_exit();
	}

	return r;
}

static void __exit dune_exit(void)
{
	perf_unregister_guest_info_callbacks(&dune_guest_cbs);
	misc_deregister(&dune_dev);
	vmx_exit();
}

module_init(dune_init);
module_exit(dune_exit);

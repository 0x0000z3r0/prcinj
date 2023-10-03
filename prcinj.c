#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pid.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/mmu_context.h>
#include <linux/mm_types.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>

#include "prcinj.h"

int (*do_mprotect_pkey)(unsigned long, size_t, unsigned long, int) = NULL;

struct prcinj_work {
	struct work_struct 	work;
	struct mm_struct 	*mm;
	unsigned long		start;
	size_t			len;
	unsigned long		prot;
	int			res;
};

static void
prcinj_work(struct work_struct *work)
{
	struct prcinj_work *cont;
	cont = container_of(work, struct prcinj_work, work);

	kthread_use_mm(cont->mm);
	{
		cont->res = do_mprotect_pkey(cont->start, cont->len, cont->prot, -1);
	}
	kthread_unuse_mm(cont->mm);
}

static long
prcinj_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long res;

	struct prcinj_req 	req;
	struct pid	 	*pid;
	struct task_struct 	*task;
	struct mm_struct	*mm;
	struct prcinj_work	work;

	pid 	= NULL;
	task 	= NULL;
	mm	= NULL;

	if (cmd != PRCINJ_IOCTL_PROT)
		return -ENOTTY;

	if (copy_from_user(&req, (void*)arg, sizeof (req)))
		return -EFAULT;

	pid = find_get_pid(req.pid);
	if (pid == NULL)
		return -ESRCH;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (task == NULL) {
		res = -ESRCH;
		goto _IOCTL_EXIT;
	}

	mm = get_task_mm(task);
	if (task == NULL) {
		res = -ESRCH;
		goto _IOCTL_EXIT;
	}

	INIT_WORK(&work.work, prcinj_work);

	work.mm 	= mm;
	work.start	= (unsigned long)req.addr;
	work.len	= req.len;
	work.prot	= req.prot;

	schedule_work(&work.work);

	flush_work(&work.work);
	res = work.res;

_IOCTL_EXIT:
	if (task != NULL)
		put_task_struct(task);
	if (pid != NULL)
		put_pid(pid);

	return res;
}

static const struct file_operations prcinj_fops = {
	.owner 		= THIS_MODULE,
	.unlocked_ioctl	= prcinj_ioctl
};

static struct miscdevice prcinj_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "prcinj",
	.fops	= &prcinj_fops
};

unsigned long prcinj_kp_addr = 0;
unsigned long (*kallsyms_lookup_name_func)(const char*) = NULL;

static int __kprobes
prcinj_handler_pre0(struct kprobe *kp, struct pt_regs *regs)
{
	--regs->ip;
	prcinj_kp_addr = regs->ip;

	return 0;
}

static int __kprobes
prcinj_handler_pre1(struct kprobe *kp, struct pt_regs *regs)
{
	return 0;
}

static int __init
prcinj_init(void)
{
	int res;

	struct kprobe kp0;
	struct kprobe kp1;

	static const char kallsyms_lookup_name_str[] = "kallsyms_lookup_name";

	kp0.symbol_name = kallsyms_lookup_name_str;
	kp0.pre_handler = prcinj_handler_pre0;
	res = register_kprobe(&kp0);
	if (res < 0) {
		pr_err("failed to register a kprobe handler0\n");
		return res;
	}

	kp1.symbol_name = kallsyms_lookup_name_str;
	kp1.pre_handler = prcinj_handler_pre1;
	res = register_kprobe(&kp1);
	if (res < 0) {
		unregister_kprobe(&kp0);

		pr_err("failed to register a kprobe handler1\n");
		return res;
	}

	unregister_kprobe(&kp1);
	unregister_kprobe(&kp0);

	kallsyms_lookup_name_func = (void*)prcinj_kp_addr;

	do_mprotect_pkey = (void*)kallsyms_lookup_name_func("do_mprotect_pkey");
	if (do_mprotect_pkey == NULL) {
		pr_err("failed to find a kernel function\n");
		return -ENOSYS;
	}
		

	res = misc_register(&prcinj_dev);
	if (res) {
		pr_err("failed to register the device\n");
		return res;
	}

	pr_info("registered the device\n");

	return 0;
}

static void __exit
prcinj_exit(void)
{
	misc_deregister(&prcinj_dev);
	pr_info("deregistered the device\n");
}

module_init(prcinj_init);
module_exit(prcinj_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("0xZ3R0");
MODULE_DESCRIPTION("Process Injection Tool");
MODULE_VERSION("v0.1");

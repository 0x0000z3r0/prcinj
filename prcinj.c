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
#include <linux/mmap_lock.h>
#include <linux/userfaultfd_k.h>
#include <linux/mman.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/string.h>

#include "prcinj.h"

int (*do_mprotect_pkey_func)(unsigned long, size_t, unsigned long, int) = NULL;
unsigned long (*do_mmap_func)(struct file*, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*, struct list_head*) = NULL;
unsigned long (*ksys_mmap_pgoff_func)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) = NULL;
void (*userfaultfd_unmap_complete_func)(struct mm_struct*, struct list_head*);
unsigned long (*mm_populate_func)(unsigned long, unsigned long, int) = NULL;

struct prcinj_work {
	struct work_struct 	work;
	struct mm_struct 	*mm;
	unsigned char		*shell;
	size_t			len;
	unsigned long		res;
};

static void
prcinj_work(struct work_struct *work)
{
	struct prcinj_work *cont;
	struct mm_struct   *mm;

	size_t		idx;
	size_t		rem;
	int		res;
	unsigned long 	pop;
	size_t		algn;
	void		*map;

	const size_t page_size = 4096;

	LIST_HEAD(uf);

	struct page 		**pages;
	struct vm_area_struct 	**vmas;

	cont = container_of(work, struct prcinj_work, work);

	idx	= 0;
	rem	= 0;
	res  	= 0;
	algn 	= 0;
	pop  	= 0;
	mm   	= cont->mm;
	pages	= NULL;
	vmas	= NULL;
	map	= NULL;

	kthread_use_mm(cont->mm);
	{
		algn = (cont->len & ~(page_size - 1)) + page_size;
		pr_info("aligned the length, from %zu to %zu\n", cont->len, algn);

		cont->res = vm_mmap(NULL, 0, page_size, 
				PROT_READ | PROT_WRITE | PROT_EXEC, 
				MAP_PRIVATE | MAP_ANONYMOUS, 0);

		pr_info("allocated a new page 0x%016lX", cont->res);

		if (cont->res == -1) {
			pr_err("failed to map a new page\n");

			kthread_unuse_mm(cont->mm);
			return;
		}

		pr_info("copying the shell code, %02X%02X%02X...\n", 
				cont->shell[0], cont->shell[1], cont->shell[2]);

		// TODO: probably don't need the second stage
		if (!copy_to_user((void*)cont->res, cont->shell, cont->len)) {
			pr_info("copied the shell code over\n");

			return;
		} else {
			pages = kmalloc(sizeof (struct page*) * (algn / page_size), GFP_KERNEL);
			if (pages == NULL) {
				pr_err("failed to allocate the page vector\n");
				goto _WORK_EXIT;
			}

			vmas = kmalloc(sizeof (struct vm_area_struct*) * (algn / page_size), GFP_KERNEL);
			if (vmas == NULL) {
				pr_err("failed to allocate the vma vector\n");
				goto _WORK_EXIT;
			}
	
			res = get_user_pages(cont->res, algn / page_size, 0, pages, vmas);	
			if (!res) {
				pr_err("failed to get the user pages\n");
				goto _WORK_EXIT;
			}

			rem = cont->len % page_size;
			for (idx = 0; idx < algn / page_size - 1; ++idx) {
				map = kmap(pages[idx]);
				memcpy(map, cont->shell + idx * page_size, page_size);
				kunmap(pages[idx]);
			}

			map = kmap(pages[idx]);
			memcpy(map, cont->shell + idx * page_size, rem);
			kunmap(pages[idx]);

			// TODO: why am I forced to this again?
			cont->res = -cont->res;

_WORK_EXIT:
			if (vmas != NULL)
				kfree(vmas);

			if (pages != NULL) {
				for (idx = 0; idx < algn / page_size; ++idx) {
					put_page(pages[idx]);
				}

				kfree(pages);
			}
		}
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

	unsigned char *shell;

	shell	= NULL;
	pid 	= NULL;
	task 	= NULL;
	mm	= NULL;

	if (cmd != PRCINJ_IOCTL_INJ)
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

	shell = kmalloc(req.len, GFP_KERNEL);
	if (shell == NULL) {
		pr_err("failed to allocate a buffer for the user shell code\n");
		goto _IOCTL_EXIT;
	}

	if (copy_from_user(shell, req.shell, req.len)) {
		pr_err("failed to copy the user shell code\n");

		res = -EFAULT;
		goto _IOCTL_EXIT;
	}

	INIT_WORK(&work.work, prcinj_work);

	work.mm 	= mm;
	work.shell	= shell;
	work.len	= req.len;

	pr_info("scheduling the work\n");
	schedule_work(&work.work);

	flush_work(&work.work);
	res = work.res;

	pr_info("finished the work\n");

_IOCTL_EXIT:
	if (shell != NULL)
		kfree(shell);
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

	do_mprotect_pkey_func = (void*)kallsyms_lookup_name_func("do_mprotect_pkey");
	if (do_mprotect_pkey_func == NULL) {
		pr_err("failed to find a kernel function (mprotect)\n");
		return -ENOSYS;
	}

	do_mmap_func = (void*)kallsyms_lookup_name_func("do_mmap");
	if (do_mmap_func == NULL) {
		pr_err("failed to find a kernel function (mmap)\n");
		return -ENOSYS;
	}

	mm_populate_func = (void*)kallsyms_lookup_name_func("__mm_populate");
	if (mm_populate_func == NULL) {
		pr_err("failed to find a kernel function (mm_populate)\n");
		return -ENOSYS;
	}

	userfaultfd_unmap_complete_func = (void*)kallsyms_lookup_name_func("userfaultfd_unmap_complete");
	if (userfaultfd_unmap_complete_func == NULL) {
		pr_err("failed to find a kernel function (userfaultfd_unmap_complete)\n");
		return -ENOSYS;
	}

	ksys_mmap_pgoff_func = (void*)kallsyms_lookup_name_func("ksys_mmap_pgoff_func");
	if (ksys_mmap_pgoff_func == NULL) {
		pr_err("failed to find a kernel function (mmap_pgoff)\n");
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

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>

static struct kprobe kp = {
    .symbol_name = "kernel_clone",
};

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    pr_info("clone_probe: pid=%d tgid=%d comm=%s\n",
            current->pid, current->tgid, current->comm);
    return 0;
}

static int __init probe_init(void)
{
    kp.pre_handler = handler_pre;
    int ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("clone_probe: register_kprobe failed: %d\n", ret);
        return ret;
    }
    pr_info("clone_probe: registered at %pS\n", kp.addr);
    return 0;
}

static void __exit probe_exit(void)
{
    unregister_kprobe(&kp);
    pr_info("clone_probe: unregistered\n");
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kprobe on kernel_clone()");

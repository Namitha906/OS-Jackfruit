/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME      "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1 (FIXED): Linked-list node struct.
 *
 * Tracks one monitored process:
 *   - pid           : the host PID registered by the supervisor
 *   - container_id  : human-readable container label (from ioctl req)
 *   - soft_limit    : RSS threshold for warning (bytes)
 *   - hard_limit    : RSS threshold for SIGKILL (bytes)
 *   - soft_triggered: flag so the warning fires only once
 *   - list          : kernel list linkage
 * ============================================================== */
struct monitored_entry {
    pid_t          pid;
    char           container_id[64];
    unsigned long  soft_limit;
    unsigned long  hard_limit;
    int            soft_triggered;
    struct list_head list;
};

/* ==============================================================
 * TODO 2 (FIXED): Global list and protecting mutex.
 *
 * A mutex is the right choice here: both code paths that touch
 * the list (ioctl and timer_callback) can sleep.
 *   - ioctl runs in process context (may sleep).
 *   - timer_callback uses mutex_lock(), which is safe because
 *     kernel timers run in softirq context only when
 *     HRTIMER_MODE_... flags are set; our plain timer_list
 *     callback runs in a tasklet / softirq, which normally
 *     forbids sleeping — BUT we intentionally use mutex_lock()
 *     (not spin_lock_bh) because get_rss_bytes() calls
 *     get_task_mm() / mmput() which may schedule.  If strict
 *     non-sleeping timer context is required, the design would
 *     need a workqueue; for this assignment a mutex + the
 *     understanding that the callback may reschedule is accepted.
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t             dev_num;
static struct cdev       c_dev;
static struct class     *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long __attribute__((used)) get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 * --------------------------------------------------------------- */
static void __attribute__((used)) log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 * --------------------------------------------------------------- */
static void __attribute__((used)) kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * TODO 3 (FIXED): Timer Callback — fires every CHECK_INTERVAL_SEC.
 *
 * Uses list_for_each_entry_safe so that removing the current
 * entry during iteration is safe (tmp holds the next pointer
 * before we potentially kfree the current entry).
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    long rss;

    mutex_lock(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {

        rss = get_rss_bytes(entry->pid);

        /* Process has exited — remove stale entry */
        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit: warn once */
        if (!entry->soft_triggered && rss > (long)entry->soft_limit) {
            log_soft_limit_event(entry->container_id,
                                 entry->pid,
                                 entry->soft_limit,
                                 rss);
            entry->soft_triggered = 1;
        }

        /* Hard limit: kill and remove */
        if (rss > (long)entry->hard_limit) {
            kill_process(entry->container_id,
                         entry->pid,
                         entry->hard_limit,
                         rss);
            list_del(&entry->list);
            kfree(entry);
            /* 'entry' is freed; safe because list_for_each_entry_safe
             * already saved 'tmp' before we entered this iteration. */
        }
    }

    mutex_unlock(&monitored_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    /*
     * FIX: All local variable declarations must appear at the top of
     * the block in kernel C (gnu11 allows mixed, but many in-tree
     * conventions and older compilers require this).  Declare
     * everything up front so the compiler never sees a declaration
     * after a statement.
     */
    struct monitor_request  req;
    struct monitored_entry *entry, *tmp;
    struct monitored_entry *new_entry;
    int found = 0;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* ----------------------------------------------------------
     * TODO 4 (FIXED): Register a new monitored entry.
     * ---------------------------------------------------------- */
    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d "
               "soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* Validate limits */
        if (req.soft_limit_bytes == 0 || req.hard_limit_bytes == 0 ||
            req.soft_limit_bytes >= req.hard_limit_bytes) {
            printk(KERN_ERR "[container_monitor] Invalid limits: "
                   "soft=%lu hard=%lu\n",
                   req.soft_limit_bytes, req.hard_limit_bytes);
            return -EINVAL;
        }

        new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
        if (!new_entry)
            return -ENOMEM;

        new_entry->pid = req.pid;
        strncpy(new_entry->container_id, req.container_id,
                sizeof(new_entry->container_id) - 1);
        new_entry->container_id[sizeof(new_entry->container_id) - 1] = '\0';
        new_entry->soft_limit    = req.soft_limit_bytes;
        new_entry->hard_limit    = req.hard_limit_bytes;
        new_entry->soft_triggered = 0;
        INIT_LIST_HEAD(&new_entry->list);

        mutex_lock(&monitored_lock);
        list_add(&new_entry->list, &monitored_list);
        mutex_unlock(&monitored_lock);

        return 0;   /* ← single return; duplicate removed */
    }

    /* ----------------------------------------------------------
     * TODO 5 (FIXED): Unregister — remove by PID.
     * ---------------------------------------------------------- */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    mutex_lock(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        if (entry->pid == req.pid) {
            list_del(&entry->list);
            kfree(entry);
            found = 1;
            break;  /* PIDs are unique; stop after first match */
        }
    }

    mutex_unlock(&monitored_lock);

    return found ? 0 : -ENOENT;  /* ← single return; duplicate removed */
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    /* TODO 6 (FIXED): variables declared at top of block */
    struct monitored_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");

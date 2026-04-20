
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container Memory Monitor");
MODULE_AUTHOR("Nanditha");

static struct class *cl = NULL;
static struct device *dev = NULL;
static dev_t dev_num;
static struct cdev c_dev;

/* ================= DEVICE ================= */
#define DEVICE_NAME "container_monitor"
static int major;

/* ================= IOCTL ================= */
#define MONITOR_MAGIC 'M'
#define MONITOR_REGISTER _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[32];
};

/* =========================================================
 * TODO 1: DEFINE LINKED LIST NODE
 * ========================================================= */
struct container_node {
    pid_t pid;                     // process ID
    char container_id[32];         // container name
    long soft_limit;               // soft memory limit
    long hard_limit;               // hard memory limit
    int warned;                    // soft warning flag

    struct list_head list;         // linked list pointer
};

/* =========================================================
 * TODO 2: GLOBAL LIST + LOCK
 * ========================================================= */
static LIST_HEAD(container_list);     // list of containers
static DEFINE_MUTEX(container_lock);  // mutex for safety

static struct timer_list monitor_timer;

/* ================= MEMORY ================= */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;

    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) return 0;

    mm = task->mm;
    if (!mm) return 0;

    return get_mm_rss(mm) << PAGE_SHIFT;
}

/* =========================================================
 * TODO 3: ADD CONTAINER
 * ========================================================= */
static void add_container(pid_t pid, const char *id, long soft, long hard)
{
    struct container_node *node;

    node = kmalloc(sizeof(*node), GFP_KERNEL);
    if (!node) return;

    node->pid = pid;
    strncpy(node->container_id, id, sizeof(node->container_id));
    node->soft_limit = soft;
    node->hard_limit = hard;
    node->warned = 0;

    mutex_lock(&container_lock);
    list_add(&node->list, &container_list);
    mutex_unlock(&container_lock);

    printk(KERN_INFO "Added container %s (PID %d)\n", id, pid);
}

/* =========================================================
 * TODO 4: REMOVE CONTAINER
 * ========================================================= */
static void remove_container(pid_t pid)
{
    struct container_node *node, *tmp;

    mutex_lock(&container_lock);

    list_for_each_entry_safe(node, tmp, &container_list, list) {
        if (node->pid == pid) {
            list_del(&node->list);
            printk(KERN_INFO "Removed container %s\n", node->container_id);
            kfree(node);
            break;
        }
    }

    mutex_unlock(&container_lock);
}

/* =========================================================
 * TODO 5: MONITOR LOGIC
 * ========================================================= */
static void monitor_containers(struct timer_list *t)
{
    struct container_node *node, *tmp;

    mutex_lock(&container_lock);

    list_for_each_entry_safe(node, tmp, &container_list, list) {

        struct task_struct *task;
        long mem;

        task = pid_task(find_vpid(node->pid), PIDTYPE_PID);

        /* REMOVE DEAD PROCESS */
        if (!task) {
            printk(KERN_INFO "Cleaning dead container %s\n", node->container_id);
            list_del(&node->list);
            kfree(node);
            continue;
        }

        mem = get_rss_bytes(node->pid);

        printk(KERN_INFO "%s using %ld bytes\n",
               node->container_id, mem);

        /* HARD LIMIT → KILL */
        if (mem > node->hard_limit) {
            printk(KERN_ALERT "Hard limit exceeded! Killing %s\n",
                   node->container_id);
            kill_pid(find_vpid(node->pid), SIGKILL, 1);
        }

        /* SOFT LIMIT → WARNING */
        else if (mem > node->soft_limit && !node->warned) {
            printk(KERN_WARNING "Soft limit exceeded for %s\n",
                   node->container_id);
            node->warned = 1;
        }
    }

    mutex_unlock(&container_lock);

    /* RUN AGAIN AFTER 1 SECOND */
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

/* ================= IOCTL ================= */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
   struct monitor_request info;

    if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
        return -EFAULT;

    switch (cmd) {
    case MONITOR_REGISTER:
        add_container(
            info.pid,
            info.container_id,
            info.soft_limit_bytes,
            info.hard_limit_bytes
        );
        break;

    case MONITOR_UNREGISTER:
        remove_container(info.pid);
        break;

    default:
        return -EINVAL;
}
    
          

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ================= INIT ================= */
static int __init monitor_init(void)
{
    printk(KERN_INFO "Monitor loaded\n");

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

    dev = device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
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

    timer_setup(&monitor_timer, monitor_containers, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));

    return 0;
}

/* =========================================================
 * TODO 6: CLEANUP
 * ========================================================= */
static void __exit monitor_exit(void)
{
    struct container_node *node, *tmp;

   if (timer_pending(&monitor_timer))
     timer_delete_sync(&monitor_timer);
    mutex_lock(&container_lock);

    list_for_each_entry_safe(node, tmp, &container_list, list) {
        list_del(&node->list);
        printk(KERN_INFO "Freeing %s\n", node->container_id);
        kfree(node);
    }

    mutex_unlock(&container_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "Monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

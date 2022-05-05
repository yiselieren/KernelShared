// Shared memory betwween kernel and user

#include <linux/version.h>
#include <linux/module.h>
#if defined(MODVERSIONS)
#include <linux/modversions.h>
#endif

#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <asm/io.h>

#include "shmdrv.h"

//#define VERBOSE_READ
//#define VERBOSE_WRITE
//#define VERBOSE_TIMER
//#define USE_CONTINUOUS_READ
#define MAX_DEV  1
#define DEV_NAME "mmapdrv"
#define SECS 1
#define WTMO_HZ  rand_range(2, 10)
#define TX_CHUNK rand_range(1, 17000)

struct FilePrivate {
    struct Data       data;
    struct timer_list timer;
    spinlock_t        rd_lock;
    spinlock_t        wr_lock;
    wait_queue_head_t wq;
    uint8_t           last_rd;
    uint8_t           last_wr;
} __attribute__((aligned(PAGE_SIZE)));

static int                do_not_restart_timer = 0;
static struct FilePrivate *fp_for_out = 0;
static int                dev_major;
static void write_to_user(struct FilePrivate *fp, int wr_len);

u32 rand_range(u32 low, u32 high)
{
    u32 rc = get_random_u32() % (high - low);
    return low + rc;
}

void mmapdrv_timer(struct timer_list *t)
{
    unsigned long next = WTMO_HZ;
#ifdef VERBOSE_TIMER
    printk("TIMER, next in %ld HZ", next);
#endif
    write_to_user(fp_for_out, TX_CHUNK);
    if (!do_not_restart_timer)
        mod_timer(t, jiffies + next);
}

/* device operations */
static int mmapdrv_open(struct inode *n, struct file *f)
{
    struct FilePrivate *fp;

    printk("mmapdrv_open");
    fp = (struct FilePrivate *)kmalloc(sizeof(struct FilePrivate) , GFP_KERNEL);
    f->private_data = (void*)fp;

    // Data
    fp->data.in.wp = fp->data.in.rp = 0;
    fp->data.out.wp = fp->data.out.rp = 0;
    fp->last_rd = fp->last_wr = 0;

    // Locks, queues
    spin_lock_init(&fp->rd_lock);
    spin_lock_init(&fp->wr_lock);
    init_waitqueue_head(&fp->wq);

    // Timer
    timer_setup(&fp->timer, mmapdrv_timer, 0);
    mod_timer(&fp->timer, jiffies + WTMO_HZ);
    do_not_restart_timer = 0;
    fp_for_out = (struct FilePrivate *)f->private_data;

    return 0;
}

static int mmapdrv_release(struct inode *n, struct file *f)
{
    do_not_restart_timer = 1;
    del_timer_sync(&((struct FilePrivate *)f->private_data)->timer);
    if (f->private_data)
        kfree(f->private_data);
    f->private_data = 0;
    return 0;
}

int mmapdrv_ioctl(struct inode *n, struct file *f, unsigned int cmd, unsigned long param)
{
    return 0;
}

static int mmapdrv_mmap(struct file *f, struct vm_area_struct *vm)
{
    struct FilePrivate *fp = (struct FilePrivate *)f->private_data;
    size_t             size = vm->vm_end - vm->vm_start;

    if (remap_pfn_range(vm, vm->vm_start, virt_to_phys(&fp->data) >> PAGE_SHIFT,
                        size, vm->vm_page_prot)) {
		printk("remap failed\n");
        return -EAGAIN;
    }
    return 0;
}

#ifdef USE_CONTINUOUS_READ

static void continuous_process_user_input(struct file *f, const volatile uint8_t *data, uint32_t len)
{
    uint8_t  *last_rd = &((struct FilePrivate *)f->private_data)->last_rd;
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (++*last_rd >= 255)
            *last_rd = 1;
        if (data[i] != *last_rd)
            printk("Read mismatch! 0x%02x expected, 0x%02x received\n", (*last_rd)&0xff, data[i]&0xff);
    }
}

#else

static void process_user_input(struct file *f)
{
    struct Data1 *data_in = &((struct FilePrivate *)f->private_data)->data.out;
    uint8_t      *last_rd = &((struct FilePrivate *)f->private_data)->last_rd;
    uint32_t     len = 0;
    for (;;) {
        uint8_t rd_char;
        if (data_in->rp == data_in->wp)
            // Empty
            break;
        rd_char = data_in->d[data_in->rp];
        len++;
#ifdef VERBOSE_READ
        if (data_in->rp + 1 >= LEN)
            printk("USER input, read buffer wraparound");
#endif
        data_in->rp = (data_in->rp + 1) % LEN;
        if (++*last_rd >= 255)
            *last_rd = 1;
        if (rd_char != *last_rd)
            printk("Read mismatch! 0x%02x expected, 0x%02x received\n", (*last_rd)&0xff, rd_char&0xff);
    }
#ifdef VERBOSE_READ
    printk("USER input, %d bytes received", len);
#endif
}

#endif /* USE_CONTINUOUS_READ */

static void write_to_user(struct FilePrivate *fp, int wr_len)
{
    struct Data1 *out_data = &fp->data.in;
    uint8_t      *last_wr = &fp->last_wr;
    uint32_t     wr_bytes = 0;
    uint32_t     next_idx;

    if (!fp)
        return;

    spin_lock(&fp->wr_lock);

    for (;;) {
        next_idx = (out_data->wp + 1) % LEN;
        if (next_idx == out_data->rp) {
            // Full
#ifdef VERBOSE_WRITE
            printk("USER output write buffer full!\n");
#endif
            break;
        }
        if (++*last_wr >= 255)
            *last_wr = 1;
        out_data->d[out_data->wp] = *last_wr;
        out_data->wp = next_idx;
        if (++wr_bytes >= wr_len)
            break;
    }

    wake_up_interruptible(&fp->wq);

    spin_unlock(&fp->wr_lock);
}

static void check_news_from_user(struct file *f)
{
    struct FilePrivate *fp = (struct FilePrivate *)f->private_data;

#ifdef USE_CONTINUOUS_READ
    // If we need continuous block processing
    struct Data1 *in_data = &fp->data.out;
    int saved_wp = in_data->wp;

    spin_lock(&fp->rd_lock);
    if (in_data->rp < saved_wp) {
        // Straight (one part)
        continuous_process_user_input(f, &in_data->d[in_data->rp], saved_wp - in_data->rp);
        in_data->rp = saved_wp;
    }
    else if (in_data->rp > saved_wp) {
        // Straight and wrap around (two parts)
        continuous_process_user_input(f, &in_data->d[in_data->rp], LEN - in_data->rp);
        continuous_process_user_input(f, &in_data->d[0], saved_wp);
        in_data->rp = saved_wp;
    }
    else
        ; // Buffer is empty
    spin_unlock(&fp->rd_lock);
#else
    spin_lock(&fp->rd_lock);
    process_user_input(f);
    spin_unlock(&fp->rd_lock);
#endif
}

static ssize_t mmapdrv_write(struct file *f, const char __user *buf, size_t count, loff_t *offset)
{
    check_news_from_user(f);
    return 0;
}

static ssize_t mmapdrv_read(struct file *f, char __user *buf, size_t count, loff_t *offset)
{
    struct FilePrivate *fp = (struct FilePrivate *)f->private_data;
    wait_event_interruptible(fp->wq, fp->data.in.rp != fp->data.in.wp);
    return 0;
}

static unsigned int mmapdrv_poll(struct file *f, struct poll_table_struct *ptable)
{
    struct FilePrivate *fp = (struct FilePrivate *)f->private_data;
	unsigned int       mask = 0;

	poll_wait(f, &fp->wq, ptable);

	spin_lock(&fp->rd_lock);

    if (fp->data.in.rp != fp->data.in.wp)
		mask |= (POLLIN | POLLRDNORM);

	spin_unlock(&fp->rd_lock);

	return(mask);
}

static struct file_operations mmapdrv_fops =
{
  .owner   = THIS_MODULE,
  .mmap    = mmapdrv_mmap,
  .open    = mmapdrv_open,
  .read    = mmapdrv_read,
  .poll    = mmapdrv_poll,
  .write   = mmapdrv_write,
  .release = mmapdrv_release
};


struct mmapdrv_device_data {
    struct cdev cdev;
};
static struct mmapdrv_device_data mmapdrv_data[MAX_DEV];
static struct class *mmapdrv_class = NULL;

static int mmapdrv_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

int init_module(void)
{
    int err, i;
    dev_t dev;

    err = alloc_chrdev_region(&dev, 0, MAX_DEV, DEV_NAME);
    dev_major = MAJOR(dev);

    mmapdrv_class = class_create(THIS_MODULE, DEV_NAME);
    mmapdrv_class->dev_uevent = mmapdrv_uevent;

    // Create necessary number of the devices
    for (i = 0; i < MAX_DEV; i++) {
        // init new device
        cdev_init(&mmapdrv_data[i].cdev, &mmapdrv_fops);
        mmapdrv_data[i].cdev.owner = THIS_MODULE;

        cdev_add(&mmapdrv_data[i].cdev, MKDEV(dev_major, i), 1);
        device_create(mmapdrv_class, NULL, MKDEV(dev_major, i), NULL, "mmapdrv%d", i);
    }

    printk("mmapdrv successful init!");
    return 0;
}

/* remove the module */
void cleanup_module(void)
{
    int i;

    for (i = 0; i < MAX_DEV; i++) {
        device_destroy(mmapdrv_class, MKDEV(dev_major, i));
    }

    class_destroy(mmapdrv_class);

    unregister_chrdev_region(MKDEV(dev_major, 0), MINORMASK);
    printk("mmapdrv remove");
    return;
}
MODULE_LICENSE("GPL");

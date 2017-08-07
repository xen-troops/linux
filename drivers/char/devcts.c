/*
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/timekeeping.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/device.h>

#include <linux/devcts.h>

MODULE_AUTHOR("Matthias Blankertz <matthias.blankertz@cetitec.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver cross-timestamping device");

#define NAME "devcts"

#define TIME_UPDATE_CYCLE_MS 100

struct cts {
	ktime_t systime;
	ktime_t devtime;
};

struct cts_dev {
	struct list_head ctsdevs;
	char *name;
	devcts_get_time_fn_t get_time_fn;
	void *get_time_ctx;
	spinlock_t time_lock;
	struct cts cts[2];
};

struct devcts_dev {
	dev_t devnum;
	struct cdev cdev;
	struct list_head ctsdevs;
	struct delayed_work work_update_times;
	struct mutex lock;
	struct class *class;
	struct device *dev;
};

static struct devcts_dev *dev = NULL;

static void devcts_update_times(struct work_struct *work)
{
	struct cts_dev *devit;

	rcu_read_lock();

	list_for_each_entry_rcu(devit, &dev->ctsdevs, ctsdevs) {
		struct cts newcts;
		if (devit->get_time_fn(&newcts.devtime, &newcts.systime,
							   devit->get_time_ctx) != 0) {
			dev_warn_ratelimited(dev->dev, "get_time_fn() failed for %s",
								 devit->name);
			continue;
		}

		spin_lock(&devit->time_lock);
		devit->cts[0] = devit->cts[1];
		devit->cts[1] = newcts;
		spin_unlock(&devit->time_lock);
	}

	rcu_read_unlock();

	schedule_delayed_work(&dev->work_update_times,
						  msecs_to_jiffies(TIME_UPDATE_CYCLE_MS));
}

static ktime_t devcts_interpolate_to_dev(struct cts_dev *ctsdev, ktime_t systime)
{
	struct cts cts[2];
	int64_t y;
	uint64_t y0, y1;
	ktime_t x = systime, x0, x1;

	spin_lock(&ctsdev->time_lock);
	cts[0] = ctsdev->cts[0];
	cts[1] = ctsdev->cts[1];
	spin_unlock(&ctsdev->time_lock);

	y0 = ktime_to_ns(cts[0].devtime);
	y1 = ktime_to_ns(cts[1].devtime);
	x0 = cts[0].systime;
	x1 = cts[1].systime;

	y = ktime_to_ns(ktime_sub(x, x0));

	y *= y1 - y0;

	y = div64_s64(y, (ktime_to_ns(ktime_sub(x1, x0))));

	y += y0;

	WARN_ON(y < 0);

	return ns_to_ktime(y);
}

static ktime_t devcts_interpolate_to_sys(struct cts_dev *ctsdev, ktime_t devtime)
{
	struct cts cts[2];
	int64_t y;
	uint64_t y0, y1;
	ktime_t x = devtime, x0, x1;

	spin_lock(&ctsdev->time_lock);
	cts[0] = ctsdev->cts[0];
	cts[1] = ctsdev->cts[1];
	spin_unlock(&ctsdev->time_lock);

	y0 = ktime_to_ns(cts[0].systime);
	y1 = ktime_to_ns(cts[1].systime);
	x0 = cts[0].devtime;
	x1 = cts[1].devtime;

	y = ktime_to_ns(ktime_sub(x, x0));

	y *= y1 - y0;

	y = div64_s64(y, (ktime_to_ns(ktime_sub(x1, x0))));

	y += y0;

	WARN_ON(y < 0);

	return ns_to_ktime(y);
}

/* Must be in RCU read-side critical section */
inline static struct cts_dev *_find_ctsdev(const char *name)
{
	struct cts_dev *ctsdev;
	list_for_each_entry_rcu(ctsdev, &dev->ctsdevs, ctsdevs) {
		if (strcmp(ctsdev->name, name) == 0) {
			return ctsdev;
		}
	}

	return NULL;
}

static int _get_name_user(char **name, const char __user *name_user)
{
	const size_t max_name_len = 128;
	long copied;

	*name = kmalloc(max_name_len, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(*name)) {
		return -ENOMEM;
	}

	copied = strncpy_from_user(*name, name_user, max_name_len);
	if (copied == 0 || copied == max_name_len) {
		kfree(*name);
		return -EFAULT;
	} else if (copied < 0) {
		kfree(*name);
		return copied;
	}

	return 0;
}


/* Interpolate the requested timestamp to another clock base */
static long devcts_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct devcts_req req;
	char *srcname = NULL, *dstname = NULL;
	struct cts_dev *srcdev = NULL, *dstdev = NULL;
	int err;
	ktime_t ts;

	if (cmd != DEVCTS_DEVTOSYS &&
		cmd != DEVCTS_SYSTODEV &&
		cmd != DEVCTS_DEVTODEV) {
		err = -ENOSYS;
		goto out_ret;
	}

	if(copy_from_user(&req, (void*)arg, sizeof(struct devcts_req)) != 0) {
		err = -EFAULT;
		goto out_ret;
	}
	
	if (cmd == DEVCTS_DEVTOSYS ||
		cmd == DEVCTS_DEVTODEV) {
		err = _get_name_user(&srcname, req.src_dev);
		if (err != 0)
			goto out_ret;
	}

	if (cmd == DEVCTS_SYSTODEV ||
		cmd == DEVCTS_DEVTODEV) {
		err = _get_name_user(&dstname, req.dst_dev);
		if (err != 0)
			goto out_srcname;
	}

	rcu_read_lock();
	if (srcname != NULL) {
		srcdev = _find_ctsdev(srcname);
		if (!srcdev) {
			err = -ENODEV;
			goto out_rcuunlock;
		}
	}

	if (dstname != NULL) {
		dstdev = _find_ctsdev(dstname);
		if (!dstdev) {
			err = -ENODEV;
			goto out_rcuunlock;
		}
	}
	
	ts = ns_to_ktime(req.src_ts);
	if ((cmd == DEVCTS_DEVTOSYS ||
		 cmd == DEVCTS_DEVTODEV) &&
		!WARN(!srcdev, "Logic error: srcdev is NULL")) {
		ts = devcts_interpolate_to_sys(srcdev, ts);
	}
	
	if ((cmd == DEVCTS_SYSTODEV ||
		 cmd == DEVCTS_DEVTODEV) &&
		!WARN(!dstdev, "Logic error: dstdev is NULL")) {
		ts = devcts_interpolate_to_dev(dstdev, ts);
	}
	rcu_read_unlock();

	req.dst_ts = ktime_to_ns(ts);

	if (copy_to_user((void*)arg, &req, sizeof(struct devcts_req)) != 0) {
		err = -EFAULT;
		goto out_dstname;
	}

	err = 0;
	goto out_dstname;
	
  out_rcuunlock:
	rcu_read_unlock();
	
  out_dstname:
	kfree(dstname);
  out_srcname:
	kfree(srcname);
  out_ret:
	return err;
}

/* Get a list of known devices */
static long devcts_read(struct file *file, char __user *data, size_t len, loff_t *off)
{
	ktime_t systime = ktime_get();
	char *buf = kmalloc(len, GFP_KERNEL);
	int spos;
	long ret = 0;
	struct cts_dev *devit;
	
	if (ZERO_OR_NULL_PTR(buf)) {
		return -ENOMEM;
	}

	spos = scnprintf(buf, len, "<system>\t%.20llu\n", ktime_to_ns(systime));

	rcu_read_lock();
	list_for_each_entry_rcu(devit, &dev->ctsdevs, ctsdevs) {
		ktime_t devtime = devcts_interpolate_to_dev(devit, systime);
		spos += scnprintf(buf+spos, len-spos, "%s\t%.20llu\n",
						  devit->name, ktime_to_ns(devtime));
	}
	rcu_read_unlock();

	if (*off < spos) {
		loff_t oldoff = *off;
		
		if (copy_to_user(data, buf+*off, spos-*off) != 0) {
			ret = -EFAULT;
			goto out;
		}

		*off = spos;
		
		ret = spos-oldoff;
	}

  out:
	kfree(buf);
	return ret;
}

static const struct file_operations devcts_fops =
{
	.owner = THIS_MODULE,
	.unlocked_ioctl = &devcts_ioctl,
	.read = &devcts_read
};

int devcts_register_device(const char *name, devcts_get_time_fn_t fn, void *ctx)
{
	struct cts_dev *ctsdev;
	int err;
	
	if (!name || !fn)
		return -EINVAL;

	ctsdev = kmalloc(sizeof(struct cts_dev), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(ctsdev))
		return -ENOMEM;

	ctsdev->name = kstrdup(name, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(ctsdev->name)) {
		err = -ENOMEM;
		goto out_alloc;
	}

	ctsdev->get_time_fn = fn;
	ctsdev->get_time_ctx = ctx;
	spin_lock_init(&ctsdev->time_lock);

	fn(&ctsdev->cts[0].devtime, &ctsdev->cts[0].systime, ctx);
	fn(&ctsdev->cts[1].devtime, &ctsdev->cts[1].systime, ctx);

	mutex_lock(&dev->lock);
	list_add_rcu(&ctsdev->ctsdevs, &dev->ctsdevs);
	mutex_unlock(&dev->lock);
	return 0;
	
  out_alloc:
	kfree(ctsdev);
	return err;
}
EXPORT_SYMBOL(devcts_register_device);

void devcts_unregister_device(const char *name)
{
	struct cts_dev *ctsdev;
	
	if (!name)
		return;

	mutex_lock(&dev->lock);
	list_for_each_entry(ctsdev, &dev->ctsdevs, ctsdevs) {
		if (strcmp(name, ctsdev->name) == 0) {
			list_del_rcu(&ctsdev->ctsdevs);
			synchronize_rcu();
			kfree(ctsdev->name);
			kfree(ctsdev);
			break;
		}
	}
	mutex_unlock(&dev->lock);
}
EXPORT_SYMBOL(devcts_unregister_device);

static int __init devcts_init(void)
{
	int err;

	dev = kmalloc(sizeof(struct devcts_dev), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(dev))
		return -ENOMEM;

	mutex_init(&dev->lock);
	INIT_LIST_HEAD(&dev->ctsdevs);
	INIT_DELAYED_WORK(&dev->work_update_times, devcts_update_times);

	dev->class = class_create(THIS_MODULE, "char");
	if (IS_ERR(dev->class)) {
		err = PTR_ERR(dev->class);
		goto out_alloc;
	}
	
	err = alloc_chrdev_region(&dev->devnum, 0, 1, NAME);
	if (err != 0) {
		goto out_class;
	}
	
	cdev_init(&dev->cdev, &devcts_fops);
	dev->cdev.owner = THIS_MODULE;

	err = cdev_add(&dev->cdev, dev->devnum, 1);
	if (err != 0) {
		goto out_region;
	}

	dev->dev = device_create(dev->class, NULL, dev->devnum, NULL, "cts");
	if (IS_ERR(dev->dev)) {
		err = PTR_ERR(dev->dev);
		goto out_cdev;
	}
	
	schedule_delayed_work(&dev->work_update_times,
						  msecs_to_jiffies(TIME_UPDATE_CYCLE_MS));
	
	return 0;

  out_cdev:
	cdev_del(&dev->cdev);
  out_region:
	unregister_chrdev_region(dev->devnum, 1);
  out_class:
	class_destroy(dev->class);
  out_alloc:
	mutex_destroy(&dev->lock);
	kfree(dev);
	return err;
}

static void __exit devcts_exit(void)
{
	struct cts_dev *ctsdev, *ctsdev2;
	cancel_delayed_work_sync(&dev->work_update_times);

	list_for_each_entry_safe(ctsdev, ctsdev2, &dev->ctsdevs, ctsdevs) {
		kfree(ctsdev->name);
		kfree(ctsdev);
	}

	device_destroy(dev->class, dev->devnum);
	class_destroy(dev->class);
	cdev_del(&dev->cdev);
	unregister_chrdev_region(dev->devnum, 1);
	mutex_destroy(&dev->lock);
	kfree(dev);
}

module_init(devcts_init);
module_exit(devcts_exit);

// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/miscdevice.h>

static unsigned int high_resolution_enable = 1;
module_param(high_resolution_enable, uint, 0644);
static unsigned int migt_debug;
module_param(migt_debug, uint, 0644);
static unsigned int migt_ms = 50;
module_param(migt_ms, uint, 0644);
static unsigned int migt_thresh = 18;
module_param(migt_thresh, uint, 0644);

static int set_migt_freq(const char *buf, const struct kernel_param *kp)
{
	return 0;
}

static int get_migt_freq(char *buf, const struct kernel_param *kp)
{
	return 0;
}

static const struct kernel_param_ops param_ops_migt_freq = {
	.set = set_migt_freq,
	.get = get_migt_freq,
};

module_param_cb(migt_freq, &param_ops_migt_freq, NULL, 0644);
module_param_cb(smart_ceiling_freq, &param_ops_migt_freq, NULL, 0644);

static int migt_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int migt_release(struct inode *ignored, struct file *file)
{
	return 0;
}

static int migt_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

static long migt_ioctl(struct file *fp, unsigned int cmd,
				 unsigned long arg)
{
	return 0;
}

static const struct file_operations migt_fops = {
	.owner = THIS_MODULE,
	.open = migt_open,
	.release = migt_release,
	.mmap = migt_mmap,
	.unlocked_ioctl = migt_ioctl,
};

static struct miscdevice migt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "migt",
	.fops = &migt_fops,
};

static int migt_init(void)
{
	misc_register(&migt_misc);
	return 0;
}

late_initcall(migt_init);

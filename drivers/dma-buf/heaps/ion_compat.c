// SPDX-License-Identifier: GPL-2.0
/*
 * ION Compatibility Shim Driver over DMA-BUF System Heap
 *
 * Bridges legacy Android ION userspace ioctls (ION_IOC_ALLOC, ION_IOC_MAP,
 * ION_IOC_FREE, ION_IOC_CUSTOM) to Linux DMA-BUF Heaps.
 *
 * Copyright (c) 2026 SavoryBanana / Google Pixel Mainline Project
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define ION_IOC_MAGIC 'I'

struct ion_allocation_data {
	size_t len;
	size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	int handle;
};

struct ion_fd_data {
	int handle;
	int fd;
};

struct ion_handle_data {
	int handle;
};

struct ion_custom_data {
	unsigned int cmd;
	unsigned long arg;
};

#define ION_IOC_ALLOC	_IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_FREE	_IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ION_IOC_MAP	_IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ION_IOC_SHARE	_IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)
#define ION_IOC_IMPORT	_IOWR(ION_IOC_MAGIC, 5, struct ion_fd_data)
#define ION_IOC_CUSTOM	_IOWR(ION_IOC_MAGIC, 6, struct ion_custom_data)
#define ION_IOC_SYNC	_IOWR(ION_IOC_MAGIC, 7, struct ion_fd_data)

struct ion_compat_client {
	struct idr idr;
	spinlock_t lock;
};

static int ion_compat_open(struct inode *inode, struct file *file)
{
	struct ion_compat_client *client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	idr_init(&client->idr);
	spin_lock_init(&client->lock);
	file->private_data = client;
	return 0;
}

static int ion_compat_release(struct inode *inode, struct file *file)
{
	struct ion_compat_client *client = file->private_data;
	struct dma_buf *dmabuf;
	int id;

	if (client) {
		idr_for_each_entry(&client->idr, dmabuf, id) {
			dma_buf_put(dmabuf);
		}
		idr_destroy(&client->idr);
		kfree(client);
	}
	return 0;
}

static long ion_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ion_compat_client *client = file->private_data;
	void __user *uarg = (void __user *)arg;

	if (!client)
		return -EBADF;

	switch (cmd) {
	case ION_IOC_ALLOC: {
		struct ion_allocation_data data;
		struct dma_heap *heap;
		struct dma_buf *dmabuf;
		int handle;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;

		heap = dma_heap_find("system-uncached");
		if (!heap)
			heap = dma_heap_find("system");
		if (!heap)
			return -ENODEV;

		dmabuf = dma_heap_buffer_alloc(heap, PAGE_ALIGN(data.len) + SZ_256K, O_RDWR | O_CLOEXEC, 0);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);

		idr_preload(GFP_KERNEL);
		spin_lock(&client->lock);
		handle = idr_alloc(&client->idr, dmabuf, 1, 0, GFP_ATOMIC);
		spin_unlock(&client->lock);
		idr_preload_end();

		if (handle < 0) {
			dma_buf_put(dmabuf);
			return handle;
		}

		data.handle = handle;
		if (copy_to_user(uarg, &data, sizeof(data))) {
			spin_lock(&client->lock);
			idr_remove(&client->idr, handle);
			spin_unlock(&client->lock);
			dma_buf_put(dmabuf);
			return -EFAULT;
		}
		return 0;
	}
	case ION_IOC_MAP:
	case ION_IOC_SHARE: {
		struct ion_fd_data data;
		struct dma_buf *dmabuf;
		int fd;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;

		spin_lock(&client->lock);
		dmabuf = idr_find(&client->idr, data.handle);
		if (dmabuf)
			get_dma_buf(dmabuf);
		spin_unlock(&client->lock);

		if (!dmabuf)
			return -EINVAL;

		fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (fd < 0) {
			dma_buf_put(dmabuf);
			return fd;
		}

		data.fd = fd;
		if (copy_to_user(uarg, &data, sizeof(data)))
			return -EFAULT;
		return 0;
	}
	case ION_IOC_FREE: {
		struct ion_handle_data data;
		struct dma_buf *dmabuf;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;

		spin_lock(&client->lock);
		dmabuf = idr_remove(&client->idr, data.handle);
		spin_unlock(&client->lock);

		if (!dmabuf)
			return -EINVAL;

		dma_buf_put(dmabuf);
		return 0;
	}
	case ION_IOC_IMPORT: {
		struct ion_fd_data data;
		struct dma_buf *dmabuf;
		int handle;

		if (copy_from_user(&data, uarg, sizeof(data)))
			return -EFAULT;

		dmabuf = dma_buf_get(data.fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);

		idr_preload(GFP_KERNEL);
		spin_lock(&client->lock);
		handle = idr_alloc(&client->idr, dmabuf, 1, 0, GFP_ATOMIC);
		spin_unlock(&client->lock);
		idr_preload_end();

		if (handle < 0) {
			dma_buf_put(dmabuf);
			return handle;
		}

		data.handle = handle;
		if (copy_to_user(uarg, &data, sizeof(data))) {
			spin_lock(&client->lock);
			idr_remove(&client->idr, handle);
			spin_unlock(&client->lock);
			dma_buf_put(dmabuf);
			return -EFAULT;
		}
		return 0;
	}
	case ION_IOC_SYNC:
	case ION_IOC_CUSTOM: {
		return 0; /* Cache maintenance / flush no-op */
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ion_compat_fops = {
	.owner = THIS_MODULE,
	.open = ion_compat_open,
	.release = ion_compat_release,
	.unlocked_ioctl = ion_compat_ioctl,
	.compat_ioctl = ion_compat_ioctl,
};

static struct miscdevice ion_compat_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &ion_compat_fops,
	.mode = 0666,
};

static int __init ion_compat_init(void)
{
	int ret = misc_register(&ion_compat_misc);
	if (ret) {
		pr_err("ion_compat: failed to register /dev/ion miscdevice: %d\n", ret);
		return ret;
	}
	pr_info("ion_compat: Android ION compatibility shim ready on /dev/ion (backed by DMA-BUF system heap)\n");
	return 0;
}

static void __exit ion_compat_exit(void)
{
	misc_deregister(&ion_compat_misc);
}

late_initcall(ion_compat_init);
module_exit(ion_compat_exit);

MODULE_AUTHOR("SavoryBanana / Google Pixel Mainline Project");
MODULE_DESCRIPTION("Android ION compatibility shim driver backed by DMA-BUF system heap");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_IMPORT_NS("DMA_BUF_HEAP");


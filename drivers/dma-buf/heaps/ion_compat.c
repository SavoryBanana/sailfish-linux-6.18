// SPDX-License-Identifier: GPL-2.0
/*
 * ION Compatibility Shim Driver over DMA-BUF System Heap
 *
 * Bridges legacy Android ION userspace ioctls (ION_IOC_ALLOC, ION_IOC_MAP,
 * ION_IOC_FREE, ION_IOC_CUSTOM, ION_IOC_SHARE, ION_IOC_IMPORT) to Linux DMA-BUF Heaps.
 *
 * Supports both legacy Android 3.18 kernel ION ABIs (handle-based, 28/32-byte structs)
 * and modern Android 8+ kernel ION ABIs (fd-based, 24-byte structs).
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

/* Modern ION allocation data (24 bytes, uapi/linux/ion.h) */
struct ion_allocation_data_v2 {
	__u64 len;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 fd;
	__u32 unused;
};

/* Legacy ION allocation data (32 bytes on 64-bit with 4-byte padding) */
struct ion_allocation_data_legacy {
	size_t len;
	size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	int handle;
};

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
	pr_info("ion_compat: /dev/ion opened by pid %d (%s)\n", current->pid, current->comm);
	return 0;
}

static int ion_compat_release(struct inode *inode, struct file *file)
{
	struct ion_compat_client *client = file->private_data;
	struct dma_buf *dmabuf;
	int id;

	pr_info("ion_compat: /dev/ion closed by pid %d (%s)\n", current->pid, current->comm);
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
	unsigned int dir = _IOC_DIR(cmd);
	unsigned int type = _IOC_TYPE(cmd);
	unsigned int nr = _IOC_NR(cmd);
	unsigned int size = _IOC_SIZE(cmd);

	if (!client) {
		pr_err("ion_compat: ioctl with NULL client from pid %d (%s)\n",
		       current->pid, current->comm);
		return -EBADF;
	}

	if (type != ION_IOC_MAGIC) {
		pr_warn("ion_compat: non-ION ioctl magic 0x%02x from pid %d (%s)\n",
			type, current->pid, current->comm);
		return -ENOTTY;
	}

	pr_info("ion_compat: ioctl cmd=0x%08x (nr=%u dir=%u size=%u) from pid %d (%s)\n",
		cmd, nr, dir, size, current->pid, current->comm);

	switch (nr) {
	case 0: { /* ION_IOC_ALLOC */
		struct dma_heap *heap;
		struct dma_buf *dmabuf;
		size_t len = 0;
		size_t align = 0;
		unsigned int heap_id_mask = 0;
		unsigned int flags = 0;
		bool is_v2 = false;

		if (size == sizeof(struct ion_allocation_data_v2)) {
			struct ion_allocation_data_v2 d2;
			if (copy_from_user(&d2, uarg, sizeof(d2)))
				return -EFAULT;
			len = d2.len;
			heap_id_mask = d2.heap_id_mask;
			flags = d2.flags;
			is_v2 = true;
		} else {
			struct ion_allocation_data_legacy d1;
			size_t copy_sz = min_t(size_t, (size_t)size, sizeof(d1));
			memset(&d1, 0, sizeof(d1));
			if (copy_from_user(&d1, uarg, copy_sz))
				return -EFAULT;
			len = d1.len;
			align = d1.align;
			heap_id_mask = d1.heap_id_mask;
			flags = d1.flags;
		}

		pr_info("ion_compat: ALLOC request: len=%zu align=%zu heap_mask=0x%x flags=0x%x (v2=%d)\n",
			len, align, heap_id_mask, flags, is_v2);

		if (!len)
			return -EINVAL;

		heap = dma_heap_find("system-uncached");
		if (!heap)
			heap = dma_heap_find("system");
		if (!heap) {
			pr_err("ion_compat: no DMA-BUF system heap available!\n");
			return -ENODEV;
		}

		dmabuf = dma_heap_buffer_alloc(heap, PAGE_ALIGN(len) + SZ_256K, O_RDWR | O_CLOEXEC, 0);
		if (IS_ERR(dmabuf)) {
			pr_err("ion_compat: dma_heap_buffer_alloc failed: %ld\n", PTR_ERR(dmabuf));
			return PTR_ERR(dmabuf);
		}

		if (is_v2) {
			int fd = dma_buf_fd(dmabuf, O_CLOEXEC);
			if (fd < 0) {
				dma_buf_put(dmabuf);
				pr_err("ion_compat: dma_buf_fd failed: %d\n", fd);
				return fd;
			}
			struct ion_allocation_data_v2 d2;
			memset(&d2, 0, sizeof(d2));
			d2.len = len;
			d2.heap_id_mask = heap_id_mask;
			d2.flags = flags;
			d2.fd = fd;
			if (copy_to_user(uarg, &d2, sizeof(d2))) {
				dma_buf_put(dmabuf);
				return -EFAULT;
			}
			pr_info("ion_compat: ALLOC success: len=%zu -> fd=%d\n", len, fd);
			return 0;
		} else {
			int handle;
			idr_preload(GFP_KERNEL);
			spin_lock(&client->lock);
			handle = idr_alloc(&client->idr, dmabuf, 1, 0, GFP_ATOMIC);
			spin_unlock(&client->lock);
			idr_preload_end();

			if (handle < 0) {
				dma_buf_put(dmabuf);
				pr_err("ion_compat: idr_alloc failed: %d\n", handle);
				return handle;
			}

			/* In 64-bit legacy ION, handle is at offset 24 (whether int or long) */
			if (size >= 28) {
				if (copy_to_user((char __user *)uarg + 24, &handle, sizeof(handle))) {
					spin_lock(&client->lock);
					idr_remove(&client->idr, handle);
					spin_unlock(&client->lock);
					dma_buf_put(dmabuf);
					return -EFAULT;
				}
			}
			pr_info("ion_compat: ALLOC success: len=%zu -> handle=%d\n", len, handle);
			return 0;
		}
	}
	case 1: { /* ION_IOC_FREE */
		int handle = 0;
		struct dma_buf *dmabuf;

		if (size >= sizeof(unsigned long)) {
			unsigned long h64 = 0;
			if (copy_from_user(&h64, uarg, sizeof(h64)))
				return -EFAULT;
			handle = (int)h64;
		} else {
			if (copy_from_user(&handle, uarg, sizeof(handle)))
				return -EFAULT;
		}

		spin_lock(&client->lock);
		dmabuf = idr_remove(&client->idr, handle);
		spin_unlock(&client->lock);

		if (!dmabuf) {
			pr_warn("ion_compat: FREE: handle %d not found\n", handle);
			return -EINVAL;
		}

		dma_buf_put(dmabuf);
		pr_info("ion_compat: FREE: handle %d freed\n", handle);
		return 0;
	}
	case 2:   /* ION_IOC_MAP */
	case 4: { /* ION_IOC_SHARE */
		int handle = 0;
		int fd = -1;
		struct dma_buf *dmabuf;
		size_t fd_offset = (size >= 16) ? 8 : 4;

		if (size >= 16) {
			unsigned long h64 = 0;
			if (copy_from_user(&h64, uarg, sizeof(h64)))
				return -EFAULT;
			handle = (int)h64;
		} else {
			if (copy_from_user(&handle, uarg, sizeof(handle)))
				return -EFAULT;
		}

		spin_lock(&client->lock);
		dmabuf = idr_find(&client->idr, handle);
		if (dmabuf)
			get_dma_buf(dmabuf);
		spin_unlock(&client->lock);

		if (!dmabuf) {
			pr_err("ion_compat: MAP/SHARE: handle %d not found!\n", handle);
			return -EINVAL;
		}

		fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (fd < 0) {
			dma_buf_put(dmabuf);
			pr_err("ion_compat: MAP/SHARE: dma_buf_fd failed: %d\n", fd);
			return fd;
		}

		if (copy_to_user((char __user *)uarg + fd_offset, &fd, sizeof(fd)))
			return -EFAULT;

		pr_info("ion_compat: MAP/SHARE: handle %d -> fd %d\n", handle, fd);
		return 0;
	}
	case 5: { /* ION_IOC_IMPORT */
		int fd = -1;
		int handle;
		struct dma_buf *dmabuf;
		size_t fd_offset = (size >= 16) ? 8 : 4;

		if (copy_from_user(&fd, (char __user *)uarg + fd_offset, sizeof(fd)))
			return -EFAULT;

		if (fd < 0 && fd_offset != 0) {
			int alt_fd = -1;
			if (copy_from_user(&alt_fd, uarg, sizeof(alt_fd)) == 0 && alt_fd >= 0)
				fd = alt_fd;
		}

		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			pr_err("ion_compat: IMPORT: dma_buf_get(fd=%d) failed: %ld\n", fd, PTR_ERR(dmabuf));
			return PTR_ERR(dmabuf);
		}

		idr_preload(GFP_KERNEL);
		spin_lock(&client->lock);
		handle = idr_alloc(&client->idr, dmabuf, 1, 0, GFP_ATOMIC);
		spin_unlock(&client->lock);
		idr_preload_end();

		if (handle < 0) {
			dma_buf_put(dmabuf);
			pr_err("ion_compat: IMPORT: idr_alloc failed: %d\n", handle);
			return handle;
		}

		if (size >= 16) {
			unsigned long h64 = handle;
			if (copy_to_user(uarg, &h64, sizeof(h64)))
				return -EFAULT;
		} else {
			if (copy_to_user(uarg, &handle, sizeof(handle)))
				return -EFAULT;
		}

		pr_info("ion_compat: IMPORT: fd %d -> handle %d\n", fd, handle);
		return 0;
	}
	case 6:   /* ION_IOC_CUSTOM */
	case 7: { /* ION_IOC_SYNC */
		pr_info("ion_compat: SYNC/CUSTOM (nr=%u) no-op\n", nr);
		return 0;
	}
	default:
		pr_warn("ion_compat: unhandled ioctl nr=%u (cmd=0x%08x)\n", nr, cmd);
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

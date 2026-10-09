// SPDX-License-Identifier: GPL-2.0-only
/* EF67 stock avclog shared buffer and avcsync message queues. */

#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#define AVCSYNC_CLIENTS		256
#define AVCSYNC_MESSAGE_MAX	128
#define AVCSYNC_QUEUE_MAX	8192
#define AVCSYNC_INIT		1000
#define AVCSYNC_GET_ID		1001
#define AVCLOG_BUFFER_SIZE	1024
#define AVCLOG_WRITE_OFFSET	16

struct avcsync_client {
	u32 id;
	pid_t opener_tgid;
	wait_queue_head_t wait;
	u8 *buffer;
	u32 capacity;
	u32 message_limit;
	u32 messages;
	u32 readers_waiting;
	u32 used;
	u32 read;
	u32 write;
	bool closing;
};

struct avclog_file {
	struct mutex lock;
	bool holds_semaphore;
};

static DEFINE_SPINLOCK(avcsync_lock);
static struct avcsync_client *avcsync_clients[AVCSYNC_CLIENTS];
static u32 avcsync_generation;
static u32 avcsync_slot;
static u32 avclog_client_id = ~0U;
static DEFINE_SEMAPHORE(avclog_semaphore);
static void *avclog_buffer;
static dma_addr_t avclog_dma;

/* All ring positions, message counts and client lookups use avcsync_lock. */
static int avcsync_enqueue(struct avcsync_client *client,
			   const u8 *message, size_t size)
{
	size_t i;

	if (!client || !client->buffer || client->closing)
		return -ENOSPC;
	if (client->messages == client->message_limit ||
	    size + 1 > client->capacity - client->used - 1)
		return -EAGAIN;

	client->buffer[client->write] = size;
	client->write = (client->write + 1) % client->capacity;
	for (i = 0; i < size; i++) {
		client->buffer[client->write] = message[i];
		client->write = (client->write + 1) % client->capacity;
	}
	client->messages++;
	client->used += size + 1;
	wake_up_interruptible(&client->wait);
	return size;
}

static int avcsync_open(struct inode *inode, struct file *file)
{
	struct avcsync_client *client;
	unsigned long flags;
	u32 slot;
	int i;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	init_waitqueue_head(&client->wait);
	client->opener_tgid = task_tgid_vnr(current);
	spin_lock_irqsave(&avcsync_lock, flags);
	for (i = 0; i < AVCSYNC_CLIENTS; i++) {
		slot = (avcsync_slot + 1 + i) & (AVCSYNC_CLIENTS - 1);
		if (!avcsync_clients[slot]) {
			avcsync_slot = slot;
			avcsync_generation += 0x10000;
			client->id = avcsync_generation | slot;
			avcsync_clients[slot] = client;
			break;
		}
	}
	spin_unlock_irqrestore(&avcsync_lock, flags);
	if (i == AVCSYNC_CLIENTS) {
		kfree(client);
		return -EMFILE;
	}
	file->private_data = client;
	return nonseekable_open(inode, file);
}

static int avcsync_release(struct inode *inode, struct file *file)
{
	struct avcsync_client *client = file->private_data;
	unsigned long flags;

	spin_lock_irqsave(&avcsync_lock, flags);
	avcsync_clients[client->id & (AVCSYNC_CLIENTS - 1)] = NULL;
	spin_unlock_irqrestore(&avcsync_lock, flags);
	kfree(client->buffer);
	kfree(client);
	return 0;
}

static long avcsync_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct avcsync_client *client = file->private_data;
	void __user *user = (void __user *)arg;
	unsigned long flags;
	u32 config[2], capacity;
	u8 *buffer;
	int ret = 0;

	if (cmd == AVCSYNC_GET_ID)
		return put_user(client->id, (u32 __user *)user);
	if (cmd != AVCSYNC_INIT)
		return -EINVAL;
	if (copy_from_user(config, user, sizeof(config)))
		return -EFAULT;
	/* Stock takes { maximum messages, payload bytes }, plus length bytes. */
	if (!config[0] || config[0] > 8000 || config[1] > 8000)
		return -EINVAL;
	capacity = config[0] + config[1] + 1;
	if (capacity > AVCSYNC_QUEUE_MAX)
		return -EINVAL;
	buffer = kzalloc(capacity, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	spin_lock_irqsave(&avcsync_lock, flags);
	if (client->buffer) {
		ret = -EEXIST;
	} else {
		client->capacity = capacity;
		client->message_limit = config[0];
		client->buffer = buffer;
		buffer = NULL;
	}
	spin_unlock_irqrestore(&avcsync_lock, flags);
	kfree(buffer);
	return ret;
}

static ssize_t avcsync_write(struct file *file, const char __user *user,
			    size_t size, loff_t *offset)
{
	u8 message[AVCSYNC_MESSAGE_MAX];
	unsigned long flags;
	int ret;

	if (!size)
		return 0;
	if (size > sizeof(message))
		return -EINVAL;
	if (copy_from_user(message, user, size))
		return -EFAULT;
	spin_lock_irqsave(&avcsync_lock, flags);
	ret = avcsync_enqueue(file->private_data, message, size);
	spin_unlock_irqrestore(&avcsync_lock, flags);
	return ret;
}

static ssize_t avcsync_read(struct file *file, char __user *user,
			   size_t size, loff_t *offset)
{
	struct avcsync_client *client = file->private_data;
	u8 message[AVCSYNC_MESSAGE_MAX];
	unsigned long flags;
	u32 length, copied, i;
	int ret;

	if (!size)
		return 0;
	if (size > sizeof(message))
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&avcsync_lock, flags);
		if (!client->buffer || client->closing) {
			spin_unlock_irqrestore(&avcsync_lock, flags);
			return -EBADF;
		}
		if (client->messages)
			break;
		if (file->f_flags & O_NONBLOCK) {
			spin_unlock_irqrestore(&avcsync_lock, flags);
			return -EAGAIN;
		}
		client->readers_waiting++;
		spin_unlock_irqrestore(&avcsync_lock, flags);
		ret = wait_event_interruptible(client->wait,
			ACCESS_ONCE(client->messages) ||
			ACCESS_ONCE(client->closing));
		spin_lock_irqsave(&avcsync_lock, flags);
		client->readers_waiting--;
		spin_unlock_irqrestore(&avcsync_lock, flags);
		if (ret)
			return ret;
	}
	length = client->buffer[client->read];
	client->read = (client->read + 1) % client->capacity;
	copied = min_t(u32, length, size);
	/* Like stock, a short read consumes the whole message. */
	for (i = 0; i < length; i++) {
		if (i < copied)
			message[i] = client->buffer[client->read];
		client->read = (client->read + 1) % client->capacity;
	}
	client->messages--;
	client->used -= length + 1;
	spin_unlock_irqrestore(&avcsync_lock, flags);
	return copy_to_user(user, message, copied) ? -EFAULT : copied;
}

static unsigned int avcsync_poll(struct file *file, poll_table *wait)
{
	struct avcsync_client *client = file->private_data;
	unsigned long flags;
	unsigned int mask = POLLOUT | POLLWRNORM;

	poll_wait(file, &client->wait, wait);
	spin_lock_irqsave(&avcsync_lock, flags);
	if (client->messages)
		mask |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&avcsync_lock, flags);
	return mask;
}

static int avcsync_flush(struct file *file, fl_owner_t owner)
{
	struct avcsync_client *client = file->private_data;
	unsigned long flags;

	spin_lock_irqsave(&avcsync_lock, flags);
	if (task_tgid_vnr(current) == client->opener_tgid &&
	    client->readers_waiting) {
		client->closing = true;
		wake_up_interruptible(&client->wait);
	}
	spin_unlock_irqrestore(&avcsync_lock, flags);
	return 0;
}

static const struct file_operations avcsync_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.open = avcsync_open,
	.release = avcsync_release,
	.flush = avcsync_flush,
	.read = avcsync_read,
	.write = avcsync_write,
	.poll = avcsync_poll,
	.unlocked_ioctl = avcsync_ioctl,
};

static int avclog_open(struct inode *inode, struct file *file)
{
	struct avclog_file *log;

	log = kzalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	mutex_init(&log->lock);
	file->private_data = log;
	return nonseekable_open(inode, file);
}

static int avclog_release(struct inode *inode, struct file *file)
{
	struct avclog_file *log = file->private_data;

	/* A dying daemon must not leave a userspace-held semaphore locked. */
	if (log->holds_semaphore)
		up(&avclog_semaphore);
	kfree(log);
	return 0;
}

static int avclog_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != PAGE_SIZE ||
	    !(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	/* mmap(1024) is page rounded; the entire dedicated page is zeroed. */
	return dma_mmap_coherent(NULL, vma, avclog_buffer, avclog_dma, PAGE_SIZE);
}

static long avclog_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct avclog_file *log = file->private_data;
	u32 id;
	int ret = 0;

	if (cmd == 0) {
		if (get_user(id, (u32 __user *)arg))
			return -EFAULT;
		ACCESS_ONCE(avclog_client_id) = id;
		return 0;
	}
	if (cmd == 2)
		return 0; /* Stock diagnostic command has no state change. */
	if (cmd != 1 && cmd != 3 && cmd != 4)
		return -EINVAL;
	if (mutex_lock_interruptible(&log->lock))
		return -ERESTARTSYS;
	if (cmd == 4) {
		if (!log->holds_semaphore) {
			ret = -EINVAL;
		} else {
			log->holds_semaphore = false;
			up(&avclog_semaphore);
		}
	} else if (cmd == 3 && log->holds_semaphore) {
		ret = -EBUSY;
	} else {
		if (!log->holds_semaphore) {
			ret = down_interruptible(&avclog_semaphore);
			if (ret)
				goto out;
		}
		if (cmd == 3) {
			log->holds_semaphore = true;
		} else {
			*(u32 *)(avclog_buffer + AVCLOG_WRITE_OFFSET) = 0;
			if (!log->holds_semaphore)
				up(&avclog_semaphore);
		}
	}
out:
	mutex_unlock(&log->lock);
	return ret;
}

static ssize_t avclog_read(struct file *file, char __user *user,
			  size_t size, loff_t *offset)
{
	struct avcsync_client *client;
	const u32 notice[2] = { 0, 0 };
	unsigned long flags;
	u32 id = ACCESS_ONCE(avclog_client_id);

	/* Stock read sends a flush notice instead of returning buffer bytes. */
	spin_lock_irqsave(&avcsync_lock, flags);
	client = avcsync_clients[id & (AVCSYNC_CLIENTS - 1)];
	if (client && client->id == id)
		avcsync_enqueue(client, (const u8 *)notice, sizeof(notice));
	spin_unlock_irqrestore(&avcsync_lock, flags);
	return 0;
}

static ssize_t avclog_write(struct file *file, const char __user *user,
			   size_t size, loff_t *offset)
{
	return 0; /* The stock daemon writes through mmap, not write(2). */
}

static const struct file_operations avclog_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.open = avclog_open,
	.release = avclog_release,
	.read = avclog_read,
	.write = avclog_write,
	.mmap = avclog_mmap,
	.unlocked_ioctl = avclog_ioctl,
};

static struct miscdevice avclog_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "avclog",
	.fops = &avclog_fops,
	.mode = 0600,
};

static struct miscdevice avcsync_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "avcsync",
	.fops = &avcsync_fops,
	.mode = 0600,
};

static int __init ef67_avclog_init(void)
{
	int ret;

	BUILD_BUG_ON(AVCLOG_BUFFER_SIZE > PAGE_SIZE);
	/* Built in: the allocation remains valid after close and across forks. */
	avclog_buffer = dma_alloc_coherent(NULL, PAGE_SIZE, &avclog_dma, GFP_KERNEL);
	if (!avclog_buffer)
		return -ENOMEM;
	memset(avclog_buffer, 0, PAGE_SIZE);
	ret = misc_register(&avcsync_device);
	if (ret)
		goto free_buffer;
	ret = misc_register(&avclog_device);
	if (!ret)
		return 0;
	misc_deregister(&avcsync_device);
free_buffer:
	dma_free_coherent(NULL, PAGE_SIZE, avclog_buffer, avclog_dma);
	return ret;
}
device_initcall(ef67_avclog_init);

/* SPDX-License-Identifier: GPL-2.0 */
/* Diagnostic snapshots from process context, never from the printk/panic path. */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/statfs.h>
#include <linux/vmalloc.h>

#define SNAPSHOT_SIZE (512 * 1024)
#define CACHE_RESERVE (8 * 1024 * 1024)

static int cache_snapshot(struct path *cache, const char *buffer, size_t length,
			 unsigned int slot)
{
	struct file *file;
	struct kstatfs stats;
	char name[48];
	loff_t position = 0;
	ssize_t written;
	int ret;

	/* Do not write into the rootfs directory underneath an absent mount. */
	if (cache->dentry != cache->mnt->mnt_root ||
	    cache->dentry->d_sb->s_magic != EXT4_SUPER_MAGIC ||
	    __mnt_is_readonly(cache->mnt))
		return -EROFS;
	ret = vfs_statfs(cache, &stats);
	if (ret)
		return ret;
	if (!stats.f_bsize || stats.f_bavail <
	    (CACHE_RESERVE + SNAPSHOT_SIZE) / stats.f_bsize + 1)
		return -ENOSPC;

	snprintf(name, sizeof(name), "/cache/kernel-latest-%u.log", slot);
	file = filp_open(name, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (IS_ERR(file))
		return PTR_ERR(file);
	/* Recheck after opening: reject mount races, hardlinks and special files. */
	if (file->f_path.mnt != cache->mnt ||
	    file->f_path.dentry->d_parent != cache->dentry ||
	    !S_ISREG(file_inode(file)->i_mode) ||
	    file_inode(file)->i_nlink != 1) {
		ret = -EINVAL;
		goto close;
	}
	while (position < length) {
		written = kernel_write(file, buffer + position,
				       length - position, position);
		if (written <= 0) {
			ret = written ? written : -EIO;
			goto close;
		}
		position += written;
	}
	ret = vfs_truncate(&file->f_path, length);
	if (!ret)
		ret = vfs_fsync(file, 0);
close:
	filp_close(file, NULL);
	return ret;
}

static int cache_kmsg_thread(void *unused)
{
	struct kmsg_dumper dumper = { .active = true };
	struct path cache;
	char *buffer;
	size_t length;
	u64 saved_seq = 0;
	u64 snapshot_seq;
	unsigned int slot = 0;
	bool saved = false;
	int ret;

	buffer = vzalloc(SNAPSHOT_SIZE);
	if (!buffer)
		return -ENOMEM;
	while (!kthread_should_stop()) {
		kmsg_dump_rewind(&dumper);
		snapshot_seq = dumper.next_seq;
		if ((!saved || dumper.next_seq != saved_seq) &&
		    kmsg_dump_get_buffer(&dumper, true, buffer,
					 SNAPSHOT_SIZE, &length) && length &&
		    !kern_path("/cache", LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &cache)) {
			ret = cache_snapshot(&cache, buffer, length, slot);
			path_put(&cache);
			if (!ret) {
				saved_seq = snapshot_seq;
				slot ^= 1;
				saved = true;
			}
		}
		msleep_interruptible(2000);
	}
	vfree(buffer);
	return 0;
}

static int __init cache_kmsg_init(void)
{
	struct task_struct *task;

	task = kthread_run(cache_kmsg_thread, NULL, "cache_kmsg");
	return IS_ERR(task) ? PTR_ERR(task) : 0;
}
late_initcall(cache_kmsg_init);

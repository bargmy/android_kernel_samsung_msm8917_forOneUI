/* SPDX-License-Identifier: GPL-2.0 */
/* Diagnostic snapshots from process context, never from the printk/panic path. */
#include <linux/delay.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
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

/* J250F cache only. Never probe userdata or guess another partition. */
static struct vfsmount *cache_mount(void)
{
	static const char * const devices[] = {
		/* Relative to this worker's private devtmpfs root. */
		"/mmcblk0p46",
	};
	struct file_system_type *type;
	struct vfsmount *mnt;
	struct path device;
	unsigned int i;
	int error = -ENOENT;

	type = get_fs_type("ext4");
	if (!type)
		return ERR_PTR(-ENODEV);
	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		if (kern_path(devices[i], LOOKUP_FOLLOW, &device))
			continue;
		if (!S_ISBLK(device.dentry->d_inode->i_mode)) {
			path_put(&device);
			continue;
		}
		path_put(&device);
		/* Private VFS mount, not a namespace mount over Android's /cache.
		 * ext4 reuses the superblock if Android already mounted this bdev.
		 * Default journal recovery is intentional; never format or force it.
		 */
		mnt = vfs_kern_mount(type, MS_NOSUID | MS_NODEV | MS_NOEXEC,
				     devices[i], NULL);
		if (!IS_ERR(mnt)) {
			put_filesystem(type);
			return mnt;
		}
		error = PTR_ERR(mnt);
		break;
	}
	put_filesystem(type);
	return ERR_PTR(error);
}

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

	snprintf(name, sizeof(name), "kernel-latest-%u.log", slot);
	file = file_open_root(cache->dentry, cache->mnt, name,
			      O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
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
	struct vfsmount *mnt = NULL;
	struct vfsmount *devices;
	struct file_system_type *devtype;
	struct path device_root;
	char *buffer;
	size_t length;
	u64 saved_seq = 0;
	u64 snapshot_seq;
	unsigned int slot = 0;
	bool saved = false;
	int ret;
	int previous_error = 0;

	/* Do not depend on ueventd making /dev/block nodes, or change init's
	 * root/mount namespace. devtmpfs creates partition nodes in-kernel.
	 */
	ret = unshare_fs_struct();
	if (ret)
		return ret;
	devtype = get_fs_type("devtmpfs");
	if (!devtype)
		return -ENODEV;
	devices = vfs_kern_mount(devtype, MS_NOSUID | MS_NOEXEC,
				 "cache-kmsg-devices", NULL);
	put_filesystem(devtype);
	if (IS_ERR(devices)) {
		pr_err("cache_kmsg: private devtmpfs failed: %ld\n",
		       PTR_ERR(devices));
		return PTR_ERR(devices);
	}
	device_root.mnt = devices;
	device_root.dentry = devices->mnt_root;
	set_fs_root(current->fs, &device_root);
	set_fs_pwd(current->fs, &device_root);
	pr_info("cache_kmsg: kernel-owned device view ready\n");
	buffer = vzalloc(SNAPSHOT_SIZE);
	if (!buffer) {
		mntput(devices);
		return -ENOMEM;
	}
	while (!kthread_should_stop()) {
		if (!mnt) {
			mnt = cache_mount();
			if (IS_ERR(mnt)) {
				ret = PTR_ERR(mnt);
				mnt = NULL;
				if (ret != previous_error) {
					pr_warn("cache_kmsg: cache mount pending/failed: %d\n", ret);
					previous_error = ret;
				}
				msleep_interruptible(250);
				continue;
			}
			cache.mnt = mnt;
			cache.dentry = mnt->mnt_root;
			pr_info("cache_kmsg: private cache mount ready\n");
		}
		kmsg_dump_rewind(&dumper);
		snapshot_seq = dumper.next_seq;
		if ((!saved || dumper.next_seq != saved_seq) &&
		    kmsg_dump_get_buffer(&dumper, true, buffer,
					 SNAPSHOT_SIZE, &length) && length) {
			ret = cache_snapshot(&cache, buffer, length, slot);
			if (!ret) {
				saved_seq = snapshot_seq;
				slot ^= 1;
				saved = true;
			}
			if (ret != previous_error) {
				if (ret)
					pr_warn("cache_kmsg: snapshot failed: %d\n", ret);
				else
					pr_info("cache_kmsg: snapshot saved\n");
				previous_error = ret;
			}
		}
		msleep_interruptible(2000);
	}
	if (mnt)
		mntput(mnt);
	mntput(devices);
	vfree(buffer);
	return 0;
}

static int __init cache_kmsg_init(void)
{
	struct task_struct *task;

	task = kthread_run(cache_kmsg_thread, NULL, "cache_kmsg");
	return IS_ERR(task) ? PTR_ERR(task) : 0;
}
fs_initcall_sync(cache_kmsg_init);

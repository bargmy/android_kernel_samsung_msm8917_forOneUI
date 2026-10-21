/* SPDX-License-Identifier: GPL-2.0 */
/* Diagnostic snapshots from process context, never from the printk/panic path. */
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/init.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sec_cache_reboot.h>
#include <linux/timekeeping.h>
#include <linux/statfs.h>
#include <linux/vmalloc.h>

#define SNAPSHOT_SIZE (512 * 1024)
#define CACHE_RESERVE (8 * 1024 * 1024)

/* One-shot persistent storage: never retain a syscall stack pointer. */
static atomic_t reboot_claimed = ATOMIC_INIT(0);
static atomic_t reboot_pending = ATOMIC_INIT(0);
static DECLARE_COMPLETION(reboot_saved);
static char reboot_record[512];
static int reboot_save_result;
static char diagnostic_boot_id[37];
static atomic_t diagnostic_ready = ATOMIC_INIT(0);

static ssize_t cache_ready_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *position)
{
	char status[96];
	int length;

	length = scnprintf(status, sizeof(status), "J2PC1 ready=%d boot=%s\n",
			   atomic_read(&diagnostic_ready), diagnostic_boot_id);
	return simple_read_from_buffer(buffer, count, position, status, length);
}

static const struct file_operations cache_ready_fops = {
	.read = cache_ready_read,
	.llseek = default_llseek,
};

void sec_cache_record_reboot(unsigned int command, const char *target)
{
	struct timespec now;
	char comm[TASK_COMM_LEN];

	if (atomic_cmpxchg(&reboot_claimed, 0, 1))
		return;
	get_monotonic_boottime(&now);
	get_task_comm(comm, current);
	scnprintf(reboot_record, sizeof(reboot_record),
		  "J2PC1 boot=%s boottime=%lld.%09ld pid=%d tgid=%d comm=%s command=0x%x target=%s\n",
		  diagnostic_boot_id, (long long)now.tv_sec, now.tv_nsec, task_pid_nr(current),
		  task_tgid_nr(current), comm, command, target ? target : "<NULL>");
	pr_emerg("%s", reboot_record);
	smp_wmb();
	atomic_set(&reboot_pending, 1);
	if (!wait_for_completion_timeout(&reboot_saved, msecs_to_jiffies(2000)))
		pr_emerg("cache_kmsg: reboot record save timed out; continuing reboot\n");
	else
		pr_emerg("cache_kmsg: reboot record save result=%d\n",
			 reboot_save_result);
}

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

static int cache_write(struct path *cache, const char *name,
		       const char *buffer, size_t length, bool exclusive)
{
	struct file *file, *directory;
	struct kstatfs stats;
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

	file = file_open_root(cache->dentry, cache->mnt, name,
			      O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK |
			      (exclusive ? O_EXCL : 0), 0600);
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
	if (!ret) {
		/* A new file's name needs directory durability as well as data. */
		directory = file_open_root(cache->dentry, cache->mnt, ".",
					   O_RDONLY | O_DIRECTORY, 0);
		if (IS_ERR(directory))
			ret = PTR_ERR(directory);
		else {
			ret = vfs_fsync(directory, 0);
			filp_close(directory, NULL);
		}
		if (ret)
			pr_err("cache_kmsg: parent directory fsync %s failed: %d\n",
			       name, ret);
	}
close:
	filp_close(file, NULL);
	return ret;
}

static int cache_snapshot(struct path *cache, const char *buffer, size_t length,
			 unsigned int slot)
{
	char name[48];

	snprintf(name, sizeof(name), "kernel-latest-%u.log", slot);
	return cache_write(cache, name, buffer, length, false);
}

/* Process-context positive control. READ bio bypasses the filesystem and block
 * page caches. It does not bypass an eMMC device's internal volatile cache.
 * Never write raw sectors, invalidate live pages, or change mount options.
 */
static int cache_marker_block_readback(struct path *cache, const char *name,
				       const char *expected, size_t length)
{
	struct super_block *sb = cache->dentry->d_sb;
	struct block_device *bdev = sb->s_bdev;
	struct file *file;
	struct page *page;
	struct bio *bio;
	sector_t block, sector;
	char device_name[BDEVNAME_SIZE];
	void *data;
	unsigned int blocksize = sb->s_blocksize;
	int ret;

	if (!bdev || blocksize > PAGE_SIZE || blocksize < 512 ||
	    length > blocksize || i_size_read(bdev->bd_inode) != 209715200)
		return -EINVAL;
	pr_info("cache_kmsg: J2RB1 boot=%s device=%s dev=%u:%u bytes=%lld uuid=%pU blocksize=%u\n",
		diagnostic_boot_id, bdevname(bdev, device_name),
		MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev),
		(long long)i_size_read(bdev->bd_inode), sb->s_uuid, blocksize);
	down_read(&sb->s_umount);
	ret = sync_filesystem(sb);
	up_read(&sb->s_umount);
	pr_info("cache_kmsg: J2RB1 sync_filesystem=%d\n", ret);
	if (ret)
		return ret;
	ret = blkdev_issue_flush(bdev, GFP_KERNEL, NULL);
	pr_info("cache_kmsg: J2RB1 device_flush=%d\n", ret);
	if (ret)
		return ret;
	file = file_open_root(cache->dentry, cache->mnt, name,
			      O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	if (!S_ISREG(file_inode(file)->i_mode) ||
	    i_size_read(file_inode(file)) != length ||
	    file->f_path.mnt != cache->mnt ||
	    file->f_path.dentry->d_parent != cache->dentry) {
		filp_close(file, NULL);
		return -EINVAL;
	}
	block = bmap(file_inode(file), 0);
	filp_close(file, NULL);
	if (!block)
		return -ENXIO;
	sector = block * (blocksize >> 9);
	if (sector + (blocksize >> 9) > (i_size_read(bdev->bd_inode) >> 9))
		return -EINVAL;
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio) {
		__free_page(page);
		return -ENOMEM;
	}
	bio->bi_bdev = bdev;
	bio->bi_iter.bi_sector = sector;
	if (bio_add_page(bio, page, blocksize, 0) != blocksize) {
		ret = -EIO;
		goto release;
	}
	ret = submit_bio_wait(READ | REQ_SYNC, bio);
	if (!ret) {
		data = kmap(page);
		if (memcmp(data, expected, length))
			ret = -EILSEQ;
		kunmap(page);
	}
	pr_info("cache_kmsg: J2RB1 boot=%s file=%s partition_sector=%llu bytes=%zu direct_read_compare=%d\n",
		diagnostic_boot_id, name, (unsigned long long)sector, length, ret);
release:
	bio_put(bio);
	__free_page(page);
	return ret;
}

/* Preserve previous-boot ramoops records on disk without relying on recovery's
 * kernel having a matching DT node/driver. Never erase a pstore record, overwrite
 * an older export, or attempt filesystem I/O in panic context.
 */
static void cache_export_pstore(struct path *cache, char *buffer)
{
	static const char * const names[] = {
		"console-ramoops", "dmesg-ramoops-0", "dmesg-ramoops-1",
		"dmesg-ramoops-2", "dmesg-ramoops-3",
		"dmesg-ramoops-0.enc.z", "dmesg-ramoops-1.enc.z",
		"dmesg-ramoops-2.enc.z", "dmesg-ramoops-3.enc.z",
		"pmsg-ramoops-0",
	};
	struct file_system_type *type;
	struct vfsmount *mnt;
	struct file *source;
	unsigned int i, slot;
	loff_t pos;
	int got, ret;
	char destination[96];

	type = get_fs_type("pstore");
	if (!type) {
		pr_warn("cache_kmsg: pstore filesystem unavailable\n");
		return;
	}
	mnt = vfs_kern_mount(type, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
			     "cache-kmsg-pstore", NULL);
	put_filesystem(type);
	if (IS_ERR(mnt)) {
		pr_warn("cache_kmsg: pstore mount failed: %ld\n", PTR_ERR(mnt));
		return;
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		source = file_open_root(mnt->mnt_root, mnt, names[i],
					O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0);
		if (IS_ERR(source)) {
			if (PTR_ERR(source) != -ENOENT)
				pr_warn("cache_kmsg: pstore open %s: %ld\n",
					names[i], PTR_ERR(source));
			continue;
		}
		pos = 0;
		while (pos < SNAPSHOT_SIZE) {
			got = kernel_read(source, pos, buffer + pos,
					  SNAPSHOT_SIZE - pos);
			if (got <= 0)
				break;
			pos += got;
		}
		filp_close(source, NULL);
		if (got < 0 || !pos) {
			pr_warn("cache_kmsg: pstore read %s: %d, bytes=%lld\n",
				names[i], got, (long long)pos);
			continue;
		}
		/* Bounded retention: at most 16 exports per record name. */
		ret = -EEXIST;
		for (slot = 0; slot < 16 && ret == -EEXIST; slot++) {
			snprintf(destination, sizeof(destination),
				 "previous-boot-%02u-%s", slot, names[i]);
			ret = cache_write(cache, destination, buffer, pos, true);
		}
		if (ret)
			pr_warn("cache_kmsg: pstore export %s failed: %d\n",
				names[i], ret);
		else
			pr_info("cache_kmsg: pstore saved %s (%lld bytes)\n",
				destination, (long long)pos);
	}
	mntput(mnt);
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
	bool exported = false;
	bool readiness_attempted = false;
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
		if (atomic_read(&reboot_pending)) {
			char name[64];
			unsigned int record_slot;
			int snapshot_ret = -ENODATA;

			smp_rmb();
			ret = -EEXIST;
			/* Preserve old requests with bounded, exclusive creation. */
			for (record_slot = 0; record_slot < 16 && ret == -EEXIST;
			     record_slot++) {
				snprintf(name, sizeof(name), "reboot-request-%02u.log",
					 record_slot);
				ret = cache_write(&cache, name, reboot_record,
						  strlen(reboot_record), true);
			}
			if (!ret)
				ret = cache_marker_block_readback(&cache, name,
						 reboot_record, strlen(reboot_record));
			reboot_save_result = ret;
			/* Capture the fatal message immediately, not on the next periodic
			 * iteration. Keep it separate from rotating/stale snapshots and
			 * identify it with this boot's ID. All I/O stays in this worker.
			 */
			kmsg_dump_rewind(&dumper);
			if (kmsg_dump_get_buffer(&dumper, true, buffer,
						 SNAPSHOT_SIZE, &length) && length) {
				snprintf(name, sizeof(name), "reboot-kmsg-%s.log",
					 diagnostic_boot_id);
				snapshot_ret = cache_write(&cache, name, buffer, length, true);
				if (!snapshot_ret) {
					down_read(&cache.dentry->d_sb->s_umount);
					snapshot_ret = sync_filesystem(cache.dentry->d_sb);
					up_read(&cache.dentry->d_sb->s_umount);
					if (!snapshot_ret)
						snapshot_ret = blkdev_issue_flush(
							cache.dentry->d_sb->s_bdev,
							GFP_KERNEL, NULL);
				}
			}
			pr_emerg("cache_kmsg: reboot snapshot boot=%s result=%d request_result=%d\n",
				 diagnostic_boot_id, snapshot_ret, reboot_save_result);
			atomic_set(&reboot_pending, 0);
			complete(&reboot_saved);
		}
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
		/* Complete old-record export before advertising probe readiness. */
		if (saved && !exported) {
			exported = true;
			cache_export_pstore(&cache, buffer);
		}
		if (saved && !readiness_attempted) {
			char name[64], status[96];
			int length;

			readiness_attempted = true;
			snprintf(name, sizeof(name), "probe-ready-%s.log",
				 diagnostic_boot_id);
			length = scnprintf(status, sizeof(status),
					   "J2PC1 ready=1 boot=%s\n", diagnostic_boot_id);
			ret = cache_write(&cache, name, status, length, true);
			if (!ret)
				ret = cache_marker_block_readback(&cache, name, status, length);
			if (!ret) {
				atomic_set(&diagnostic_ready, 1);
				pr_info("cache_kmsg: J2PC1 durable readiness boot=%s\n",
					diagnostic_boot_id);
			} else {
				pr_err("cache_kmsg: readiness write failed: %d\n", ret);
			}
		}
		msleep_interruptible(500);
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
	u8 boot_id[16];

	get_random_bytes(boot_id, sizeof(boot_id));
	snprintf(diagnostic_boot_id, sizeof(diagnostic_boot_id), "%pU", boot_id);
	if (!proc_create("j2_cache_ready", 0400, NULL, &cache_ready_fops))
		return -ENOMEM;

	task = kthread_run(cache_kmsg_thread, NULL, "cache_kmsg");
	return IS_ERR(task) ? PTR_ERR(task) : 0;
}
fs_initcall_sync(cache_kmsg_init);

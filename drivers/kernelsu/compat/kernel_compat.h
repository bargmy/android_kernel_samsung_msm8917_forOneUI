#ifndef __KSU_H_KERNEL_COMPAT
#define __KSU_H_KERNEL_COMPAT

#include <linux/fs.h>
#include <linux/version.h>
#include <linux/task_work.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
/* 3.1x: vmalloc/vfree/vzalloc are only declared via <linux/vmalloc.h> */
#include <linux/vmalloc.h>
#endif
#include "ss/policydb.h"
#include "linux/key.h"

/*
 * Adapt to Huawei HISI kernel without affecting other kernels ,
 * Huawei Hisi Kernel EBITMAP Enable or Disable Flag ,
 * From ss/ebitmap.h
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)) &&                         \
		(LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)) ||             \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)) &&                    \
		(LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0))
#ifdef HISI_SELINUX_EBITMAP_RO
#define CONFIG_IS_HW_HISI
#endif
#endif

// Checks for UH, KDP and RKP
#ifdef SAMSUNG_UH_DRIVER_EXIST
#if defined(CONFIG_UH) || defined(CONFIG_KDP) || defined(CONFIG_RKP)
#error "CONFIG_UH, CONFIG_KDP and CONFIG_RKP is enabled! Please disable or remove it before compile a kernel with KernelSU!"
#endif
#endif

extern struct file *ksu_filp_open_compat(const char *filename, int flags,
					 umode_t mode);
extern ssize_t ksu_kernel_read_compat(struct file *p, void *buf, size_t count,
				      loff_t *pos);
extern ssize_t ksu_kernel_write_compat(struct file *p, const void *buf,
				       size_t count, loff_t *pos);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) ||                           \
	defined(CONFIG_IS_HW_HISI) || defined(CONFIG_KSU_ALLOWLIST_WORKAROUND)
extern struct key *init_session_keyring;
#endif

extern long ksu_copy_from_user_nofault(void *dst, const void __user *src, size_t size);
/*
 * ksu_copy_from_user_retry
 * try nofault copy first, if it fails, try with plain
 * paramters are the same as copy_from_user
 * 0 = success
 */
static inline long ksu_copy_from_user_retry(void *to, 
		const void __user *from, unsigned long count)
{
	long ret = ksu_copy_from_user_nofault(to, from, count);
	if (likely(!ret))
		return ret;

	// we faulted! fallback to slow path
	return copy_from_user(to, from, count);
}

/*
 * untagged_addr() is an ARM64 ABI tag abstraction; on 3.1x there are no
 * tagged pointers.
 */
#ifndef untagged_addr
#define untagged_addr(addr) ((unsigned long)(addr))
#endif

/*
 * d_is_reg() arrived with the dentry flag rework (3.20); derive it from the
 * dentry inode on older trees.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 20, 0)
#include <linux/dcache.h>
#include <linux/fs.h>
static inline bool d_is_reg(const struct dentry *dentry)
{
	return d_is_positive(dentry) && S_ISREG(dentry->d_inode->i_mode);
}
#endif

/*
 * ALIGN_DOWN() arrived in 4.19.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#include <linux/kernel.h>
#define ALIGN_DOWN(x, a) __ALIGN_KERNEL((x) - ((a) - 1), (a))
#endif

/*
 * static-key convenience API (DEFINE_STATIC_KEY_FALSE / static_branch_unlikely
 * / static_key_enable / static_key_disable) post-dates 3.18; emulate it on top
 * of the legacy struct static_key (atomic enabled counter) API.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
#include <linux/jump_label.h>

#define DEFINE_STATIC_KEY_FALSE(name)                                          \
	struct {                                                               \
		struct static_key key;                                         \
	} name = { .key = STATIC_KEY_INIT_FALSE }

static inline bool ksu_static_key_enabled(void *p)
{
	return static_key_count((struct static_key *)p) > 0;
}

static inline bool ksu_static_branch_unlikely(void *p)
{
	return static_key_true((struct static_key *)p);
}

static inline void ksu_static_key_enable(void *p)
{
	if (!ksu_static_key_enabled(p))
		static_key_slow_inc((struct static_key *)p);
}

static inline void ksu_static_key_disable(void *p)
{
	if (ksu_static_key_enabled(p))
		static_key_slow_dec((struct static_key *)p);
}

#define static_key_enabled(x)      ksu_static_key_enabled((void *)(x))
#define static_branch_unlikely(x)  ksu_static_branch_unlikely((void *)(x))
#define static_key_enable(x)       ksu_static_key_enable((void *)(x))
#define static_key_disable(x)      ksu_static_key_disable((void *)(x))
#endif /* < 4.6 */


#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
extern void *ksu_compat_kvrealloc(const void *p, size_t oldsize, size_t newsize,
				  gfp_t flags);
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
static inline void *ksu_kvmalloc(size_t size, gfp_t flags)
{
	void *buf = kmalloc(size, flags);
	if (!buf)
		buf = vmalloc(size);
	
	return buf;
}

static inline void ksu_kvfree(const void *buf)
{
	if (is_vmalloc_addr(buf))
		vfree(buf);
	else
		kfree(buf);
}
#define kvmalloc ksu_kvmalloc
#define kvfree ksu_kvfree
#endif

/*
 * kvcalloc()/kvmalloc_array() post-date 3.18 (kvcalloc: 4.18,
 * kvmalloc_array: 4.12); emulate them with the kvmalloc family above (the
 * kvmalloc name maps to ksu_kvmalloc on kernels lacking it).
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
#include <linux/kallsyms.h>
static inline void *ksu_compat_kvcalloc(size_t n, size_t size, gfp_t flags)
{
	if (size == 0 || n == 0)
		return NULL;
	if (n > (UINT_MAX / size))
		return NULL;
	return kvmalloc(n * size, flags);
}
#define kvcalloc(n, s, f)       ksu_compat_kvcalloc((n), (s), (f))
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#define kvmalloc_array(n, s, f) ksu_compat_kvcalloc((n), (s), (f))
#endif
#endif /* < 4.18 */

// https://elixir.bootlin.com/linux/v4.14.222/source/lib/string.c#L282
static inline ssize_t __strscpy_pad(char *dest, const char *src, size_t count)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 222)
    return strscpy_pad(dest, src, count);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
    ssize_t res = strscpy(dest, src, count);
    if (res >= 0 && (size_t)res < count) {
        memset(dest + res, 0, count - res);
    }
    return res;
#else
    if (count == 0)
        return -E2BIG;

    memcpy(dest, src, count);
    dest[count - 1] = '\0';
    return strlen(dest);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#define ksu_access_ok(addr, size) access_ok(addr, size)
#else
#define ksu_access_ok(addr, size) access_ok(VERIFY_READ, addr, size)
#endif

#ifndef KSU_OPTIONAL_STRNCPY
extern long strncpy_from_user_nofault(char *dst, const void __user *unsafe_addr,
				   long count);
#endif // #ifndef KSU_OPTIONAL_STRNCPY

// Linux >= 5.7
// task_work_add (struct, struct, enum)
// Linux pre-5.7
// task_work_add (struct, struct, bool)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
#ifndef TWA_RESUME
#define TWA_RESUME true
#endif
#endif

#endif // #ifndef __KSU_H_KERNEL_COMPAT

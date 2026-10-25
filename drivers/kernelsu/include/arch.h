#ifndef __KSU_H_ARCH
#define __KSU_H_ARCH

#include <linux/version.h>

#if defined(__aarch64__)

#define __PT_PARM1_REG regs[0]
#define __PT_PARM2_REG regs[1]
#define __PT_PARM3_REG regs[2]
#define __PT_SYSCALL_PARM4_REG regs[3]
#define __PT_CCALL_PARM4_REG regs[3]
#define __PT_PARM5_REG regs[4]
#define __PT_PARM6_REG regs[5]
#define __PT_RET_REG regs[30]
#define __PT_FP_REG regs[29] /* Works only with CONFIG_FRAME_POINTER */
#define __PT_RC_REG regs[0]
#define __PT_SP_REG sp
#define __PT_IP_REG pc

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#define REBOOT_SYMBOL "__arm64_sys_reboot"
#define SYS_READ_SYMBOL "__arm64_sys_read"
#define SYS_EXECVE_SYMBOL "__arm64_sys_execve"
#define SYS_EXECVEAT_SYMBOL "__arm64_sys_execveat"
#define SYS_SETNS_SYMBOL __arm64_sys_setns
// https://cs.android.com/android/kernel/superproject/+/common-android-mainline:common/scripts/syscalltbl.sh;l=57;drc=9142be9e6443fd641ca37f820efe00d9cd890eb1
// https://cs.android.com/android/kernel/superproject/+/common-android-mainline:common/scripts/syscall.tbl;l=104;drc=b36d4b6aa88ef039647228b98c59a875e92f8c8e
#define SYS_FSTAT_SYMBOL "__arm64_sys_newfstat"
#else
#define REBOOT_SYMBOL "sys_reboot"
#define SYS_READ_SYMBOL "sys_read"
#define SYS_EXECVE_SYMBOL "sys_execve"
#define SYS_EXECVEAT_SYMBOL "sys_execveat"
#define SYS_SETNS_SYMBOL sys_setns
#define SYS_FSTAT_SYMBOL "sys_newfstat"
#endif

#elif defined(__x86_64__)

#define __PT_PARM1_REG di
#define __PT_PARM2_REG si
#define __PT_PARM3_REG dx
/* syscall uses r10 for PARM4 */
#define __PT_SYSCALL_PARM4_REG r10
#define __PT_CCALL_PARM4_REG cx
#define __PT_PARM5_REG r8
#define __PT_PARM6_REG r9
#define __PT_RET_REG sp
#define __PT_FP_REG bp
#define __PT_RC_REG ax
#define __PT_SP_REG sp
#define __PT_IP_REG ip

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#define REBOOT_SYMBOL "__x64_sys_reboot"
#define SYS_READ_SYMBOL "__x64_sys_read"
#define SYS_EXECVE_SYMBOL "__x64_sys_execve"
#define SYS_EXECVEAT_SYMBOL "__x64_sys_execveat"
#define SYS_SETNS_SYMBOL __x64_sys_setns
#define SYS_FSTAT_SYMBOL "__x64_sys_newfstat"
#else
#define REBOOT_SYMBOL "sys_reboot"
#define SYS_READ_SYMBOL "sys_read"
#define SYS_EXECVE_SYMBOL "sys_execve"
#define SYS_EXECVEAT_SYMBOL "sys_execveat"
#define SYS_SETNS_SYMBOL sys_setns
#define SYS_FSTAT_SYMBOL "sys_newfstat"
#endif

#elif defined(__arm__)

/*
 * ARM32 (CONFIG_ARM): struct pt_regs carries the register frame in uregs[]
 * (arch/arm/include/asm/ptrace.h). These mappings are only referenced by the
 * kprobe/syscall-table hook backends; manual hook mode does not rely on them,
 * but the header must still compile on arm32.
 */
#define __PT_PARM1_REG uregs[0]		/* ARM_r0 */
#define __PT_PARM2_REG uregs[1]		/* ARM_r1 */
#define __PT_PARM3_REG uregs[2]		/* ARM_r2 */
#define __PT_SYSCALL_PARM4_REG uregs[3]	/* ARM_r3 (scratch) */
#define __PT_CCALL_PARM4_REG uregs[3]	/* ARM_r3 */
#define __PT_PARM5_REG uregs[4]		/* ARM_r4 */
#define __PT_PARM6_REG uregs[5]		/* ARM_r5 */
#define __PT_RET_REG uregs[15]		/* ARM_pc */
#define __PT_FP_REG uregs[11]		/* ARM_fp (works only with CONFIG_FRAME_POINTER) */
#define __PT_RC_REG uregs[0]		/* ARM_r0 */
#define __PT_SP_REG uregs[13]		/* ARM_sp */
#define __PT_IP_REG uregs[14]		/* ARM_lr */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#define REBOOT_SYMBOL "__arm_sys_reboot"
#define SYS_READ_SYMBOL "__arm_sys_read"
#define SYS_EXECVE_SYMBOL "__arm_sys_execve"
#define SYS_EXECVEAT_SYMBOL "__arm_sys_execveat"
#define SYS_SETNS_SYMBOL __arm_sys_setns
#define SYS_FSTAT_SYMBOL "__arm_sys_newfstat"
#else
#define REBOOT_SYMBOL "sys_reboot"
#define SYS_READ_SYMBOL "sys_read"
#define SYS_EXECVE_SYMBOL "sys_execve"
#define SYS_EXECVEAT_SYMBOL "sys_execveat"
#define SYS_SETNS_SYMBOL sys_setns
#define SYS_FSTAT_SYMBOL "sys_newfstat"
#endif

#else
#error "Unsupported arch"
#endif

/* allow some architecutres to override `struct pt_regs` */
#ifndef __PT_REGS_CAST
#define __PT_REGS_CAST(x) (x)
#endif

#define PT_REGS_PARM1(x) (__PT_REGS_CAST(x)->__PT_PARM1_REG)
#define PT_REGS_PARM2(x) (__PT_REGS_CAST(x)->__PT_PARM2_REG)
#define PT_REGS_PARM3(x) (__PT_REGS_CAST(x)->__PT_PARM3_REG)
#define PT_REGS_SYSCALL_PARM4(x) (__PT_REGS_CAST(x)->__PT_SYSCALL_PARM4_REG)
#define PT_REGS_CCALL_PARM4(x) (__PT_REGS_CAST(x)->__PT_CCALL_PARM4_REG)
#define PT_REGS_PARM5(x) (__PT_REGS_CAST(x)->__PT_PARM5_REG)
#define PT_REGS_PARM6(x) (__PT_REGS_CAST(x)->__PT_PARM6_REG)
#define PT_REGS_RET(x) (__PT_REGS_CAST(x)->__PT_RET_REG)
#define PT_REGS_FP(x) (__PT_REGS_CAST(x)->__PT_FP_REG)
#define PT_REGS_RC(x) (__PT_REGS_CAST(x)->__PT_RC_REG)
#define PT_REGS_SP(x) (__PT_REGS_CAST(x)->__PT_SP_REG)
#define PT_REGS_IP(x) (__PT_REGS_CAST(x)->__PT_IP_REG)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#define PT_REAL_REGS(regs) ((struct pt_regs *)PT_REGS_PARM1(regs))
#else
#define PT_REAL_REGS(regs) ((regs))
#endif


#endif

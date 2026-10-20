#ifndef _LINUX_SEC_CACHE_REBOOT_H
#define _LINUX_SEC_CACHE_REBOOT_H
#ifdef CONFIG_SEC_CACHE_KMSG
void sec_cache_record_reboot(unsigned int command, const char *target);
#else
static inline void sec_cache_record_reboot(unsigned int command,
					 const char *target) { }
#endif
#endif

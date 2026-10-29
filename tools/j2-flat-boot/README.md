# J2 Quack flat-system test boot

This reproduces the ramdisk changes from the local `j2-quack-flat-boot-TEST.img`
and inserts the zImage compiled by this repository. CI now publishes that test
boot alongside the kernel, configuration, maps, and device trees.

```
python3 tools/j2-flat-boot/build.py --kernel out/arch/arm/boot/zImage --output boot-TEST.img
```

The original working package is `OneUI3.1-Core-j2y18lte.zip/boot.img`.
`provenance.json` records its checksum. The compressed template retains its
header, original ramdisk, second payload, and QCDT, with the old kernel removed.
The first-stage init is a retained prebuilt executable, not rebuilt Android init
source. No Linux C change is needed for these mount-layout changes.

The versioned Python transformations:

- Mount the flat system filesystem at `/j2_system`, with `/system` linking to it,
  avoiding the reference first-stage init's system-as-root switch.
- Set `/vendor`, `/product`, `/bin`, `/etc`, and other root links for this layout.
- Preserve the reference first-stage init and QCDT bytes and existing cpio metadata.
- Install the editable `ramdisk/*.rc` files and Android 10 environment.
- Select `/system/etc/j2-rootdir/init.rc` on the boot command line and retain the
  local test build's permissive SELinux setting.
- Recalculate payload sizes, alignment, and the legacy boot ID; reject images
  exceeding the 32 MiB boot partition.

Requires the matching `system-quack-j2y18lte-TEST.img`, containing the Android 10
init wrapper, original init, vendor, and `/etc/j2-rootdir` files from the port.
This tool does not modify or rebuild that system image. The separately published
DTBs are not substituted for the known reference QCDT in this boot build.

The configuration intentionally retains the requested legacy Binder protocol 7
(`CONFIG_ANDROID_BINDER_IPC_32BIT=y`) and cache logging. CPU bitness alone does
not establish the userspace Binder ABI; this setting still needs device testing
with the chosen GSI. The workflow now verifies the settings actually requested
instead of rejecting them with stale Binder-8 and logging-disabled assertions.

This is an offline-validated test build, not a device-tested release. No flashing
or automatic device reboot is performed by these tools.

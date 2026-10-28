#!/usr/bin/env python3
"""Host-side tests of actual driver functions with mocked kernel/I2C APIs.

This does not emulate the controller or prove that touch firmware supports DT2W.
Run with Python 3 and a host C compiler; no phone access or extra packages needed.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise ValueError("Function not found: " + name)
    start = match.start()
    pos = match.end()
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint16_t u16;
#define IST30XX_NOISE_MODE
#define IST30XX_DATA_LEN 4
#define IST30XX_ENABLE 1
#define IST30XX_DISABLE 0
#define IST30XX_HIB_INTR_MSG 1
#define IST30XX_HIB_GESTURE_REG 2
#define IST30XX_HIB_CMD 3
#define IST30XX_LPM_VALUE 4
#define eHCOM_GESTURE_EN 0x12
#define KEY_WAKEUP 143
#define KEY_BLACK_UI_GESTURE 0x270
#define SPONGE_EVENT_TYPE_SPAY 4
#define SPONGE_EVENT_TYPE_AOD_DOUBLETAB 11
#define TOUCH_MODE 0
#define LPM_MODE 1
#define NOISE_MODE_POWER 8
#define EVENT_TIMER_INTERVAL 1
#define tsp_err(...) ((void)0)
#define tsp_warn(...) ((void)0)
#define tsp_info(...) ((void)0)
#define tsp_noti(...) ((void)0)
#define tsp_debug(...) ((void)0)
struct device { int unused; };
struct i2c_client { struct device dev; int irq; void *data; };
struct ist30xx_data {
    struct i2c_client *client;
    void *input_dev, *pinctrl;
    gesture_reg g_reg;
    struct { int width, height; } tsp_info;
    struct { int sys_mode, power; bool noise_mode; } status;
    bool double_tap_to_wake, suspend, spay, aod, irq_wake_enabled;
    int debugging_mode, noise_mode, lock, aod_lock, event_timer;
    int work_reset_check, work_noise_protect, work_debug_algorithm;
    unsigned int scrub_id, scrub_x, scrub_y;
};
static int read_error, write_error, command_error, irq_error;
static int write_count, command_count, wake_keys, black_keys;
static int wake_refs, may_wakeup = 1, power_offs, resets;
static gesture_reg written, event;
#define to_i2c_client(d) ((struct i2c_client *)(d))
#define i2c_get_clientdata(c) ((struct ist30xx_data *)(c)->data)
#define device_may_wakeup(d) (may_wakeup)
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define del_timer_sync(x) ((void)0)
#define cancel_delayed_work_sync(x) ((void)0)
#define mod_timer(x,y) ((void)0)
#define get_jiffies_64() 0
#define clear_input_data(d) ((void)0)
#define ist30xx_enable_irq(d) ((void)0)
#define ist30xx_disable_irq(d) ((void)0)
#define ist30xx_start(d) ((void)0)
static int ist30xx_burst_write(struct i2c_client *c, u32 addr, u32 *v, u16 n)
{ write_count++; memset(&written, 0, sizeof(written)); memcpy(written.full, v, n * 4); return write_error; }
static int ist30xx_burst_read(struct i2c_client *c, u32 addr, u32 *v, u16 n, bool b)
{ memcpy(v, event.full, n * 4); return read_error; }
static int ist30xx_write_cmd(struct ist30xx_data *d, u32 addr, u32 v)
{ command_count++; return command_error; }
static void input_report_key(void *d, int key, int value)
{ if (value && key == KEY_WAKEUP) wake_keys++; if (value && key == KEY_BLACK_UI_GESTURE) black_keys++; }
static void input_sync(void *d) {}
static int enable_irq_wake(int irq) { if (!irq_error) wake_refs++; return irq_error; }
static int disable_irq_wake(int irq) { assert(wake_refs == 1); wake_refs--; return 0; }
static int ist30xx_power_off(struct ist30xx_data *d) { power_offs++; d->status.power = 0; return 0; }
static int ist30xx_power_on(struct ist30xx_data *d, bool b) { d->status.power = 1; return 0; }
static int ist30xx_reset(struct ist30xx_data *d, bool b) { resets++; return 0; }
static int ist30xx_pinctrl_configure(struct ist30xx_data *d, bool b) { return 0; }
'''

TESTS = r'''
int main(void)
{
    struct i2c_client c = {0};
    struct ist30xx_data d = {0};
    gesture_reg saved;
    d.client = &c; c.data = &d;
    d.tsp_info.width = 540; d.tsp_info.height = 960;
    d.g_reg.b.ctrl = IST30XX_GETURE_CTRL_SPAY;
    d.g_reg.b.setting = IST30XX_GETURE_SET_SPAY;
    d.g_reg.b.w = 100; d.g_reg.b.h = 200; d.g_reg.b.x = 10;
    saved = d.g_reg;
    d.double_tap_to_wake = true;
    assert(!ist30xx_write_gesture(&d));
    assert(written.b.w == 540 && written.b.h == 960 && !written.b.x && !written.b.y);
    assert(written.b.ctrl & IST30XX_GETURE_CTRL_AOD);
    assert(written.b.ctrl & IST30XX_GETURE_CTRL_SPAY);
    assert(written.b.setting & IST30XX_GETURE_SET_AOD);
    assert(!memcmp(&saved, &d.g_reg, sizeof(saved)));
    d.double_tap_to_wake = false;
    assert(!ist30xx_write_gesture(&d));
    assert(!memcmp(&saved, &written, sizeof(saved)));

    /* Abort at the first failed command; never claim successful LPM. */
    command_error = -EIO; write_count = 0;
    assert(ist30xx_cmd_gesture(&d, 1) == -EIO && write_count == 0);
    command_error = 0; write_error = -EIO; command_count = 0;
    assert(ist30xx_cmd_gesture(&d, 1) == -EIO && command_count == 1);
    write_error = 0;

    event.b.evt = IST30XX_GETURE_EVT_AOD;
    event.b.evt_x = 42; event.b.evt_y = 84;
    d.suspend = true; d.double_tap_to_wake = true;
    ist30xx_special_cmd(&d, 0);
    assert(wake_keys == 1 && black_keys == 1 && written.b.evt == 0);
    assert(d.scrub_x == 42 && d.scrub_y == 84);
    assert(!memcmp(&saved, &d.g_reg, sizeof(saved)));
    d.suspend = false; ist30xx_special_cmd(&d, 0);
    assert(wake_keys == 1 && black_keys == 2);
    d.suspend = true; d.double_tap_to_wake = false;
    ist30xx_special_cmd(&d, 0);
    assert(wake_keys == 1 && black_keys == 3);
    d.double_tap_to_wake = true; read_error = -EIO;
    ist30xx_special_cmd(&d, 0); assert(wake_keys == 1 && black_keys == 3);
    read_error = 0; write_error = -EIO;
    ist30xx_special_cmd(&d, 0); assert(wake_keys == 1 && black_keys == 3);
    write_error = 0;
    event.b.evt = IST30XX_GETURE_EVT_SPAY;
    ist30xx_special_cmd(&d, 0); assert(wake_keys == 1 && black_keys == 4);

    d.suspend = false; d.status.power = 1;
    assert(!ist30xx_suspend(&c.dev));
    assert(d.suspend && d.status.sys_mode == LPM_MODE && wake_refs == 1);
    assert(!ist30xx_suspend(&c.dev) && wake_refs == 1);
    assert(!ist30xx_resume(&c.dev) && wake_refs == 0 && !d.suspend);
    assert(!ist30xx_resume(&c.dev) && wake_refs == 0);
    /* Disabling device wake must not unbalance the reference on resume. */
    assert(!ist30xx_suspend(&c.dev) && wake_refs == 1);
    may_wakeup = 0;
    assert(!ist30xx_resume(&c.dev) && wake_refs == 0);
    assert(!ist30xx_suspend(&c.dev) && wake_refs == 0 && !d.status.power);
    assert(!ist30xx_resume(&c.dev)); may_wakeup = 1;
    irq_error = -EINVAL;
    assert(!ist30xx_suspend(&c.dev));
    assert(!wake_refs && !d.irq_wake_enabled && !d.status.power);
    assert(d.status.sys_mode == TOUCH_MODE); irq_error = 0;
    assert(!ist30xx_resume(&c.dev));
    command_error = -EIO;
    assert(!ist30xx_suspend(&c.dev) && !wake_refs && !d.status.power);
    command_error = 0; assert(!ist30xx_resume(&c.dev));
    assert(!ist30xx_suspend(&c.dev) && wake_refs == 1);
    command_error = -EIO;
    assert(!ist30xx_resume(&c.dev) && !wake_refs && resets == 1);
    command_error = 0;
    d.double_tap_to_wake = false;
    assert(!ist30xx_suspend(&c.dev) && !wake_refs && !d.status.power);
    assert(power_offs >= 4);
    puts("PASS: gesture overlay, event filtering, I2C failures, suspend/resume and IRQ wake balance");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver-dir", type=Path, default=Path(__file__).resolve().parents[2] / "drivers/input/touchscreen/imagis_30xxh")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    header = (args.driver_dir / "ist30xxh.h").read_text()
    core = (args.driver_dir / "ist30xxh.c").read_text()
    sys = (args.driver_dir / "ist30xxh_sys.c").read_text()
    union = re.search(r"typedef union \{\n    struct \{\n        u32 ctrl:8;.*?\} gesture_reg;", header, re.S).group()
    constants = "\n".join(line for line in header.splitlines() if line.startswith("#define IST30XX_GETURE_"))
    # Insert the driver's actual ABI rather than duplicating its register layout.
    prefix = STUBS.replace("struct device {", constants + "\n" + union + "\nstruct device {")
    funcs = "\n".join([function(sys, "ist30xx_write_gesture"), function(sys, "ist30xx_cmd_gesture"), function(core, "ist30xx_special_cmd"), function(core, "ist30xx_suspend"), function(core, "ist30xx_resume")])
    assert "getnstimeofday" not in function(core, "report_input_data")
    with tempfile.TemporaryDirectory(prefix="ist30xxh-dt2w-") as tmp:
        source = Path(tmp) / "test.c"
        binary = Path(tmp) / ("test.exe" if os.name == "nt" else "test")
        source.write_text(prefix + "\n" + funcs + "\n" + TESTS)
        subprocess.run([args.cc, "-std=gnu89", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

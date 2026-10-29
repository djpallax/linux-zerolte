// SPDX-License-Identifier: GPL-2.0-only
/* Experimental SM-G925I pre-userspace diagnostic; no partition access. */
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/rtmutex.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/regmap.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>

static struct regmap *s6_pmu;
static bool s6_recovery_enabled;

static bool s6_before_userspace(void)
{
	return READ_ONCE(system_state) == SYSTEM_SCHEDULING;
}

/* Inspect adapter metadata only: no bus transfer and no core_lock acquisition. */
static void s6_i2c_owner_snapshot(void)
{
	struct device *dev;
	struct i2c_adapter *adap;
	struct task_struct *owner;
	char name[16];
	int nr;

	for (nr = 0; nr < 4; nr++) {
		snprintf(name, sizeof(name), "i2c-%d", nr);
		dev = bus_find_device_by_name(&i2c_bus_type, NULL, name);
		if (!dev)
			continue;
		adap = i2c_verify_adapter(dev);
		if (!adap) {
			put_device(dev);
			continue;
		}
		rcu_read_lock();
		owner = rt_mutex_owner(&adap->bus_lock.rtmutex);
		if (owner)
			get_task_struct(owner);
		rcu_read_unlock();
		pr_emerg("bus=%s name=%s flags=%lx owner=%p\n", name,
			 adap->name, READ_ONCE(adap->locked_flags), owner);
		if (owner) {
			pr_emerg(
				"I2C OWNER pid=%d comm=%s state=%x; stack follows\n",
				task_pid_nr(owner), owner->comm,
				READ_ONCE(owner->__state));
			sched_show_task(owner);
			put_task_struct(owner);
		}
		put_device(dev);
	}
}

static void s6_boot_snapshot(unsigned int elapsed)
{
	struct task_struct *init;

	pr_emerg("stalled before userspace at %us; blocked tasks follow\n",
		 elapsed);
	show_state_filter(TASK_UNINTERRUPTIBLE);
	rcu_read_lock();
	init = pid_task(find_vpid(1), PIDTYPE_PID);
	if (init)
		get_task_struct(init);
	rcu_read_unlock();
	if (init) {
		pr_emerg("PID1 stack (last, for framebuffer capture)\n");
		sched_show_task(init);
		put_task_struct(init);
	}
	s6_i2c_owner_snapshot();
	{
		unsigned int irq, count;

		for (irq = 0; irq < 128; irq++) {
			count = kstat_irqs_cpu(irq, 0);
			if (count)
				pr_emerg("irq=%u count=%u\n", irq, count);
		}
	}
	pr_emerg("snapshot %us complete; USB may not be initialized\n",
		 elapsed);
}

static void s6_boot_recover_once(void)
{
	unsigned int old_mode, readback;
	int ret;

	/* The initramfs starts only after SYSTEM_RUNNING. Never reset a live root. */
	if (!s6_recovery_enabled || !s6_before_userspace() ||
	    num_online_cpus() != 1)
		return;
	ret = regmap_read(s6_pmu, 0x80c, &old_mode);
	if (ret)
		goto failed;
	ret = regmap_write(s6_pmu, 0x80c, 0x12345674);
	if (ret)
		goto failed;
	ret = regmap_read(s6_pmu, 0x80c, &readback);
	if (ret || readback != 0x12345674) {
		regmap_write(s6_pmu, 0x80c, old_mode);
		goto failed;
	}
	if (!s6_before_userspace()) {
		regmap_write(s6_pmu, 0x80c, old_mode);
		pr_emerg("userspace reached; recovery cancelled\n");
		return;
	}
	pr_emerg("recovery mode readback verified; one PMU restart at 120s\n");
	/*
	 * Same syscon register/mask as the already validated DT restart driver.
	 * Avoid device_shutdown(): the task holding a probe mutex may be stuck.
	 * No filesystems have been mounted by our guarded initramfs at this stage.
	 */
	ret = regmap_update_bits(s6_pmu, 0x400, 1, 1);
	if (!ret) {
		msleep(1000);
		regmap_write(s6_pmu, 0x80c, old_mode);
	}
failed:
	pr_emerg("restart unavailable/failed; STOP, manual TWRP required\n");
}

static int s6_boot_worker(void *unused)
{
	msleep(45000);
	if (!s6_before_userspace())
		return 0;
	s6_boot_snapshot(45);
	msleep(45000);
	if (!s6_before_userspace())
		return 0;
	s6_boot_snapshot(90);
	msleep(30000);
	s6_boot_recover_once();
	return 0;
}

static int __init s6_bootdiag_init(void)
{
	struct device_node *chosen, *pmu, *mode, *reset, *target;
	struct task_struct *worker;
	u32 mode_off, recovery, reset_off, reset_mask;
	bool valid;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return 0;
	chosen = of_find_node_by_path("/chosen");
	if (!of_property_read_bool(chosen, "samsung,s6-bootdiag")) {
		of_node_put(chosen);
		return 0;
	}
	s6_recovery_enabled =
		of_property_read_bool(chosen, "samsung,s6-bootdiag-recovery");
	of_node_put(chosen);
	pmu = of_find_node_by_path("/soc@0/system-controller@105c0000");
	mode = of_get_child_by_name(pmu, "reboot-mode");
	reset = of_get_child_by_name(pmu, "syscon-reboot");
	target = of_parse_phandle(reset, "regmap", 0);
	valid = pmu && of_device_is_compatible(pmu, "samsung,exynos7-pmu") &&
		of_device_is_compatible(mode, "syscon-reboot-mode") &&
		of_device_is_compatible(reset, "syscon-reboot") &&
		target == pmu &&
		!of_property_read_u32(mode, "offset", &mode_off) &&
		mode_off == 0x80c &&
		!of_property_read_u32(mode, "mode-recovery", &recovery) &&
		recovery == 0x12345674 &&
		!of_property_read_u32(reset, "offset", &reset_off) &&
		reset_off == 0x400 &&
		!of_property_read_u32(reset, "mask", &reset_mask) &&
		reset_mask == 1;
	if (valid) {
		s6_pmu = syscon_node_to_regmap(pmu);
		valid = !IS_ERR(s6_pmu);
	}
	s6_recovery_enabled &= valid;
	of_node_put(target);
	of_node_put(reset);
	of_node_put(mode);
	of_node_put(pmu);
	pr_info("boot snapshots at 45/90s; pre-userspace recovery at 120s=%d\n",
		s6_recovery_enabled);
	worker = kthread_run(s6_boot_worker, NULL, "s6-bootdiag");
	return PTR_ERR_OR_ZERO(worker);
}
core_initcall(s6_bootdiag_init);

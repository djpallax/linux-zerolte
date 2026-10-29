// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zerolte TMU monitoring using Samsung cal_tmu7420 calibration, with no TMU writes.
 * Requires firmware-initialized converters; invalid sensors fail closed.
 * Applies CPU frequency limits and core parking at the passive thermal trip,
 * and requests recovery at the critical trip.
 */
#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/of_address.h>
#include <linux/cpuhplock.h>
#include <linux/delay.h>
#include <linux/kmod.h>
#include <linux/ktime.h>
#include <linux/reboot.h>
#include <linux/sched/clock.h>
#include <linux/smp.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#include <clocksource/arm_arch_timer.h>
#include <asm/sysreg.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct s6_tmu {
	void __iomem *base[4];
	u32 calibration[4];
	struct mutex lock, cooling_lock;
	struct device *dev;
	void __iomem *apollo;
	struct cpufreq_policy *policy[2];
	struct freq_qos_request maxfreq[2];
	struct thermal_zone_device *zone;
	struct thermal_cooling_device *cooling;
	struct delayed_work guard;
	struct work_struct park_work;
	bool cooling_latched, stopping, sensor_fault;
	atomic_t recovery_requested;
};

static const char *const s6_labels[] = { "TMU_10060000", "TMU_10064000",
					 "TMU_10068000", "TMU_1006c000" };
static int s6_tmu_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *value)
{
	struct s6_tmu *d = dev_get_drvdata(dev);
	void __iomem *p = d->base[channel];
	u32 status, trim, code, first, second, after;
	int ret, temp;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	mutex_lock(&d->lock);
	if (!(readl(p + 0x20) & 1)) {
		ret = -ENODATA;
		goto out;
	}
	/*
	 * TEMP0/VALID_P0 belong to P0 even while another probe is selected.
	 * Only TRIMINFO is multiplexed; its immutable P0 value was captured at probe.
	 */
	ret = readl_poll_timeout(p + 0x28, status, status & BIT(12), 10, 20000);
	if (ret)
		goto out;
	trim = d->calibration[channel];
	code = readl(p + 0x40) & 0x1ff;
	after = readl(p + 0x28);
	if (!(after & BIT(12))) {
		ret = -EAGAIN;
		goto out;
	}
	first = trim & 0x1ff;
	second = (trim >> 9) & 0x1ff;
	if (!first || !code || first == 0x1ff || code == 0x1ff) {
		ret = -ENODATA;
		goto out;
	}
	if (trim & BIT(23)) {
		if (second <= first || second == 0x1ff) {
			ret = -ENODATA;
			goto out;
		}
		temp = ((int)code - (int)first) * 60000 /
			       (int)(second - first) +
		       25000;
	} else
		temp = ((int)code - (int)first) * 1000 + 25000;
	/* No clamping: impossible samples must not masquerade as safe temperatures. */
	if (temp < -40000 || temp > 150000) {
		ret = -ENODATA;
		goto out;
	}
	*value = temp;
	ret = 0;
out:
	mutex_unlock(&d->lock);
	return ret;
}

static int s6_tmu_label(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;
	*str = s6_labels[channel];
	return 0;
}

static umode_t s6_tmu_visible(const void *d, enum hwmon_sensor_types type,
			      u32 attr, int ch)
{
	return type == hwmon_temp && (attr == hwmon_temp_input ||
				      attr == hwmon_temp_label) ?
		       0444 :
		       0;
}

static const struct hwmon_ops s6_tmu_ops = {
	.is_visible = s6_tmu_visible,
	.read = s6_tmu_read,
	.read_string = s6_tmu_label,
};

static const struct hwmon_channel_info *const s6_tmu_channels[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info s6_tmu_info = { .ops = &s6_tmu_ops,
						    .info = s6_tmu_channels };

static int s6_capture_calibration(struct s6_tmu *d, int channel)
{
	void __iomem *p = d->base[channel];
	u32 status, trim, after, first, second;
	int attempt, ret;

	if (!(readl(p + 0x20) & 1))
		return -ENODATA;
	for (attempt = 0; attempt < 3; attempt++) {
		ret = readl_poll_timeout(p + 0x28, status,
					 !(status & GENMASK(10, 8)) &&
						 (status & BIT(12)),
					 10, 200000);
		if (ret)
			continue;
		trim = readl(p);
		after = readl(p + 0x28);
		if ((after & GENMASK(10, 8)) || !(after & BIT(12)))
			continue;
		first = trim & 0x1ff;
		second = (trim >> 9) & 0x1ff;
		if (!first || first == 0x1ff ||
		    ((trim & BIT(23)) && (second <= first || second == 0x1ff)))
			return -ENODATA;
		d->calibration[channel] = trim;
		dev_dbg(d->dev, "TMU%d P0 calibration=%08x captured once\n",
			channel, trim);
		return 0;
	}
	return -ETIMEDOUT;
}

struct s6_counter_sample {
	u64 mpidr, phys, virt, selected, sched, mono;
	u32 frq;
};

static void s6_counter_read(void *arg)
{
	struct s6_counter_sample *s = arg;

	isb();
	s->mpidr = read_sysreg(mpidr_el1);
	s->frq = read_sysreg(cntfrq_el0);
	s->phys = read_sysreg(cntpct_el0);
	s->virt = read_sysreg(cntvct_el0);
	s->selected = arch_timer_read_counter();
	s->sched = sched_clock();
	s->mono = ktime_get_ns();
}

static ssize_t counters_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct s6_counter_sample *s;
	int cpu, n = 0, ret;

	s = kcalloc(nr_cpu_ids, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		ret = smp_call_function_single(cpu, s6_counter_read, &s[cpu],
					       1);
		if (ret) {
			n = ret;
			break;
		}
		n += sysfs_emit_at(
			buf, n,
			"cpu=%d mpidr=%llx frq=%u phys=%llu virt=%llu selected=%llu sched_ns=%llu mono_ns=%llu\n",
			cpu, s[cpu].mpidr, s[cpu].frq, s[cpu].phys, s[cpu].virt,
			s[cpu].selected, s[cpu].sched, s[cpu].mono);
	}
	cpus_read_unlock();
	kfree(s);
	return n;
}

static DEVICE_ATTR_RO(counters);
static ssize_t dvfs_registers_show(struct device *dev,
				   struct device_attribute *a, char *buf)
{
	struct s6_tmu *d = dev_get_drvdata(dev);
	u32 offsets[] = { 0x100, 0x200, 0x204, 0x208, 0x400, 0x404,
			  0x408, 0x600, 0x604, 0x700, 0x704 };
	int i, n = 0;

	if (!d->apollo)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		n += sysfs_emit_at(buf, n, "%03x %08x\n", offsets[i],
				   readl(d->apollo + offsets[i]));
	return n;
}

static DEVICE_ATTR_RO(dvfs_registers);
static struct attribute *s6_attrs[] = { &dev_attr_counters.attr,
					&dev_attr_dvfs_registers.attr, NULL };
static const struct attribute_group s6_group = { .attrs = s6_attrs };
static int s6_soc_temp(struct thermal_zone_device *zone, int *temp)
{
	struct s6_tmu *d = thermal_zone_device_priv(zone);
	long t;
	int n, ret, max = -40000;

	for (n = 0; n < 4; n++) {
		ret = s6_tmu_read(d->dev, hwmon_temp, hwmon_temp_input, n, &t);
		if (ret)
			return ret;
		max = max_t(int, max, t);
	}
	*temp = max;
	return 0;
}

static int s6_cooling_max(struct thermal_cooling_device *c,
			  unsigned long *state)
{
	*state = 1;
	return 0;
}

static int s6_cooling_get(struct thermal_cooling_device *c,
			  unsigned long *state)
{
	struct s6_tmu *d = c->devdata;
	*state = READ_ONCE(d->cooling_latched);
	return 0;
}

static void s6_park_work(struct work_struct *work)
{
	struct s6_tmu *d = container_of(work, struct s6_tmu, park_work);
	int cpu, r;

	mutex_lock(&d->cooling_lock);
	if (d->cooling_latched)
		for_each_possible_cpu(cpu) {
			if (!cpu || !cpu_online(cpu))
				continue;
			if (cpu == 1 && !READ_ONCE(d->sensor_fault))
				continue;
			r = remove_cpu(cpu);
			dev_warn(d->dev, "thermal park CPU%d rc=%d\n", cpu, r);
		}
	mutex_unlock(&d->cooling_lock);
}

static int s6_cooling_set(struct thermal_cooling_device *c, unsigned long state)
{
	struct s6_tmu *d = c->devdata;

	if (state > 1 || READ_ONCE(d->stopping))
		return -EINVAL;
	mutex_lock(&d->cooling_lock);
	d->cooling_latched = state;
	mutex_unlock(&d->cooling_lock);
	/* The actuator runs on CPU0 so it can never offline its own worker. */
	if (state)
		queue_work_on(0, system_percpu_wq, &d->park_work);
	/* Relaxing cooling never automatically onlines an unvalidated core. */
	return 0;
}

static const struct thermal_cooling_device_ops s6_cooling_ops = {
	.get_max_state = s6_cooling_max,
	.get_cur_state = s6_cooling_get,
	.set_cur_state = s6_cooling_set
};

static bool s6_should_bind(struct thermal_zone_device *zone,
			   const struct thermal_trip *trip,
			   struct thermal_cooling_device *cooling,
			   struct cooling_spec *spec)
{
	struct s6_tmu *d = thermal_zone_device_priv(zone);

	if (trip->type != THERMAL_TRIP_PASSIVE)
		return false;
	if (cooling != d->cooling) {
		unsigned long max;

		if (strncmp(cooling->type, "thermal-cpufreq-", 16))
			return false;
		if (cooling->ops->get_max_state(cooling, &max))
			return false;
		spec->lower = max;
		spec->upper = max;
		spec->weight = THERMAL_WEIGHT_DEFAULT;
		return true;
	}
	spec->lower = 1;
	spec->upper = 1;
	spec->weight = THERMAL_WEIGHT_DEFAULT;
	return true;
}

/* Free the private argument vectors after the asynchronous helper exits. */
static void s6_recovery_cleanup(struct subprocess_info *info)
{
	kfree(info->argv);
	kfree(info->envp);
}

static void s6_critical(struct thermal_zone_device *zone)
{
	struct s6_tmu *d = thermal_zone_device_priv(zone);
	static char *const recovery_argv[] = {
		"/rescue/bin/busybox", "sh", "-c",
		"sync; /rescue/bin/busybox mount -o remount,ro /; /sbin/reboot-recovery",
		NULL
	};
	static char *const recovery_env[] = {
		"HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL
	};
	struct subprocess_info *info;
	char **argv, **env;
	int ret;

	if (atomic_cmpxchg(&d->recovery_requested, 0, 1))
		return;
	dev_emerg(d->dev,
		  "critical thermal limit; requesting clean TWRP recovery\n");

	/* The helper owns mutable copies while the templates remain constant. */
	argv = kmemdup(recovery_argv, sizeof(recovery_argv), GFP_KERNEL);
	env = kmemdup(recovery_env, sizeof(recovery_env), GFP_KERNEL);
	if (!argv || !env)
		goto free_vectors;

	info = call_usermodehelper_setup(argv[0], argv, env, GFP_KERNEL, NULL,
					 s6_recovery_cleanup, NULL);
	if (!info)
		goto free_vectors;

	ret = call_usermodehelper_exec(info, UMH_NO_WAIT);
	if (ret)
		dev_err(d->dev, "failed to queue thermal recovery: %d\n", ret);
	return;

free_vectors:
	kfree(argv);
	kfree(env);
	dev_err(d->dev, "cannot allocate thermal recovery arguments\n");
}

static const struct thermal_zone_device_ops s6_zone_ops = {
	.get_temp = s6_soc_temp,
	.should_bind = s6_should_bind,
	.critical = s6_critical
};

static const struct thermal_trip s6_trips[] = {
	{ .temperature = 55000,
	  .hysteresis = 5000,
	  .type = THERMAL_TRIP_PASSIVE },
	{ .temperature = 85000,
	  .hysteresis = 5000,
	  .type = THERMAL_TRIP_CRITICAL }
};

static const struct thermal_zone_params s6_zone_params = { .governor_name =
								   "step_wise",
							   .no_hwmon = true };
static void s6_zone_unregister(void *arg)
{
	thermal_zone_device_unregister(arg);
}

static void s6_guard_work(struct work_struct *work)
{
	struct s6_tmu *d =
		container_of(to_delayed_work(work), struct s6_tmu, guard);
	int ret, temp;

	if (system_state == SYSTEM_RUNNING) {
		ret = thermal_zone_get_temp(d->zone, &temp);
		WRITE_ONCE(d->sensor_fault, ret != 0);
		{
			int i;

			for (i = 0; i < 2; i++) {
				if (!d->policy[i]) {
					d->policy[i] = cpufreq_cpu_get(i);
					if (d->policy[i] &&
					    freq_qos_add_request(
						    &d->policy[i]->constraints,
						    &d->maxfreq[i],
						    FREQ_QOS_MAX,
						    FREQ_QOS_MAX_DEFAULT_VALUE) <
						    0) {
						cpufreq_cpu_put(d->policy[i]);
						d->policy[i] = NULL;
					}
				}
				if (d->policy[i] && (ret || temp >= 55000))
					freq_qos_update_request(&d->maxfreq[i],
								500000);
				else if (d->policy[i] && temp <= 50000)
					freq_qos_update_request(
						&d->maxfreq[i],
						FREQ_QOS_MAX_DEFAULT_VALUE);
			}
		}
		if (ret || temp >= 55000) {
			s6_cooling_set(d->cooling, 1);
			if (ret)
				dev_warn_ratelimited(
					d->dev,
					"invalid TMU; secondary CPUs parked\n");
		} else if (temp <= 50000)
			s6_cooling_set(d->cooling, 0);
		if (!ret && temp >= 85000)
			s6_critical(d->zone);
	}
	if (!READ_ONCE(d->stopping))
		schedule_delayed_work_on(0, &d->guard, HZ);
}

static void s6_guard_stop(void *arg)
{
	struct s6_tmu *d = arg;

	WRITE_ONCE(d->stopping, true);
	cancel_delayed_work_sync(&d->guard);
	cancel_work_sync(&d->park_work);
	{
		int i;

		for (i = 0; i < 2; i++)
			if (d->policy[i]) {
				freq_qos_remove_request(&d->maxfreq[i]);
				cpufreq_cpu_put(d->policy[i]);
			}
	}
}

static int s6_tmu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev, *hw;
	struct s6_tmu *d;
	struct clk *apb, *sclk;
	int i, ret;

	if (!of_machine_is_compatible("samsung,exynos7420-zerolte"))
		return -ENODEV;
	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	{
		struct device_node *cmu =
			of_parse_phandle(dev->of_node, "samsung,apollo-cmu", 0);
		struct resource r;

		if (!cmu)
			return -EINVAL;
		ret = of_address_to_resource(cmu, 0, &r);
		of_node_put(cmu);
		if (ret)
			return ret;
		d->apollo = devm_ioremap(dev, r.start, resource_size(&r));
		if (!d->apollo)
			return -ENOMEM;
	}
	mutex_init(&d->lock);
	mutex_init(&d->cooling_lock);
	d->dev = dev;
	platform_set_drvdata(pdev, d);
	atomic_set(&d->recovery_requested, 0);
	apb = devm_clk_get_enabled(dev, "tmu_apbif");
	if (IS_ERR(apb))
		return dev_err_probe(dev, PTR_ERR(apb), "TMU APB clock\n");
	sclk = devm_clk_get_enabled(dev, "tmu_sclk");
	if (IS_ERR(sclk))
		return dev_err_probe(dev, PTR_ERR(sclk), "TMU sample clock\n");
	for (i = 0; i < 4; i++) {
		d->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(d->base[i]))
			return PTR_ERR(d->base[i]);
		ret = s6_capture_calibration(d, i);
		if (ret)
			return dev_err_probe(dev, ret, "TMU%d P0 calibration\n",
					     i);
		dev_dbg(dev, "%s control=%08x status=%08x; observation only\n",
			s6_labels[i], readl(d->base[i] + 0x20),
			readl(d->base[i] + 0x28));
	}
	hw = devm_hwmon_device_register_with_info(dev, "s6_tmu", d,
						  &s6_tmu_info, NULL);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	INIT_WORK(&d->park_work, s6_park_work);
	d->cooling = devm_thermal_cooling_device_register(
		dev, "s6-secondary-park", d, &s6_cooling_ops);
	if (IS_ERR(d->cooling))
		return PTR_ERR(d->cooling);
	d->zone = thermal_zone_device_register_with_trips(
		"s6-soc", s6_trips, ARRAY_SIZE(s6_trips), d, &s6_zone_ops,
		&s6_zone_params, 1000, 1000);
	if (IS_ERR(d->zone))
		return PTR_ERR(d->zone);
	ret = devm_add_action_or_reset(dev, s6_zone_unregister, d->zone);
	if (ret)
		return ret;
	ret = thermal_zone_device_enable(d->zone);
	if (ret)
		return ret;
	ret = devm_device_add_group(dev, &s6_group);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&d->guard, s6_guard_work);
	ret = devm_add_action_or_reset(dev, s6_guard_stop, d);
	if (ret)
		return ret;
	schedule_delayed_work_on(0, &d->guard, 15 * HZ);
	return 0;
}

static const struct of_device_id s6_tmu_match[] = {
	{ .compatible = "samsung,zerolte-tmu-observe" },
	{}
};

MODULE_DEVICE_TABLE(of, s6_tmu_match);
static struct platform_driver s6_tmu_driver = {
	.probe = s6_tmu_probe,
	.driver = { .name = "s6-tmu-observe", .of_match_table = s6_tmu_match }
};

module_platform_driver(s6_tmu_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(
	"Zerolte firmware-initialized TMU observation, no control writes");

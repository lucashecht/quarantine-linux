#include <linux/kobject.h>
#include <linux/string.h>
#include "internal.h"

static ssize_t hypiso_sysfs_hypiso_on_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	if (*buf == '0')
		hypiso_disable();
	else if (*buf == '1')
		hypiso_enable();

	return count;
}

static ssize_t hypiso_sysfs_hypiso_on_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_on);
}

static ssize_t hypiso_sysfs_nr_host_cpus_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long nr;

	if (kstrtol(buf, 0, &nr)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (nr < 1) {
		printk("HYPISO: need at least 1 host cpu\n");
		return count;
	}

	hypiso_set_nr_host_cpus(nr);

	return count;
}

static ssize_t hypiso_sysfs_nr_host_cpus_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_nr_host_cpus);
}

static ssize_t hypiso_sysfs_nr_guest_cpus_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long nr;

	if (kstrtol(buf, 0, &nr)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (nr < 1) {
		printk("HYPISO: need at least 1 guest cpu\n");
		return count;
	}

	hypiso_set_nr_guest_cpus(nr);

	return count;
}

static ssize_t hypiso_sysfs_nr_guest_cpus_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_nr_guest_cpus);
}

static ssize_t hypiso_sysfs_core_config_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "host_cpus  %*pbl\nguest_cpus %*pbl\n",
		cpumask_pr_args(host_cpus), cpumask_pr_args(guest_cpus));
}

static ssize_t hypiso_sysfs_watchdog_interval_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long interval;

	if (kstrtol(buf, 0, &interval)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (interval < 100) {
		printk("HYPISO: watchdog interval must be at least 100ms\n");
		return count;
	}

	hypiso_watchdog_interval_ms = interval;
	printk("HYPISO: watchdog interval set to %d ms\n", hypiso_watchdog_interval_ms);

	return count;
}

static ssize_t hypiso_sysfs_watchdog_interval_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_watchdog_interval_ms);
}

static ssize_t hypiso_sysfs_scale_request_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long request;

	if (kstrtol(buf, 0, &request)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (request < -1 || request > 1) {
		printk("HYPISO: scale_request must be -1, 0, or 1\n");
		return count;
	}

	hypiso_scale_request = request;
	printk("HYPISO: scale request set to %ld\n", request);

	return count;
}

static ssize_t hypiso_sysfs_scale_request_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_scale_request);
}

static ssize_t hypiso_sysfs_window_size_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long size;

	if (kstrtol(buf, 0, &size)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (size < 1 || size > 100) {
		printk("HYPISO: window_size must be between 1 and 100\n");
		return count;
	}

	hypiso_window_size = size;
	printk("HYPISO: window size set to %ld samples\n", size);

	return count;
}

static ssize_t hypiso_sysfs_window_size_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_window_size);
}

static ssize_t hypiso_sysfs_scale_up_threshold_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long threshold;

	if (kstrtol(buf, 0, &threshold)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (threshold < 0 || threshold > 100) {
		printk("HYPISO: scale_up_threshold must be between 0 and 100\n");
		return count;
	}

	hypiso_scale_up_threshold = threshold;
	printk("HYPISO: scale up threshold set to %ld%%\n", threshold);

	return count;
}

static ssize_t hypiso_sysfs_scale_up_threshold_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_scale_up_threshold);
}

static ssize_t hypiso_sysfs_scale_down_threshold_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long threshold;

	if (kstrtol(buf, 0, &threshold)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (threshold < 0 || threshold > 100) {
		printk("HYPISO: scale_down_threshold must be between 0 and 100\n");
		return count;
	}

	hypiso_scale_down_threshold = threshold;
	printk("HYPISO: scale down threshold set to %ld%%\n", threshold);

	return count;
}

static ssize_t hypiso_sysfs_scale_down_threshold_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_scale_down_threshold);
}

static ssize_t hypiso_sysfs_consecutive_checks_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long checks;

	if (kstrtol(buf, 0, &checks)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (checks < 1 || checks > 100) {
		printk("HYPISO: consecutive_checks must be between 1 and 100\n");
		return count;
	}

	hypiso_consecutive_checks = checks;
	printk("HYPISO: consecutive checks set to %ld\n", checks);

	return count;
}

static ssize_t hypiso_sysfs_consecutive_checks_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_consecutive_checks);
}

static ssize_t hypiso_sysfs_cooldown_ms_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long cooldown;

	if (kstrtol(buf, 0, &cooldown)) {
		printk("HYPISO: parsing of '%s' as a number failed\n", buf);
		return count;
	}

	if (cooldown < 0) {
		printk("HYPISO: cooldown_ms must be non-negative\n");
		return count;
	}

	hypiso_cooldown_ms = cooldown;
	printk("HYPISO: cooldown set to %ld ms\n", cooldown);

	return count;
}

static ssize_t hypiso_sysfs_cooldown_ms_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hypiso_cooldown_ms);
}

static struct kobj_attribute hypiso_sysfs_hypiso_on = {
	.attr = {
		.name = "hypiso_on",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_hypiso_on_store,
	.show = hypiso_sysfs_hypiso_on_show,
};

static struct kobj_attribute hypiso_sysfs_nr_host_cpus = {
	.attr = {
		.name = "nr_host_cpus",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_nr_host_cpus_store,
	.show = hypiso_sysfs_nr_host_cpus_show,
};

static struct kobj_attribute hypiso_sysfs_nr_guest_cpus = {
	.attr = {
		.name = "nr_guest_cpus",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_nr_guest_cpus_store,
	.show = hypiso_sysfs_nr_guest_cpus_show,
};

static struct kobj_attribute hypiso_sysfs_core_config = {
	.attr = {
		.name = "core_config",
		.mode = S_IRUSR,
	},
	.show = hypiso_sysfs_core_config_show,
};

static struct kobj_attribute hypiso_sysfs_watchdog_interval = {
	.attr = {
		.name = "watchdog_interval_ms",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_watchdog_interval_store,
	.show = hypiso_sysfs_watchdog_interval_show,
};

static struct kobj_attribute hypiso_sysfs_scale_request = {
	.attr = {
		.name = "scale_request",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_scale_request_store,
	.show = hypiso_sysfs_scale_request_show,
};

static struct kobj_attribute hypiso_sysfs_window_size = {
	.attr = {
		.name = "window_size",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_window_size_store,
	.show = hypiso_sysfs_window_size_show,
};

static struct kobj_attribute hypiso_sysfs_scale_up_threshold = {
	.attr = {
		.name = "scale_up_threshold",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_scale_up_threshold_store,
	.show = hypiso_sysfs_scale_up_threshold_show,
};

static struct kobj_attribute hypiso_sysfs_scale_down_threshold = {
	.attr = {
		.name = "scale_down_threshold",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_scale_down_threshold_store,
	.show = hypiso_sysfs_scale_down_threshold_show,
};

static struct kobj_attribute hypiso_sysfs_consecutive_checks = {
	.attr = {
		.name = "consecutive_checks",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_consecutive_checks_store,
	.show = hypiso_sysfs_consecutive_checks_show,
};

static struct kobj_attribute hypiso_sysfs_cooldown_ms = {
	.attr = {
		.name = "cooldown_ms",
		.mode = S_IWUSR | S_IRUSR,
	},
	.store = hypiso_sysfs_cooldown_ms_store,
	.show = hypiso_sysfs_cooldown_ms_show,
};

static struct attribute *hysiso_attrs[] = {
	&hypiso_sysfs_hypiso_on.attr,
	&hypiso_sysfs_nr_host_cpus.attr,
	&hypiso_sysfs_nr_guest_cpus.attr,
	&hypiso_sysfs_core_config.attr,
	&hypiso_sysfs_watchdog_interval.attr,
	&hypiso_sysfs_scale_request.attr,
	&hypiso_sysfs_window_size.attr,
	&hypiso_sysfs_scale_up_threshold.attr,
	&hypiso_sysfs_scale_down_threshold.attr,
	&hypiso_sysfs_consecutive_checks.attr,
	&hypiso_sysfs_cooldown_ms.attr,
	NULL,
};

static struct attribute_group hysiso_attr_group = {
	.attrs = hysiso_attrs,
};

void hypiso_init_sysfs(void)
{
	int ret;
	struct kobject *hysiso_kobj = kobject_create_and_add("hypiso", kernel_kobj);

	ret = sysfs_create_group(hysiso_kobj, &hysiso_attr_group);
	if (ret)
		kobject_put(hysiso_kobj);
}

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/kernel_stat.h>
#include <linux/sched/cputime.h>
#include "internal.h"

int hypiso_watchdog_interval_ms = 1000;  // 1 second
int hypiso_scale_request = 0;  // 0 = no change, 1 = scale up, -1 = scale down

/* Utilization tracking configuration */
#define MAX_WINDOW_SIZE 100				 // Maximum samples in window
int hypiso_window_size = 10;
int hypiso_scale_up_threshold = 60;      // 60% utilization
int hypiso_scale_down_threshold = 30;    // 30% utilization
int hypiso_consecutive_checks = 3;       // indications before scaling
int hypiso_cooldown_ms = 10000;          // 10 seconds cooldown

struct hypiso_utilization_tracker {
	u64 samples[MAX_WINDOW_SIZE];       // circular buffer of utilization samples
	int head;                            // current position in buffer
	int sample_count;                    // number of samples collected
	u64 last_idle[NR_CPUS];             // previous idle time per CPU
	u64 last_total[NR_CPUS];            // previous total time per CPU
	unsigned long last_scale_jiffies;    // last scaling operation timestamp
	int consecutive_high;                // consecutive high utilization checks
	int consecutive_low;                 // consecutive low utilization checks
};

static struct hypiso_utilization_tracker tracker;

static struct task_struct *watchdog_task;
pid_t hypiso_watchdog_pid = -1;

/*
 * Get CPU times for a specific CPU.
 * Returns idle time and total time in nanoseconds.
 */
static void hypiso_get_cpu_times(int cpu, u64 *idle, u64 *total)
{
	struct kernel_cpustat kcs;
	int i;

	*idle = 0;
	*total = 0;

	kcpustat_cpu_fetch(&kcs, cpu);

	/* Calculate total time across all CPU time types */
	for (i = 0; i < NR_STATS; i++)
		*total += kcs.cpustat[i];

	*idle = kcs.cpustat[CPUTIME_IDLE] + kcs.cpustat[CPUTIME_IOWAIT];
}

/*
 * Calculate current CPU utilization across all host cores.
 * Returns average busy percentage (0-100).
 */
static u64 hypiso_calculate_utilization(void)
{
	int cpu;
	u64 total_busy = 0;
	int num_busy_cpus = 0;

	for_each_cpu(cpu, host_cpus) {
		u64 idle, total;
		u64 idle_delta, total_delta;
		u64 busy_pct;

		hypiso_get_cpu_times(cpu, &idle, &total);

		/* Calculate deltas since last measurement */
		idle_delta = idle - tracker.last_idle[cpu];
		total_delta = total - tracker.last_total[cpu];

		/* Store current values for next iteration */
		tracker.last_idle[cpu] = idle;
		tracker.last_total[cpu] = total;

		/* Calculate busy percentage for this CPU */
		if (total_delta > 0) {
			busy_pct = ((total_delta - idle_delta) * 100) / total_delta;
			total_busy += busy_pct;
			num_busy_cpus++;
		}
	}

	return num_busy_cpus > 0 ? total_busy / num_busy_cpus : 0;
}

/*
 * Add a new sample to the sliding window and return the window average.
 */
static u64 hypiso_update_window(u64 sample)
{
	u64 sum = 0;
	int i, count;

	tracker.samples[tracker.head] = sample;
	tracker.head = (tracker.head + 1) % hypiso_window_size;

	/* Track how many samples we have (while buffer is not full) */
	if (tracker.sample_count < hypiso_window_size)
		tracker.sample_count++;

	/* Calculate average of all samples in window */
	count = tracker.sample_count;
	for (i = 0; i < count; i++)
		sum += tracker.samples[i];

	return count > 0 ? sum / count : 0;
}

/*
 * Check if we should scale based on utilization and hysteresis.
 * Returns: 1 = scale up, -1 = scale down, 0 = no action
 */
static int hypiso_check_utilization(u64 avg_util)
{
	unsigned long now = jiffies;
	unsigned long cooldown_jiffies = msecs_to_jiffies(hypiso_cooldown_ms);

	/* Check cooldown period */
	if (time_before(now, tracker.last_scale_jiffies + cooldown_jiffies)) {
		/* Still in cooldown, don't scale */
		return 0;
	}

	/* Check for high utilization (scale up) */
	if (avg_util > hypiso_scale_up_threshold) {
		tracker.consecutive_high++;
		tracker.consecutive_low = 0;

		if (tracker.consecutive_high >= hypiso_consecutive_checks) {
			printk("HYPISO: High utilization detected (%llu%% > %d%%) for %d checks\n",
				avg_util, hypiso_scale_up_threshold, hypiso_consecutive_checks);
			tracker.consecutive_high = 0;
			tracker.last_scale_jiffies = now;
			return 1;  /* Scale up */
		}
	}
	/* Check for low utilization (scale down) */
	else if (avg_util < hypiso_scale_down_threshold) {
		tracker.consecutive_low++;
		tracker.consecutive_high = 0;

		if (tracker.consecutive_low >= hypiso_consecutive_checks) {
			printk("HYPISO: Low utilization detected (%llu%% < %d%%) for %d checks\n",
				avg_util, hypiso_scale_down_threshold, hypiso_consecutive_checks);
			tracker.consecutive_low = 0;
			tracker.last_scale_jiffies = now;
			return -1;  /* Scale down */
		}
	}
	/* Within normal range, reset counters */
	else {
		tracker.consecutive_high = 0;
		tracker.consecutive_low = 0;
	}

	return 0;
}

/*
 * Main utilization tracking and scaling decision logic.
 */
static void hypiso_check_and_scale(void)
{
	u64 current_util, avg_util;
	int request;
	int scale_decision;
	int ret;

	request = READ_ONCE(hypiso_scale_request);  // atomic read

	/* Calculate current utilization */
	current_util = hypiso_calculate_utilization();

	/* Update sliding window and get average */
	avg_util = hypiso_update_window(current_util);

	/* Check if we should scale */
	scale_decision = request != 0 ? request : hypiso_check_utilization(avg_util);

	if (scale_decision > 0) {
		/* Scale up */
		ret = hypiso_scale_up_host_cores();
		if (ret != 0) {
			printk("HYPISO: Failed to scale up host cores (ret=%d)\n", ret);
		} else {
			printk("HYPISO: Scaled up host cores (utilization: %llu%%)\n", avg_util);
		}
	} else if (scale_decision < 0) {
		/* Scale down */
		ret = hypiso_scale_down_host_cores();
		if (ret != 0) {
			printk("HYPISO: Failed to scale down host cores (ret=%d)\n", ret);
		} else {
			printk("HYPISO: Scaled down host cores (utilization: %llu%%)\n", avg_util);
		}
	}

	WRITE_ONCE(hypiso_scale_request, 0);
}

static int hypiso_watchdog_thread(void *data)
{
	printk("HYPISO: Watchdog thread started on CPU %d\n", smp_processor_id());

	while (!kthread_should_stop()) {
		if (hypiso_on) {
			hypiso_check_and_scale();
		}

		msleep_interruptible(hypiso_watchdog_interval_ms);
	}

	printk("HYPISO: Watchdog thread stopped\n");
	return 0;
}

void hypiso_init_watchdog(void)
{
	int cpu;

	/* Initialize utilization tracker */
	memset(&tracker, 0, sizeof(tracker));
	tracker.last_scale_jiffies = jiffies - msecs_to_jiffies(hypiso_cooldown_ms);

	/* Initialize CPU time baselines */
	for_each_possible_cpu(cpu) {
		u64 idle, total;
		hypiso_get_cpu_times(cpu, &idle, &total);
		tracker.last_idle[cpu] = idle;
		tracker.last_total[cpu] = total;
	}

	watchdog_task = kthread_create(hypiso_watchdog_thread, NULL, "hypiso-watchdog");
	if (IS_ERR(watchdog_task)) {
		printk("HYPISO: Failed to create watchdog thread\n");
		return;
	}

	hypiso_watchdog_pid = watchdog_task->pid;

	// Set CPU affinity to host CPUs
	kthread_bind_mask(watchdog_task, host_cpus);

	wake_up_process(watchdog_task);

	printk("HYPISO: Watchdog initialized, interval=%d ms, window=%d samples\n",
		hypiso_watchdog_interval_ms, hypiso_window_size);
	printk("HYPISO: Scale-up threshold: %d%%, Scale-down threshold: %d%%\n",
		hypiso_scale_up_threshold, hypiso_scale_down_threshold);
}

void hypiso_stop_watchdog(void)
{
	if (watchdog_task) {
		kthread_stop(watchdog_task);
		watchdog_task = NULL;
		printk("HYPISO: Watchdog stopped\n");
	}
}
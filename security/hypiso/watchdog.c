#include <linux/kthread.h>
#include <linux/delay.h>
#include "internal.h"

int hypiso_watchdog_interval_ms = 1000;  // 1 second
int hypiso_scale_request = 0;  // 0 = no change, 1 = scale up, -1 = scale down

static struct task_struct *watchdog_task;

static void hypiso_check_scaling(void)
{
	int request;
	int ret;

	request = READ_ONCE(hypiso_scale_request);  // atomic read
	
	/* Listen for scaling signal
	   TODO: replace with actual utilisation metric */
	if (request == 0)
		return;

	printk("HYPISO: Watchdog detected scaling request: %d\n", request);

	if (request > 0) {
		/* Scale up */
		ret = hypiso_scale_up_host_cores();
		if (ret != 0) {
			printk("HYPISO: Failed to scale up host cores (ret=%d)\n", ret);
		}
	} else if (request < 0) {
		/* Scale down */
		ret = hypiso_scale_down_host_cores();
		if (ret != 0) {
			printk("HYPISO: Failed to scale down host cores (ret=%d)\n", ret);
		}
	}

	WRITE_ONCE(hypiso_scale_request, 0);
}

static int hypiso_watchdog_thread(void *data)
{
	printk("HYPISO: Watchdog thread started on CPU %d\n", smp_processor_id());

	while (!kthread_should_stop()) {
		if (hypiso_on) {
			hypiso_check_scaling();
		}
		
		msleep_interruptible(hypiso_watchdog_interval_ms);
	}

	printk("HYPISO: Watchdog thread stopped\n");
	return 0;
}

void hypiso_init_watchdog(void)
{
	watchdog_task = kthread_create(hypiso_watchdog_thread, NULL, "hypiso-watchdog");
	if (IS_ERR(watchdog_task)) {
		printk("HYPISO: Failed to create watchdog thread\n");
		return;
	}

	hypiso_watchdog_pid = watchdog_task->pid;

    // Set CPU affinity to host CPUs
	kthread_bind_mask(watchdog_task, host_cpus);
	
	wake_up_process(watchdog_task);
	
	printk("HYPISO: Watchdog initialized, interval=%d ms\n", 
		hypiso_watchdog_interval_ms);
}

void hypiso_stop_watchdog(void)
{
	if (watchdog_task) {
		kthread_stop(watchdog_task);
		watchdog_task = NULL;
		printk("HYPISO: Watchdog stopped\n");
	}
}
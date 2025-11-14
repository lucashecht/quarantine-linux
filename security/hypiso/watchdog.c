#include <linux/kthread.h>
#include <linux/delay.h>
#include "internal.h"

int hypiso_watchdog_interval_ms = 1000;  // 1 second
int hypiso_scale_request = 0;  // 0 = no change, 1 = scale up, -1 = scale down

static struct task_struct *watchdog_task;

static void hypiso_check_scaling(void)
{
	int request;

	request = READ_ONCE(hypiso_scale_request);  // atomic read
	
	if (request == 0)
		return;

	printk("HYPISO: Watchdog detected scaling request: %d\n", request);

	if (request > 0) {
		// Scale up
		int new_nr = hypiso_nr_host_cpus + 1;
		printk("HYPISO: Scaling up host cores from %d to %d\n", 
			hypiso_nr_host_cpus, new_nr);
		
		// TODO: Add actual scaling logic here
		hypiso_set_nr_host_cpus(new_nr);
		
	} else if (request < 0) {
		// Scale down
		int new_nr = hypiso_nr_host_cpus - 1;
		if (new_nr < 1) {
			printk("HYPISO: Cannot scale down below 1 host core\n");
			WRITE_ONCE(hypiso_scale_request, 0);
			return;
		}
		
		printk("HYPISO: Scaling down host cores from %d to %d\n",
			hypiso_nr_host_cpus, new_nr);
		hypiso_set_nr_host_cpus(new_nr);
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
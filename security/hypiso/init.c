#include <linux/topology.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include "internal.h"

cpumask_var_t host_cpus;
cpumask_var_t guest_cpus;

int hypiso_on = 0;
int hypiso_nr_host_cpus = 1;
int hypiso_nr_guest_cpus = 1;

u64 hypiso_nr_vcpus = 0;
struct kvm_vcpu *hypiso_vcpus[MAX_NR_VCPUS];

DEFINE_SPINLOCK(hypiso_cpumask_lock);

/*
 * Set @target CPUs in @cpus using a small amount of cores, none of which have a
 * CPU in @taken.
 */
static int hypiso_set_cores(cpumask_var_t cpus, int target, cpumask_var_t taken)
{
	int cpu, sibling;
	struct cpumask forbidden;

	/*
	 * Mark all siblings of taken CPUs as forbidden.
	 */
	cpumask_clear(&forbidden);
	for_each_cpu(cpu, taken)
		cpumask_or(&forbidden, &forbidden, topology_sibling_cpumask(cpu));

	for_each_cpu(cpu, cpu_online_mask) {
		if (cpumask_test_cpu(cpu, &forbidden))
			continue;
		for_each_cpu(sibling, topology_sibling_cpumask(cpu)) {
			if (cpumask_weight(cpus) >= target)
				break;
			cpumask_set_cpu(sibling, cpus);
		}
	}

	if (cpumask_weight(cpus) != target)
		printk("HYPISO: there are only %d cpus available\n", cpumask_weight(cpus));

	return cpumask_weight(cpus);
}

static void hypiso_config_cores(void)
{
	cpumask_clear(host_cpus);
	cpumask_clear(guest_cpus);
	hypiso_nr_host_cpus = hypiso_set_cores(host_cpus, hypiso_nr_host_cpus, guest_cpus);
	hypiso_nr_guest_cpus = hypiso_set_cores(guest_cpus, hypiso_nr_guest_cpus, host_cpus);
}

void hypiso_init(void)
{
	zalloc_cpumask_var(&host_cpus, GFP_KERNEL);
	zalloc_cpumask_var(&guest_cpus, GFP_KERNEL);
	hypiso_config_cores();
	hypiso_init_sysfs();
	hypiso_init_watchdog();
	if (hypiso_on)
		hypiso_enable();
}

static struct task_struct *hypiso_spawn_runner(struct kvm_vcpu *vcpu)
{
	pid_t pid;
	struct task_struct *runner;
	char name[TASK_COMM_LEN];
	unsigned int old_flags = current->flags;
	unsigned long clone_flags = CLONE_FILES | CLONE_FS | CLONE_IO
				| CLONE_SIGHAND | CLONE_THREAD | CLONE_VM;

	current->flags |= PF_KTHREAD;
	pid = kernel_thread(hypiso_runner, vcpu, clone_flags);
	current->flags = old_flags;

	sched_setaffinity(pid, guest_cpus);
	runner = find_get_task_by_vpid(pid);

	snprintf(name, sizeof(name), "runner-%llu", hypiso_nr_vcpus);
	set_task_comm(runner, name);

	printk("HYPISO: %s/%d:%d@%px is creating vcpu %llu\n", current->comm,
		smp_processor_id(), current->pid, current, hypiso_nr_vcpus);

	return runner;
}

void hypiso_init_vcpu(struct kvm_vcpu *vcpu)
{
	hypiso_vcpus[hypiso_nr_vcpus] = vcpu;
	hypiso_nr_vcpus++;
	BUG_ON(hypiso_nr_vcpus > MAX_NR_VCPUS);

	get_task_struct(current);
	vcpu->owner = current;
	INIT_LIST_HEAD(&vcpu->node);
	vcpu->vmrunnable = false;
	vcpu->vmexit_pending = false;
	init_completion(&vcpu->runner_activated);

	vcpu->runner = hypiso_spawn_runner(vcpu);
}

void hypiso_set_nr_host_cpus(int new_nr_host_cpus)
{
	cpumask_clear(host_cpus);
	hypiso_nr_host_cpus = hypiso_set_cores(host_cpus, new_nr_host_cpus, guest_cpus);
}

void hypiso_set_nr_guest_cpus(int new_nr_guest_cpus)
{
	cpumask_clear(guest_cpus);
	hypiso_nr_guest_cpus = hypiso_set_cores(guest_cpus, new_nr_guest_cpus, host_cpus);
}


// TODO: check if affinity is updated after scaling (hypiso_reroute_irqs etc.)
/*
 * Scale up host cores by 1. If no unassigned CPUs are available,
 * try to repurpose a guest core first.
 * Returns 0 on success, -1 if scaling is not possible.
 */
int hypiso_scale_up_host_cores(void)
{
	int new_nr;
	unsigned long flags;

	spin_lock_irqsave(&hypiso_cpumask_lock, flags);

	new_nr = hypiso_nr_host_cpus + 1;
	if (new_nr + hypiso_nr_guest_cpus > num_online_cpus()) {
		/* No unassigned CPUs left */
		if (hypiso_nr_guest_cpus > 1) {
			/* For the prototype we assume that there's only one guest */
			/* So 1 guest CPU total is enough */
			printk("HYPISO: Cannot scale up host cores; trying to scale down guest cores first\n");

			/* Repurpose a guest core for host use */
			cpumask_clear(guest_cpus);
			hypiso_nr_guest_cpus = hypiso_set_cores(guest_cpus, hypiso_nr_guest_cpus - 1, host_cpus);
			/* TODO: vCPU affinity should be updated before the guest core is removed, to prevent
			them being scheduled on host cores */
			hypiso_isolate_vcpus(guest_cpus);

			/* TODO: add repurposing logic */
			/* TODO: microarch cleaning */
			/* This needs to trigger a function on the guest core */
		} else {
			printk("HYPISO: Cannot scale up host cores; not enough CPUs available\n");
			spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
			return -1;
		}
	}

	printk("HYPISO: Scaling up host cores from %d to %d\n", 
		hypiso_nr_host_cpus, new_nr);

	cpumask_clear(host_cpus);
	hypiso_nr_host_cpus = hypiso_set_cores(host_cpus, new_nr, guest_cpus);

	spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);

	// TODO: update watchdog affinity
	/* Re-apply isolation with new cpumasks */
	hypiso_enforce_isolation();

	return 0;
}

/*
 * Scale down host cores by 1.
 * Returns 0 on success, -EINVAL if already at minimum.
 */
int hypiso_scale_down_host_cores(void)
{
	int new_nr;
	unsigned long flags;

	spin_lock_irqsave(&hypiso_cpumask_lock, flags);

	new_nr = hypiso_nr_host_cpus - 1;
	if (new_nr < 1) {
		printk("HYPISO: Cannot scale down below 1 host core\n");
		spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
		return -EINVAL;
	}

	printk("HYPISO: Scaling down host cores from %d to %d\n",
		hypiso_nr_host_cpus, new_nr);

	cpumask_clear(host_cpus); // necessary?
	hypiso_nr_host_cpus = hypiso_set_cores(host_cpus, new_nr, guest_cpus);
	// TODO: add guest core

	spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);

	/* Re-apply isolation with new cpumasks */
	hypiso_enforce_isolation();

	return 0;
}

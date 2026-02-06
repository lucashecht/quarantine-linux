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
 * Remove one core (and all its siblings) from @cpus.
 * Returns the first CPU number of the removed core, or -1 if cpus is empty.
 */
static int hypiso_remove_one_core(cpumask_var_t cpus)
{
	int cpu, sibling;
	int first_cpu;

	printk("HYPISO: Before %*pbl\n", cpumask_pr_args(cpus));
	cpu = cpumask_first(cpus);
	printk("HYPISO: first %d\n", cpu);
	if (cpu >= nr_cpu_ids)
		return -1;

	first_cpu = cpu;
	for_each_cpu(sibling, topology_sibling_cpumask(cpu)) {
		printk("HYPISO: sibling %d\n", sibling);
		cpumask_clear_cpu(sibling, cpus);
	}

	printk("HYPISO: After  %*pbl\n", cpumask_pr_args(cpus));

	return first_cpu;
}

/*
 * Add one core (and all its siblings) to @cpus based on the specified CPU number.
 * Returns 0 on success, or -1 if the CPU is invalid.
 */
static int hypiso_add_one_core(cpumask_var_t cpus, int cpu)
{
	int sibling;

	printk("HYPISO: Before %*pbl\n", cpumask_pr_args(cpus));
	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -1;

	for_each_cpu(sibling, topology_sibling_cpumask(cpu))
		cpumask_set_cpu(sibling, cpus);

	printk("HYPISO: After  %*pbl\n", cpumask_pr_args(cpus));

	return 0;
}

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
	int repurposed_cpu;
	unsigned long flags;

	new_nr = hypiso_nr_host_cpus + 1;
	printk("HYPISO: Scaling up host cores from %d to %d...\n",
		hypiso_nr_host_cpus, new_nr);

	spin_lock_irqsave(&hypiso_cpumask_lock, flags);

	if (new_nr + hypiso_nr_guest_cpus > num_online_cpus()) {
		/* No unassigned CPUs left */
		/* Repurpose a guest core for host use */
		printk("HYPISO: Cannot scale up host cores; trying to scale down guest cores first\n");

		/* For the prototype we assume that there's only one guest */
		/* So 1 guest CPU total is the minimum */
		if (hypiso_nr_guest_cpus <= 1) {
			printk("HYPISO: Cannot scale down guest cores; not enough CPUs available\n");
			spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
			return -1;
		}

		repurposed_cpu = hypiso_remove_one_core(guest_cpus);
		if (repurposed_cpu < 0) {
			printk("HYPISO: Failed to repurpose a guest core\n");
			spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
			return -1;
		}

		hypiso_nr_guest_cpus--;
		printk("HYPISO: Repurposing guest CPU %d for host use\n", repurposed_cpu);

		/* vCPU affinity is updated before the core is added to the host pool,
		to prevent them being scheduled on a host core */
		hypiso_isolate_vcpus(guest_cpus);

		/* Clean the repurposed CPU before adding to host pool */
		hypiso_microarch_clean_cpu(repurposed_cpu);

		/* Add the repurposed guest core to host pool */
		if (hypiso_add_one_core(host_cpus, repurposed_cpu) < 0) {
			printk("HYPISO: Failed to add repurposed core to host\n");
			spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
			return -1;
		}
		hypiso_nr_host_cpus++;

	} else {
		/* TODOs:
		- Can this be made more efficient?
		- Should the microarch state be cleaned before adding the core to the host or do we assume it is clean?*/
		hypiso_set_nr_host_cpus(new_nr);
	}

	spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);

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
	int repurposed_cpu;
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

	repurposed_cpu = hypiso_remove_one_core(host_cpus);
	if (repurposed_cpu < 0) {
		printk("HYPISO: Failed to repurpose host core\n");
		spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
		return -1;
	}

	hypiso_nr_host_cpus--;
	printk("HYPISO: Repurposing host CPU %d for guest use\n", repurposed_cpu);

	/* Update affinity of host processes, IRQs, and watchdog before adding
	core to guest pool */
	hypiso_enforce_isolation(); // TODO: reduce work performed while holding lock

	/* Clean the repurposed CPU before adding to guest pool */
	hypiso_microarch_clean_cpu(repurposed_cpu);

	/* Add the repurposed host core to guest pool */
	if (hypiso_add_one_core(guest_cpus, repurposed_cpu) < 0) {
		printk("HYPISO: Failed to add repurposed core to guest\n");
		spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);
		return -1;
	}
	hypiso_nr_guest_cpus++;

	spin_unlock_irqrestore(&hypiso_cpumask_lock, flags);

	/* Re-apply isolation with new cpumask */
	hypiso_isolate_vcpus(guest_cpus);

	return 0;
}

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
int hypiso_max_cpus = 2;
int hypiso_debug = 1;

u64 hypiso_nr_vcpus = 0;
struct kvm_vcpu *hypiso_vcpus[MAX_NR_VCPUS];

static void hypiso_update_max_cpus(void)
{
	hypiso_max_cpus = hypiso_nr_host_cpus + hypiso_nr_guest_cpus;
}

static int hypiso_get_core_mask(int cpu, struct cpumask *core)
{
	int weight;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;

	cpumask_and(core, topology_sibling_cpumask(cpu), cpu_online_mask);
	weight = cpumask_weight(core);
	if (weight < 1)
		return -EINVAL;
	if (weight > 2) {
		printk("HYPISO: unsupported core with %d sibling CPUs\n", weight);
		return -EINVAL;
	}

	return weight;
}

static int hypiso_count_cores(const struct cpumask *cpus)
{
	int cpu, weight, cores = 0;
	struct cpumask core, visited;

	cpumask_clear(&visited);
	for_each_cpu(cpu, cpus) {
		if (cpumask_test_cpu(cpu, &visited))
			continue;

		weight = hypiso_get_core_mask(cpu, &core);
		if (weight < 0)
			continue;

		cpumask_or(&visited, &visited, &core);
		cores++;
	}

	return cores;
}

static bool hypiso_one_core_left(const struct cpumask *cpus)
{
	return hypiso_count_cores(cpus) <= 1;
}

/*
 * Remove one core (and all its siblings) from @cpus.
 * Returns the number of removed logical CPUs, or a negative error code.
 */
static int hypiso_remove_one_core(cpumask_var_t cpus, int *removed_cpu)
{
	int cpu, sibling, weight;
	struct cpumask core;

	hypiso_dbg("HYPISO: Before %*pbl\n", cpumask_pr_args(cpus));
	cpu = cpumask_first(cpus);
	hypiso_dbg("HYPISO: first %d\n", cpu);
	if (cpu >= nr_cpu_ids)
		return -ENOSPC;

	if (hypiso_one_core_left(cpus)) {
		printk("HYPISO: Cannot remove core, because only siblings are in the set\n");
		return -EINVAL;
	}

	weight = hypiso_get_core_mask(cpu, &core);
	if (weight < 0)
		return weight;

	*removed_cpu = cpu;
	for_each_cpu(sibling, &core) {
		hypiso_dbg("HYPISO: sibling %d\n", sibling);
		cpumask_clear_cpu(sibling, cpus);
	}

	hypiso_dbg("HYPISO: After  %*pbl\n", cpumask_pr_args(cpus));

	return weight;
}

/*
 * Add one core (and all its siblings) to @cpus based on the specified CPU number.
 * Returns the number of added logical CPUs, or a negative error code.
 */
static int hypiso_add_one_core(cpumask_var_t cpus, int cpu)
{
	int sibling, weight;
	struct cpumask core;

	hypiso_dbg("HYPISO: Before %*pbl\n", cpumask_pr_args(cpus));
	weight = hypiso_get_core_mask(cpu, &core);
	if (weight < 0)
		return weight;

	for_each_cpu(sibling, &core)
		cpumask_set_cpu(sibling, cpus);

	hypiso_dbg("HYPISO: After  %*pbl\n", cpumask_pr_args(cpus));

	return weight;
}

/*
 * Set at least @target CPUs in @cpus using whole cores, none of which have a
 * sibling CPU in @taken. Returns the actual logical CPU count selected.
 */
static int hypiso_set_cores(cpumask_var_t cpus, int target, cpumask_var_t taken)
{
	int cpu, sibling, weight;
	struct cpumask core, visited;

	cpumask_clear(&visited);

	for_each_cpu(cpu, cpu_online_mask) {
		if (cpumask_weight(cpus) >= target)
			break;
		if (cpumask_test_cpu(cpu, &visited))
			continue;

		weight = hypiso_get_core_mask(cpu, &core);
		if (weight < 0)
			continue;

		cpumask_or(&visited, &visited, &core);
		if (cpumask_intersects(&core, taken))
			continue;

		for_each_cpu(sibling, &core)
			cpumask_set_cpu(sibling, cpus);
	}

	if (cpumask_weight(cpus) < target)
		printk("HYPISO: there are only %d cpus available\n", cpumask_weight(cpus));
	else if (cpumask_weight(cpus) != target)
		printk("HYPISO: rounded CPU request %d to %d to keep whole cores\n",
			target, cpumask_weight(cpus));

	return cpumask_weight(cpus);
}

static void hypiso_config_cores(void)
{
	cpumask_clear(host_cpus);
	cpumask_clear(guest_cpus);
	hypiso_nr_host_cpus = hypiso_set_cores(host_cpus, hypiso_nr_host_cpus, guest_cpus);
	hypiso_nr_guest_cpus = hypiso_set_cores(guest_cpus, hypiso_nr_guest_cpus, host_cpus);
	hypiso_update_max_cpus();
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
	hypiso_update_max_cpus();
}

void hypiso_set_nr_guest_cpus(int new_nr_guest_cpus)
{
	cpumask_clear(guest_cpus);
	hypiso_nr_guest_cpus = hypiso_set_cores(guest_cpus, new_nr_guest_cpus, host_cpus);
	hypiso_update_max_cpus();
}


// TODO: check if affinity is updated after scaling (hypiso_reroute_irqs etc.)
/*
 * Scale up host cores by repurposing one guest core.
 * Returns 0 on success, -1 if scaling is not possible.
 */
int hypiso_scale_up_host_cores(void)
{
	int added_cpus;
	int repurposed_cpu;

	hypiso_dbg("HYPISO: Scaling up host cores from %d CPUs...\n",
		hypiso_nr_host_cpus);


	/* For the prototype we assume that there's only one guest */
	/* So 1 guest core total is the minimum */
	if (hypiso_one_core_left(guest_cpus)) {
		printk("HYPISO: Cannot scale down guest cores; not enough CPUs available\n");
		return -1;
	}

	added_cpus = hypiso_remove_one_core(guest_cpus, &repurposed_cpu);
	if (added_cpus < 0) {
		printk("HYPISO: Failed to repurpose a guest core\n");
		return -1;
	}

	hypiso_nr_guest_cpus -= added_cpus;
	hypiso_dbg("HYPISO: Repurposing guest core containing CPU %d for host use\n",
		repurposed_cpu);

	/* vCPU affinity is updated before the core is added to the host pool,
	 * to prevent them being scheduled on a host core
	 */
	hypiso_isolate_vcpus(guest_cpus);

	/* Clean the repurposed CPU before adding to host pool */
	hypiso_microarch_clean_cpu(repurposed_cpu);

	/* Add the repurposed guest core to host pool */
	added_cpus = hypiso_add_one_core(host_cpus, repurposed_cpu);
	if (added_cpus < 0) {
		printk("HYPISO: Failed to add repurposed core to host\n");
		return -1;
	}
	hypiso_nr_host_cpus += added_cpus;


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
	int moved_cpus;
	int repurposed_cpu;

	if (hypiso_one_core_left(host_cpus)) {
		//printk("HYPISO: Cannot scale down below 1 host core\n");
		return -EINVAL;
	}

	hypiso_dbg("HYPISO: Scaling down host cores from %d CPUs\n",
		hypiso_nr_host_cpus);

	moved_cpus = hypiso_remove_one_core(host_cpus, &repurposed_cpu);
	if (moved_cpus < 0) {
		printk("HYPISO: Failed to repurpose host core\n");
		return -1;
	}

	hypiso_nr_host_cpus -= moved_cpus;
	hypiso_dbg("HYPISO: Repurposing host core containing CPU %d for guest use\n",
		repurposed_cpu);

	/* Update affinity of host processes, IRQs, and watchdog before adding
	core to guest pool */
	hypiso_enforce_isolation();

	/* Clean the repurposed CPU before adding to guest pool */
	hypiso_microarch_clean_cpu(repurposed_cpu);

	/* Add the repurposed host core to guest pool */
	moved_cpus = hypiso_add_one_core(guest_cpus, repurposed_cpu);
	if (moved_cpus < 0) {
		printk("HYPISO: Failed to add repurposed core to guest\n");
		return -1;
	}
	hypiso_nr_guest_cpus += moved_cpus;


	/* Re-apply isolation with new cpumask */
	hypiso_isolate_vcpus(guest_cpus);

	return 0;
}

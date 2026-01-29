#include "internal.h"

static void hypiso_isolate_processes(const struct cpumask *cpus)
{
	struct task_struct *p, *t;

	for_each_process(p) {
		// doesn't affect watchdog
		if (p->flags & PF_KTHREAD)
			continue;
		for_each_thread(p, t) {
			if (t->flags & PF_KTHREAD)
				continue;
			sched_setaffinity(t->pid, cpus);
		}
	}
}

static void hypiso_isolate_watchdog(const struct cpumask *cpus)
{
	if (hypiso_watchdog_pid > 0) {
		sched_setaffinity(hypiso_watchdog_pid, cpus);
	}
}

void hypiso_isolate_vcpus(const struct cpumask *cpus)
{
	int i;
	struct kvm_vcpu *vcpu;

	for (i = 0; i < hypiso_nr_vcpus; i++) {
		vcpu = hypiso_vcpus[i];
		sched_setaffinity(vcpu->runner->pid, cpus);
	}
}

static void hypiso_reroute_irqs(const struct cpumask *cpus)
{
	int irq;
	for_each_active_irq(irq)
		irq_set_affinity(irq, cpus);
}

static void hypiso_start_runners(void)
{
	int i;
	struct kvm_vcpu *vcpu;

	for (i = 0; i < hypiso_nr_vcpus; i++) {
		vcpu = hypiso_vcpus[i];
		sched_setaffinity(vcpu->runner->pid, guest_cpus);
		complete(&vcpu->runner_activated);
	}
}

static void hypiso_stop_runners(void)
{
	int i;
	for (i = 0; i < hypiso_nr_vcpus; i++)
		reinit_completion(&hypiso_vcpus[i]->runner_activated);
}

void hypiso_enforce_isolation(void)
{
	if (!hypiso_on)
		return;
	hypiso_isolate_processes(host_cpus);
	hypiso_isolate_watchdog(host_cpus);
	hypiso_reroute_irqs(host_cpus);
	hypiso_isolate_vcpus(guest_cpus); // now this gets called twice
}

void hypiso_enable(void)
{
	hypiso_isolate_processes(host_cpus);
	hypiso_reroute_irqs(host_cpus);
	hypiso_start_runners();
	hypiso_on = 1;
}

void hypiso_disable(void)
{
	hypiso_on = 0;
	hypiso_stop_runners();
	hypiso_reroute_irqs(cpu_online_mask);
	hypiso_isolate_processes(cpu_online_mask);
}

void hypiso_microarch_clean_cpu(int cpu)
{
	/*Placeholder for microarchitectural state cleaning on CPU 'cpu' */
}
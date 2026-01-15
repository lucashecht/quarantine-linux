#include <linux/topology.h>
#include <linux/slab.h>
#include <linux/kvm_host.h>

#ifndef __HYPISO_INTERNAL_H__
#define __HYPISO_INTERNAL_H__

#define MAX_NR_VCPUS 128
extern cpumask_var_t host_cpus;
extern cpumask_var_t guest_cpus;
extern int hypiso_on;
extern int hypiso_nr_host_cpus;
extern int hypiso_nr_guest_cpus;
extern u64 hypiso_nr_vcpus;
extern struct kvm_vcpu *hypiso_vcpus[MAX_NR_VCPUS];
extern spinlock_t hypiso_cpumask_lock;
void hypiso_set_nr_host_cpus(int new_nr_host_cpus);
void hypiso_set_nr_guest_cpus(int new_nr_guest_cpus);
int hypiso_scale_up_host_cores(void);
int hypiso_scale_down_host_cores(void);

void hypiso_enforce_isolation(void);
void hypiso_enable(void);
void hypiso_disable(void);

void hypiso_host_cpu_init(int cpu);

void hypiso_guest_cpu_init(int cpu);
int hypiso_runner(void *data);

void hypiso_init_sysfs(void);

// Scaling watchdog
void hypiso_init_watchdog(void);
void hypiso_stop_watchdog(void);
extern int hypiso_watchdog_interval_ms;
extern int hypiso_scale_request;  /* 0 = no change, 1 = scale up, -1 = scale down */

#endif /* __HYPISO_INTERNAL_H__ */

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
extern int hypiso_max_cpus;
extern u64 hypiso_nr_vcpus;
extern struct kvm_vcpu *hypiso_vcpus[MAX_NR_VCPUS];
void hypiso_set_nr_host_cpus(int new_nr_host_cpus);
void hypiso_set_nr_guest_cpus(int new_nr_guest_cpus);
int hypiso_scale_up_host_cores(void);
int hypiso_scale_down_host_cores(void);

void hypiso_isolate_vcpus(const struct cpumask *cpus);
void hypiso_enforce_isolation(void);
void hypiso_enable(void);
void hypiso_disable(void);
void hypiso_microarch_clean_cpu(int cpu);

void hypiso_host_cpu_init(int cpu);

void hypiso_guest_cpu_init(int cpu);
int hypiso_runner(void *data);

void hypiso_init_sysfs(void);

// Scaling watchdog
extern pid_t hypiso_watchdog_pid;
void hypiso_init_watchdog(void);
void hypiso_stop_watchdog(void);
extern int hypiso_watchdog_interval_ms;
extern int hypiso_scale_request;  // 0 = no change, 1 = scale up, -1 = scale down
extern int hypiso_window_size;
extern int hypiso_scale_up_threshold;
extern int hypiso_scale_down_threshold;
extern int hypiso_consecutive_checks;
extern int hypiso_cooldown_ms;

// Scaling-latency instrumentation published via sysfs
extern u64 hypiso_last_scale_ns;
extern int hypiso_last_scale_dir;  // +1 = scaled up, -1 = scaled down, 0 = none
extern int hypiso_last_scale_ret;  // return code of the last scaling operation
extern u64 hypiso_scale_seq;       // incremented after each measured operation

extern int hypiso_debug;
#define hypiso_dbg(fmt, ...) \
	do { if (hypiso_debug) printk(fmt, ##__VA_ARGS__); } while (0)

#endif /* __HYPISO_INTERNAL_H__ */

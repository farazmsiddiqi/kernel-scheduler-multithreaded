#ifndef __MP2_GIVEN_INCLUDE__
#define __MP2_GIVEN_INCLUDE__

#include <linux/pid.h>
#include <linux/sched.h>

struct task_struct* find_task_by_pid(unsigned int nr)
{
        struct task_struct* task;
        rcu_read_lock();
        task=pid_task(find_vpid(nr), PIDTYPE_PID);
        rcu_read_unlock();
        return task;
}

ssize_t read_callback(struct file *, char __user * , size_t, loff_t *);
ssize_t write_callback(struct file *, const char __user *, size_t, loff_t *);

int register_node(struct file * fp, char* buffer, size_t n, loff_t * seek);
struct linked_list* create_registration_node(pid_t pid, unsigned long period, unsigned long processing_time);
void deregister_node(struct file* fp, char* buffer, size_t n, loff_t* seek);
void yield_node(struct file * fp, char* buffer, size_t n, loff_t * seek);
bool can_admit(pid_t pid, unsigned long processing_time, unsigned long period);

struct linked_list* find_node_in_ll(pid_t pid);

void timer_callback(struct timer_list * data);
int kthread_fn(void* pv);

void del_node(struct list_head* node);
void delete_linked_list(void);

#endif

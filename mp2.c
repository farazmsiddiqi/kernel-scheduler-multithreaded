#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <uapi/linux/sched/types.h>
#include <linux/jiffies.h>
#include "mp2_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_ID");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1
#define READY 1
#define RUNNING 2
#define SLEEPING 3
#define STATUS "status"
#define MP2_PROC_NAME "mp2"

static const struct proc_ops proc_fops = {
   .proc_read = read_callback,
   .proc_write = write_callback,
};

static struct proc_dir_entry *proc_parent;
static struct proc_dir_entry *proc_status;

struct linked_list {
   struct list_head head;
   struct task_struct* linux_task;
   struct timer_list wakeup_timer;
   pid_t pid;
   unsigned long period;
   unsigned long processing_time;
   unsigned int state;
   // TODO: use deadline_jiff
   unsigned long deadline_jiff;
};

struct linked_list* cur_running_task;
static struct task_struct* kthread;
struct kmem_cache* km_cache;
struct linked_list ll_head;
bool is_head_init = false;


// DEFINE_MUTEX(ll_lock);
DEFINE_SPINLOCK(ll_sp_lock);

ssize_t read_callback(struct file * fp, char __user * usr_str, size_t n, loff_t * seek) {
   char *buffer = kzalloc(n, GFP_KERNEL);
   struct list_head *iter;
   int bytes_written = 0;
   size_t buf_len;
   size_t num_bytes_NOT_copied;

   spin_lock(&ll_sp_lock);
   if (!is_head_init) {
      printk(KERN_INFO "read_callback called when linked list head not initialized.");
   } else {
      list_for_each(iter, &ll_head.head) {
         pid_t pid = list_entry(iter, struct linked_list, head)->pid;
         long period = list_entry(iter, struct linked_list, head)->period;
         long processing_time = list_entry(iter, struct linked_list, head)->processing_time;
         bytes_written += sprintf(buffer + bytes_written, "%d: %ld, %ld\n", pid, period, processing_time);
         printk(KERN_INFO "wrote %d: %ld, %ld to buffer\n", pid, period, processing_time);
      }
   }
   spin_unlock(&ll_sp_lock);

   buf_len = strlen(buffer);

   if (*seek >= buf_len || *seek > 0) {
      return 0; // because you didnt copy anything in this case
   }

   // can replace with simple read
   num_bytes_NOT_copied = copy_to_user(usr_str, buffer, buf_len);
   if (num_bytes_NOT_copied < 0) {
      seek = seek + (n - num_bytes_NOT_copied);
      printk(KERN_ALERT "buffer not fully copied to usr_str on read_callback!");
      return -ENOMEM;
   }

   kfree(buffer);
   *seek += buf_len - num_bytes_NOT_copied;

   return buf_len - num_bytes_NOT_copied;
}

ssize_t write_callback(struct file * fp, const char __user * usr_str, size_t n, loff_t * seek) {
   char buffer[250] = {0};
   size_t offset;
   int ret = 0;
   *seek = 0;

   offset = simple_write_to_buffer(buffer, 250, seek, usr_str, n);
   printk(KERN_INFO "Write callback triggered. Recieved input from user: %s", buffer);

   if (buffer[0] == 'R') {
      // on registration of process, add to linked list
      printk(KERN_INFO "recieved R (registration) in write_callback");
      ret = register_node(fp, buffer, n, seek);
   } else if (buffer[0] == 'Y') {
      printk(KERN_INFO "recieved Y (yield) in write_callback");
      yield_node(fp, buffer, n, seek);

   } else if (buffer[0] == 'D') {
      // on deregistration, remove from linked list
      printk(KERN_INFO "recieved D (de-registration) in write_callback");
      deregister_node(fp, buffer, n, seek);
   }
   
   if (ret)
      return ret;

   return offset;
}


void yield_node(struct file * fp, char* buffer, size_t n, loff_t * seek) {
   // get pid of process to be yielded
   char c;
   pid_t pid;
   struct linked_list* node;
   size_t vars_populated = sscanf(buffer, "%c, %d", &c, &pid);
   unsigned long timer_duration_jiffies;

   if (vars_populated != 2) {
      printk(KERN_ALERT "DEREG SSCANF FAILURE: variables processed=%zu", vars_populated);
   }
   printk(KERN_INFO "variables processed. pid: %d", pid);

   // find PID of process in LL
   node = find_node_in_ll(pid);
   
   if (!node) {
      printk(KERN_INFO "The node queried for does not exist in linked list.");
      return;
   }

   // setup wakeup timer
   spin_lock(&ll_sp_lock); // pot remove

   //timer_duration = deadline - jiffies
   node->deadline_jiff = node->deadline_jiff + node->period;
   timer_duration_jiffies = node->deadline_jiff - jiffies;

   printk(KERN_INFO "mod_timer");
   mod_timer(&node->wakeup_timer, jiffies + timer_duration_jiffies); // "wake yielded task up at the end of the period."
   spin_unlock(&ll_sp_lock);
   
   // wakeup dispatching thread
   printk(KERN_INFO "waking up kthread");
   // could this be why rmmod is hanging? kthread is not ending properly.
   spin_lock(&ll_sp_lock);
   wake_up_process(kthread);
   spin_unlock(&ll_sp_lock);

   // set current process to sleep
   printk(KERN_INFO "sleeping yielded process");
   set_current_state(TASK_UNINTERRUPTIBLE);
   printk(KERN_INFO "calling schedule");
   schedule();
}


void timer_callback(struct timer_list * data) {
   // when timer expires
   struct linked_list *tmp;

   spin_lock(&ll_sp_lock);
   // printk(KERN_INFO "timer_callback triggered.");
   tmp = list_entry(data, struct linked_list, wakeup_timer);

   
   tmp->state = READY;
   printk(KERN_INFO "T%d Changed state in timer_callback to READY.", tmp->pid);
   spin_unlock(&ll_sp_lock);
   
   // how to set state of sleeping process to READY in timer_callback?
   wake_up_process(kthread);
}


int kthread_fn(void* pv) {

   // do scheduling logic. iterate through ready tasks, pick task to run.
   pid_t highest_priority_task_pid;
   unsigned long smallest_period = 0xffffffff;
   struct sched_attr attr;
   struct list_head *iter, *tmp;
   struct task_struct* chosen_task = NULL;
   struct linked_list* cur_node = NULL;
   struct linked_list* highest_priority_node = NULL;

   printk(KERN_INFO "entered kthread function");

   /* 
   On first runthrough of kthread fn from yield, there are no tasks in ready state.
   Hence, this statement is triggered. After the timer callback fires, THEN a process
   will be moved to ready state, which allows kthread to enter scheduling loop.
   */
   // printk(KERN_INFO "checking empty hpn case.");
   // printk(KERN_INFO "highest_priority_node: %llx", (unsigned long long)highest_priority_node);
   if (highest_priority_node == NULL) {
      set_current_state(TASK_INTERRUPTIBLE);
      printk(KERN_INFO "slept kthread. calling schedule after kthread.");
      schedule();
   }

   while (!kthread_should_stop()) {
      printk(KERN_INFO "entered while loop");
      if (!is_head_init) {
         printk(KERN_INFO "Head is not initialized.");
         return 0;
      }

      // hpn = null
      // sleep kthread

      highest_priority_node = NULL;
      highest_priority_task_pid = 0;
      smallest_period = 0xffffffff;
      cur_node = NULL;
      chosen_task = NULL;
      memset(&attr, 0, sizeof(struct sched_attr));

      spin_lock(&ll_sp_lock);
      // find pid of task to run next
      //printk(KERN_INFO "finding pid of task to run next");
      //printk(KERN_INFO "BEFORE LOOP: current smallest period: %ld", smallest_period);
      list_for_each_safe(iter, tmp, &ll_head.head) {
         //printk(KERN_INFO "evaluating node");
         cur_node = list_entry(iter, struct linked_list, head);
         printk(KERN_INFO "evaluating node with pid: %d, period: %ld, STATE: %d", cur_node->pid, cur_node->period, cur_node->state);
         //printk(KERN_INFO "IN LOOP: current smallest period: %ld", smallest_period);

         // period denotes priority in RMS, so a lower period is a higher priority
         if (cur_node->state == READY && cur_node->period < smallest_period) {
            printk(KERN_INFO "found READY task in LL with PID: %d", cur_node->pid);
            smallest_period = cur_node->period;
            // printk(KERN_INFO "CHANGED: current smallest period: %ld", smallest_period);
            highest_priority_task_pid = cur_node->pid;
            highest_priority_node = cur_node;
         }
      }

      // try new lock to lock only cur running task changes
      
      printk(KERN_INFO "beginning 4 case analysis");
      if (highest_priority_node && cur_running_task) {
         // if ready task exists and curtask exists --> preempt curtask and run ready task
         printk(KERN_INFO "entered case 1");

         // preempt curtask: set curtask to READY
         if (cur_running_task->state == RUNNING) {
            chosen_task = find_task_by_pid(cur_running_task->pid);
            attr.sched_policy = SCHED_NORMAL;
            attr.sched_priority = 0;
            //printk(KERN_INFO "Ready task exists, curtask exists");
            // null ptr deref issue
            sched_setattr_nocheck(chosen_task, &attr);
            set_current_state(TASK_UNINTERRUPTIBLE);
            cur_running_task->state = READY;
            printk(KERN_INFO "T%d put curtask back in ready state", cur_running_task->pid);

         }

         // run ready task: ctx switch to chosen_task. set the chosen_task state to running. set priority of task.
         printk(KERN_INFO "waking up chosen task");
         chosen_task = find_task_by_pid(highest_priority_task_pid);
         wake_up_process(chosen_task);
         //printk(KERN_INFO "setting cur_running_task");
         cur_running_task = highest_priority_node;
         printk(KERN_INFO "T%d setting running task state to running", highest_priority_task_pid);
         cur_running_task->state = RUNNING;
         //printk(KERN_INFO "changing sched policy to FIFO");
         attr.sched_policy = SCHED_FIFO;
         //printk(KERN_INFO "setting priority 99");
         attr.sched_priority = 99;

         // printk(KERN_INFO "Ready task exists, curtask does not exist");
         sched_setattr_nocheck(chosen_task, &attr);
         //printk(KERN_INFO "running chosen task");
         

      } else if (highest_priority_node && !cur_running_task) {
         printk(KERN_INFO "entered case 2");
         // if ready task exists and curtask does not exist --> run ready task, set curtask pointer to ready task

         // run ready task: ctx switch to chosen_task. set the chosen_task state to running. set priority of task.
         //printk(KERN_INFO "waking up chosen task");
         chosen_task = find_task_by_pid(highest_priority_task_pid);
         wake_up_process(chosen_task);
         //printk(KERN_INFO "setting cur_running_task");
         cur_running_task = highest_priority_node;
         printk(KERN_INFO "T%d setting running task state to running", highest_priority_task_pid);
         cur_running_task->state = RUNNING;
        // printk(KERN_INFO "changing sched policy to FIFO");
         attr.sched_policy = SCHED_FIFO;
         //printk(KERN_INFO "setting priority 99");
         attr.sched_priority = 99;

         //printk(KERN_INFO "Ready task exists, curtask does not exist");
         sched_setattr_nocheck(chosen_task, &attr);
         //printk(KERN_INFO "running chosen task");
         

      } else if (!highest_priority_node && cur_running_task) {
         printk(KERN_INFO "entered case 3");
         // if ready task does not exist and curtask exists --> keep running curtask

         // KEEP RUNNING CURRENT TASK
         // run ready task: ctx switch to chosen_task. set the chosen_task state to running. set priority of task.
         if (!(cur_running_task->state == RUNNING)) {
            //printk(KERN_INFO "waking up chosen task");
            chosen_task = find_task_by_pid(cur_running_task->pid);
            wake_up_process(chosen_task);
            //printk(KERN_INFO "setting running task state to running");
            cur_running_task->state = RUNNING;
            //printk(KERN_INFO "changing sched policy to FIFO");
            attr.sched_policy = SCHED_FIFO;
            //printk(KERN_INFO "setting priority 99");
            attr.sched_priority = 99;

            // printk(KERN_INFO "Ready task exists, curtask does not exist");
            sched_setattr_nocheck(chosen_task, &attr);
            //printk(KERN_INFO "running chosen task");
            // panic("Boom");
         }

      } else {
         // if ready task does not exist and curtask does not exist --> will never happen (??)
         printk(KERN_ALERT "case 4:");
      }
      
      printk(KERN_INFO "sleeping kthread. calling schedule after kthread.");
      set_current_state(TASK_INTERRUPTIBLE);
      spin_unlock(&ll_sp_lock);
      schedule();
   }
   
   return 0;
}


struct linked_list* find_node_in_ll(pid_t pid) {
   struct list_head *iter, *tmp;

   if (!is_head_init) {
      printk(KERN_INFO "Head is not initialized. Failed to find node with pid: %d in linked list.", pid);
      // NULL return is checked for later on.
      return NULL;
   }

   list_for_each_safe(iter, tmp, &ll_head.head) {
      struct linked_list* cur_node = list_entry(iter, struct linked_list, head);
      if (pid == cur_node->pid) {
         return cur_node;
      }
   }

   printk(KERN_INFO "Failed to find node with pid: %d in linked list.", pid);
   
   // NULL return is checked for later on.
   return NULL;
}


void deregister_node(struct file* fp, char* buffer, size_t n, loff_t* seek) {
   // get pid of process to be deregistered
   char c;
   pid_t pid;
   size_t vars_populated = sscanf(buffer, "%c, %d", &c, &pid);
   struct list_head *iter, *tmp;

   if (vars_populated != 2) {
      printk(KERN_ALERT "DEREG SSCANF FAILURE: variables processed=%zu", vars_populated);
   }
   printk(KERN_INFO "variables processed. pid: %d", pid);

   if (!is_head_init) {
      return;
   }

   // find node, delete it.
   spin_lock(&ll_sp_lock);
   list_for_each_safe(iter, tmp, &ll_head.head) {
      struct linked_list* cur_node = list_entry(iter, struct linked_list, head);
      if (pid == cur_node->pid) {
         del_node(iter);
         printk(KERN_INFO "Deregistered node with pid: %d", pid);
         spin_unlock(&ll_sp_lock);
         wake_up_process(kthread);
         return;
      }
   }

   spin_unlock(&ll_sp_lock);
   wake_up_process(kthread);
   // printk(KERN_INFO "Failed to find node with pid: %d in linked list.", pid);
}


int register_node(struct file * fp, char* buffer, size_t n, loff_t * seek) {
   // parse usr_str to get pid, period, process_time
   char c;
   pid_t pid;
   unsigned long period;
   unsigned long processing_time;
   struct linked_list* ll_entry;
   size_t vars_populated = sscanf(buffer, "%c, %d, %ld, %ld", &c, &pid, &period, &processing_time);

   if (vars_populated != 4) {
      printk(KERN_ALERT "REG SSCANF FAILURE: variables processed=%zu", vars_populated);
   }
   printk(KERN_INFO "variables processed. pid: %d, period: %ld, process time: %ld", pid, period, processing_time);

   if (!can_admit(period, processing_time, period)) {
      // if you cannot admit the node: dont register the node
      printk(KERN_INFO "cannot register node with pid: %d due to admission control requirements.", pid);
      return -EINVAL;
   }

   ll_entry = create_registration_node(pid, period, processing_time);
   ll_entry->state = SLEEPING;

   spin_lock(&ll_sp_lock);
   list_add_tail(&ll_entry->head, &ll_head.head);
   spin_unlock(&ll_sp_lock);

   return 0;
}


bool can_admit(pid_t pid, unsigned long processing_time, unsigned long period) {
   struct list_head *iter, *tmp;
   unsigned long factor = 10000000;
   unsigned long c_over_p = 0;

   if (!is_head_init) {
      return true;
   }

   list_for_each_safe(iter, tmp, &ll_head.head) {
      struct linked_list* cur_node = list_entry(iter, struct linked_list, head);
      c_over_p = c_over_p + ( (cur_node->processing_time * factor) / (cur_node->period) );
   }

   c_over_p += ( (processing_time * factor) / (period) );
   printk("c_over_p=%ld", c_over_p);
   printk(KERN_INFO "RHS: %ld", (693 * factor/1000));
   return c_over_p <= (693 * factor/1000);
}


struct linked_list* create_registration_node(pid_t pid, unsigned long period, unsigned long processing_time) {
   struct linked_list* ll_entry;
   spin_lock(&ll_sp_lock);

   if (!is_head_init) {
      INIT_LIST_HEAD(&ll_head.head); // move this to init function, then ur flag is no longer needed
      printk(KERN_INFO "initialized ll_head.head");
      is_head_init = true;
   }

   spin_unlock(&ll_sp_lock);

   // if head already initialized, add a node right before ll_head
   printk(KERN_INFO "adding additional node to linked list");
   ll_entry = kmem_cache_alloc(km_cache, GFP_KERNEL); // use slab allocator
   ll_entry->pid = pid;
   ll_entry->period = period;
   ll_entry->processing_time = processing_time;
   ll_entry->deadline_jiff = jiffies + msecs_to_jiffies(period);
   ll_entry->linux_task = NULL;
   timer_setup(&ll_entry->wakeup_timer, timer_callback, 0);

   return ll_entry;
}


void delete_linked_list(void) {
   struct list_head *iter, *tmp;
   if (!is_head_init) {
      return;
   }
   
   spin_lock(&ll_sp_lock);
   list_for_each_safe(iter, tmp, &ll_head.head) {
      del_node(iter);
   }
   spin_unlock(&ll_sp_lock);
}


void del_node(struct list_head* node) {
   struct linked_list* cur_node;
   pid_t deleted_pid;

   if (!is_head_init) {
      return;
   }

   // del the rest of the struct
   cur_node = list_entry(node, struct linked_list, head);

   if (cur_node->pid == cur_running_task->pid) {
      cur_running_task = NULL;
   }
   
   list_del(node);
   deleted_pid = cur_node->pid;
   del_timer(&cur_node->wakeup_timer);
   kmem_cache_free(km_cache, cur_node);
   cur_node = NULL;

   printk(KERN_INFO "deleted node with pid %d", deleted_pid);
}


// mp2_init - Called when module is loaded
int __init mp2_init(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP2 MODULE LOADING\n");
   #endif
   // Insert your code here ...
   printk(KERN_INFO "hello world");

   // init MP2 dir
   proc_parent = proc_mkdir(MP2_PROC_NAME, NULL);
   if (!proc_parent) {
   printk(KERN_ALERT "Failed to initialize /proc/mp2/ dir");
   return EPERM;
   }

   // init status file in /proc/mp2/
   proc_status = proc_create(STATUS, 0666, proc_parent, &proc_fops);
   if (!proc_status) {
   printk(KERN_ALERT "Failed to initialize status file");
   return EPERM;
   }

   // define static kmem_cache struct, have it capture output of macro
   printk(KERN_INFO "initializing kmem cache");
   km_cache = KMEM_CACHE(linked_list, SLAB_POISON);

   // init dispatcher thread kthread
   kthread = kthread_create(kthread_fn, NULL, "Dispatcher Thread");
   cur_running_task = NULL;

   printk(KERN_ALERT "MP2 MODULE LOADED\n");
   return 0;
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP2 MODULE UNLOADING\n");
   #endif
   // Insert your code here ...

   printk(KERN_INFO "stopping dispatcher thread (kthread)");
   kthread_stop(kthread);

   printk(KERN_INFO "removing /proc/mp2/status file");
   remove_proc_entry(STATUS, proc_parent);
   printk(KERN_INFO "removing /proc/mp2/ dir");
   remove_proc_entry(MP2_PROC_NAME, NULL);

   printk(KERN_INFO "deleting linked list");
   delete_linked_list();

   printk(KERN_INFO "destroying km_cache");
   kmem_cache_destroy(km_cache);

   // printk(KERN_INFO "destroying mutex lock");
   // mutex_destroy(&ll_lock);

   printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);


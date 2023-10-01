# mp2-farazmsiddiqi

### Summary
On init, the kernel module creates a `/proc/mp2` directory is created with a file, `status`, inside. 

```C
int __init mp2_init(void)
```

A slab allocator and kthread are initialized as well. The data structure actually keeping track of each PID's information is a kernel linked list. I implemented it using the kernel linked list API. This data structure will be allocated using the slab allocator.

```C
km_cache = KMEM_CACHE(linked_list, SLAB_POISON);
kthread = kthread_create(kthread_fn, NULL, "Dispatcher Thread");
```

There are three functions that handle Registration, Yield, and Deregistration of a node. Each function is kicked off by the write callback. When the module recieves any of these commands, their respective functions handle the operations that must be done with the PID. The register function adds a node to my linked list with metadata for the period, processing time, and other information for a process. The yield function sleeps the task and calls a custom scheduler (through the kthread) to schedule a new task. The deregister function deletes the node from my linked list.

```C
void register_node(struct file * fp, char* buffer, size_t n, loff_t * seek);
void yield_node(struct file * fp, char* buffer, size_t n, loff_t * seek);
void deregister_node(struct file* fp, char* buffer, size_t n, loff_t* seek);
```

When a user tries to write to the `/proc/mp2/status` file, my `write_callback` function is triggered. This function is customized to write the user buffer into my internal kernel linked list.

```C
ssize_t write_callback(struct file * fp, const char __user * usr_str, size_t n, loff_t * seek)
```

Any addition to the linked list must be threadsafe:

```C
   mutex_lock(&ll_lock);
   list_add_tail(&ll_entry->head, &ll_head.head);
   mutex_unlock(&ll_lock);
```

When the user tries to read from the `/proc/mp2/status` file, my `read_callback` function is triggered. This function iterates through my internal kernel linked list and displays PIDs in a specific format to the console.

```C
ssize_t read_callback(struct file * fp, char __user * usr_str, size_t n, loff_t * seek)
```

The `read_callback` iterates through the linked list as well:
```C
list_for_each(iter, &ll_head.head) {
     pid_t node_pid = list_entry(iter, struct linked_list, head)->pid;
     long node_user_time = list_entry(iter, struct linked_list, head)->user_time;
     bytes_written += sprintf(buffer + bytes_written, "%d: %ld\n", node_pid, node_user_time);
     printk(KERN_INFO "wrote PID:%d to buffer", node_pid);
}
```

On Yield, the function tells a thread to go to sleep, and kicks off a timer function and a kthread. The timer function wakes up the kthread and sets the PID state to ready. The kthread iterates through the linked list to find the ready task with the highest priority and chooses that to run next. 

```C
      list_for_each_safe(iter, tmp, &ll_head.head) {
         cur_node = list_entry(iter, struct linked_list, head);

         // period denotes priority in RMS, so a lower period is a higher priority
         if (cur_node->state == READY && cur_node->period < smallest_period) {
            smallest_period = cur_node->period;
            highest_priority_task_pid = cur_node->pid;
            highest_priority_node = cur_node;
         }
      }
```

On exit, the kernel module deletes the kernel linked list it was using to store PID information. It destroys the timer, locks, slab allocator, and kthread as well. All other data structures are destroyed.
```C
void __exit mp2_exit(void)
```

### User Interaction With Proc File System
The `/proc` folder refers to a filesystem that is created by Linux on boot, and is used to store information about various system
metadata. This MP was harder than the last because the kthread had to sleep at the right time and ensure that locks were put in the right place. My kernel module creates a new sub-directory in the `/proc`
filesystem on initialization, and adds a `status` file to the new subdirectory. The user is able to `cat` my `/proc/status` file to see the contents of the file.
When the `cat` command is called on my status file, the system calls my `read_callback` function. 
I implemented this function to define custom behaviour for the file when it is read. 
This is because I want to display specific information in a singular format. 
I implemented a custom version of this function to write PIDs and the user time of each PID in a specific format. 
The user can send Register, Yield and Deregister commands through the userapp. These commands are then handled by different functions.

### Storing Process Information Using Kernel List
I utilized the kernel linked list API to implement a linked list that holds a process PID and a bunch of other metadata.
This implementation allows the user to have custom struct functionality while efficiently utilizing the provided linked list methods. 
I add a node to the list when the user registers a node via the `/proc/mp2/status` file, and I delete a node from the list when a deregister command is recieved or the module is removed from the kernel. 

This function deletes my linked list on exit:
```C
void delete_linked_list(void)
```

#define LINUX
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>  
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>      
#include <linux/sched.h>
#include <linux/sched/types.h>    
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include "mp2_given.h"

// !!!!!!!!!!!!! IMPORTANT !!!!!!!!!!!!!
// Please put your name and email here
MODULE_AUTHOR("Kaiyang Teng <kteng4@illinois.edu>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CS-423 MP2");


#define SLEEPING 0
#define READY    1
#define RUNNING  2
// #define DEBUG 1

static struct proc_dir_entry *mp2dir;
static struct proc_dir_entry *mp2status;
static struct list_head mp2listhead; 
static struct kmem_cache *mp2cache;
static struct mutex mp2mutex;
static struct mp2pcb *currtask; 
static struct task_struct *dispathread;



struct mp2pcb
{
	pid_t pid;
	unsigned long comp_time,period,nxt_wake_up_time;
	atomic_t RMSstate;
	struct task_struct *linux_task;
	struct list_head node;
	struct timer_list timer;
};


// Raise a task to real-time priority and wake it for execution.
void promote_task(struct task_struct *intask)
{
    struct sched_attr attr={0};
	if(!intask) return;
    attr.sched_policy=SCHED_FIFO;
    attr.sched_priority=98;////////////////////////////////////
    sched_setattr_nocheck(intask,&attr);
    wake_up_process(intask);
}


// Restore a task to normal scheduling policy.
void demote_task(struct task_struct *intask)
{
    struct sched_attr attr={0};
	if(!intask) return;
    attr.sched_policy=SCHED_NORMAL;
    attr.sched_priority=0;
    sched_setattr_nocheck(intask,&attr);
}


// Select the highest-priority READY task and update scheduling state.
void dispatcher(void)
{
	struct mp2pcb*curr,*nexttask=NULL;
	struct task_struct *protaskptr=NULL,*demtaskptr=NULL;
	mutex_lock(&mp2mutex);

	// list_for_each_entry(curr,&mp2listhead,node)
    // {
    //     if(curr->RMSstate==SLEEPING&&jiffies>=curr->nxt_wake_up_time) 
    //     {
    //         curr->RMSstate=READY;
    //     }
    // }
	list_for_each_entry(curr,&mp2listhead,node)
	{
		if(atomic_read(&curr->RMSstate)!=READY) continue;
		if(!nexttask||curr->period<nexttask->period) nexttask=curr;
	}
	if(!currtask||atomic_read(&currtask->RMSstate)==SLEEPING)
	{
		if(!nexttask) 
		{
			mutex_unlock(&mp2mutex);
			return;
		}
		currtask=nexttask;
		atomic_set(&currtask->RMSstate,RUNNING);
		protaskptr=currtask->linux_task;
	}
	else if(atomic_read(&currtask->RMSstate)==RUNNING)
	{
		if(!nexttask) 
		{
			mutex_unlock(&mp2mutex);
			return;
		}
		if(currtask->period>nexttask->period)
		{
			demtaskptr=currtask->linux_task;
			atomic_set(&currtask->RMSstate,READY);
			currtask=nexttask;
			atomic_set(&currtask->RMSstate,RUNNING);
			protaskptr=currtask->linux_task;
		}
	}
	mutex_unlock(&mp2mutex);
	if(protaskptr) promote_task(protaskptr);
	if(demtaskptr) demote_task(demtaskptr);
}

// Wake up the dispatcher thread if it is sleeping.
void wakethread(void)
{
    if(dispathread) wake_up_process(dispathread);
}

// Main loop of the dispatcher kernel thread.
int threadfunc(void *data)
{
	struct sched_attr attr={0};
    attr.sched_policy=SCHED_FIFO;
    attr.sched_priority=99; 
    sched_setattr_nocheck(current, &attr);
	while(!kthread_should_stop())
	{
		dispatcher();
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) 
		{   
			__set_current_state(TASK_RUNNING); 
			break;                     
    	}
		schedule();
		__set_current_state(TASK_RUNNING);
	}
	return 0;
}

// Timer callback that marks a sleeping task as READY and wakes up the dispatcher.
void timercb(struct timer_list *intimer)
{
	struct mp2pcb*temp=from_timer(temp,intimer,timer);
	atomic_set(&temp->RMSstate,READY);
	wakethread();
}

// Register a new periodic task after passing admission control.
struct mp2pcb*taskregister(struct task_struct *linux_task,pid_t pid,unsigned long period,unsigned long comp_time)
{
	struct mp2pcb*temp;
	struct mp2pcb*curnode=kmem_cache_alloc(mp2cache,GFP_KERNEL);
	unsigned long long usum;
	if(!curnode||period==0||comp_time==0) return NULL;
	usum=(comp_time*1000)/period;
	curnode->linux_task=linux_task;
	curnode->pid=pid;
	curnode->period=period;
	curnode->comp_time=comp_time;
	curnode->nxt_wake_up_time=jiffies;
	atomic_set(&curnode->RMSstate,SLEEPING);
	timer_setup(&curnode->timer,timercb,0);
	INIT_LIST_HEAD(&curnode->node);
	mutex_lock(&mp2mutex);
	list_for_each_entry(temp,&mp2listhead,node)
	{
		usum+=(temp->comp_time*1000)/temp->period;
		if(temp->pid==pid) 
		{
			mutex_unlock(&mp2mutex);
			kmem_cache_free(mp2cache,curnode);
			pr_info("task already exist pid=%d \n",pid);
			return NULL;
		}
	}
	if(usum>693) 
	{
		mutex_unlock(&mp2mutex);
		kmem_cache_free(mp2cache,curnode);
		pr_info("utilization exceeds bound pid=%d \n",pid);
		return NULL;
	}
	list_add_tail(&curnode->node,&mp2listhead);
	mutex_unlock(&mp2mutex);
	return curnode;
}


// Remove a task from the scheduler and release its resources.
bool deregister(pid_t pid)
{
	struct mp2pcb*curr,*target=NULL;
	mutex_lock(&mp2mutex);
	list_for_each_entry(curr,&mp2listhead,node)
	{
		if(curr->pid==pid) 
		{
			target=curr;
			list_del(&curr->node);
			if(curr==currtask) currtask=NULL;
			break;
		}
	}
	mutex_unlock(&mp2mutex);
	if(!target) return false;
	demote_task(target->linux_task);
	del_timer_sync(&target->timer);
	kmem_cache_free(mp2cache,target);
	if(!currtask) wakethread();
	return true;
}


// Put the current task to sleep until its next period.
bool taskyield(pid_t pid)
{
	bool success=false;
	struct mp2pcb*curr;
	if(pid!=current->pid) return false;
	mutex_lock(&mp2mutex);
	list_for_each_entry(curr,&mp2listhead,node)
	{
		if(curr->pid==pid) 
		{
			atomic_set(&curr->RMSstate,SLEEPING);
			curr->nxt_wake_up_time+=msecs_to_jiffies(curr->period);
			mod_timer(&curr->timer,curr->nxt_wake_up_time);
			if(curr==currtask) currtask=NULL;
			success=true;
			break;
		}
	}
	mutex_unlock(&mp2mutex);
	if(!success) return false;
	wakethread();
	set_current_state(TASK_UNINTERRUPTIBLE);
    schedule();
    __set_current_state(TASK_RUNNING);
	return true;
}

// Read the current registered task list from /proc/mp2/status.
ssize_t my_read(struct file * inputfile, char __user * usrbuf, size_t len, loff_t * p)
{
	struct mp2pcb*temp;
    char *kbuf;
	int total=0,left=0,cap=10240;
	kbuf=kmalloc(cap,GFP_KERNEL);
	if(!kbuf)
	{
		pr_info("kmalloc failed\n");
		return -1;
	}
	mutex_lock(&mp2mutex);
	list_for_each_entry(temp,&mp2listhead,node)
	{
		if(total>=cap-16) break;
		total+=scnprintf(kbuf+total,cap-total,"%d: %lu, %lu\n",temp->pid,temp->period,temp->comp_time);
	}
	mutex_unlock(&mp2mutex);
	left=total-*p;
	if(left<=0)
	{
		kfree(kbuf);
		return 0;
	}
	if(left<len) len=left; 
	if(copy_to_user(usrbuf,kbuf+*p,len)) 
	{
		kfree(kbuf);
		pr_info("copy_to_user failed\n");
		return -1;
	}
	*p+=len;
	kfree(kbuf);
    return len;
}

// Parse user commands from /proc/mp2/status and handle task operations.
ssize_t my_write(struct file * inputfile, const char __user *usrbuf, size_t len, loff_t * p)
{
	char kbuf[51];
	char extra;
	if(len>=sizeof(kbuf)||len==0) 
	{
		pr_info("Invalid input\n");
		return -1;
	}
	if(copy_from_user(kbuf,usrbuf,len)) 
	{
		pr_info("copy_from_user failed\n");
		return -1;
	}
	kbuf[len]='\0';
	strim(kbuf);
	if(kbuf[0]=='R')
	{
		int pid,period,comp_time;
		if(sscanf(kbuf,"R,%d,%d,%d,%c",&pid,&period,&comp_time,&extra)==3) 
		{
			// pr_info(KERN_INFO "Operation is R, pid:%d, period:%d, computation:%d\n",pid,period,computation);
			struct task_struct *linux_task=find_task_by_pid(pid);
			if(!linux_task) 
			{
				pr_info("linux_task not found\n");
				return -1;
			}
			if(!taskregister(linux_task,pid,period,comp_time)) 
			{
				pr_info("taskregister failed\n");
				return -1;
			}
		}
		else 
		{
			pr_info("Bad input R\n");
			return -1;
		}
	}
	else if(kbuf[0]=='Y')
	{
		int pid;
		if(sscanf(kbuf,"Y,%d,%c",&pid,&extra)==1) 
		{
			if(!taskyield(pid)) 
			{
				pr_info("taskyield failed\n");
				return -1;
			}
		}
		else 
		{
			pr_info("Bad input Y\n");
			return -1;
		}
	}
	else if(kbuf[0]=='D')
	{
		int pid;
		if(sscanf(kbuf,"D,%d,%c",&pid,&extra)==1) 
		{
			if(!deregister(pid))
			{
				pr_info("deregister failed\n");
				return -1;
			}
		}
		else 
		{
			pr_info("Bad input D\n");
			return -1;
		}
	}
	else 
	{
		pr_info("Bad input\n");
		return -1;
	}
	return len;
}


// Remove all tasks from the list and free their resources.
void cleanuplist(void)
{
	struct mp2pcb*cur,*pad;
	mutex_lock(&mp2mutex);
	list_for_each_entry_safe(cur,pad,&mp2listhead,node)
	{
		list_del(&cur->node);
		demote_task(cur->linux_task);
		del_timer_sync(&cur->timer);
		kmem_cache_free(mp2cache,cur);
	}
	mutex_unlock(&mp2mutex);
}


static const struct proc_ops my_ops=
{
	.proc_read=my_read,
	.proc_write=my_write,
};


// Initialize proc entries, data structures, and the dispatcher thread.
int __init mp2_init(void)
{
	mp2dir=proc_mkdir("mp2",NULL);
	if(!mp2dir) 
	{
		pr_info("proc_mkdir failed\n");
		return -1;
	}
	mp2status=proc_create("status",0666,mp2dir,&my_ops);
	if(!mp2status)
	{
		proc_remove(mp2dir);
		mp2dir=NULL;
		pr_info("proc_create failed\n");
		return -1;
	}
	mutex_init(&mp2mutex);
	INIT_LIST_HEAD(&mp2listhead);
	mp2cache=kmem_cache_create("cech4mp2",sizeof(struct mp2pcb),0,SLAB_HWCACHE_ALIGN,NULL);
	if(!mp2cache) 
	{
        proc_remove(mp2status);
        mp2status = NULL;
        proc_remove(mp2dir);
        mp2dir = NULL;
        pr_info("kmem_cache_create failed\n");
		return -1;
    }
	dispathread=kthread_run(threadfunc,NULL,"threadfunction");
	
	pr_info("mp2: module loaded\n");
	return 0;
}

// Stop the dispatcher thread and clean up all module resources.
void __exit mp2_exit(void)
{
	// del_timer_sync(&timer);
	// flush_work(&worker);
	if(dispathread) 
	{
        kthread_stop(dispathread);    
        dispathread=NULL;
    }
	cleanuplist();
	mutex_destroy(&mp2mutex);
	if(mp2cache) 
	{
		kmem_cache_destroy(mp2cache);
		mp2cache=NULL;
	}
	if(mp2status) 
	{
        proc_remove(mp2status);
        mp2status=NULL;
    }
    if(mp2dir) 
	{
        proc_remove(mp2dir);
        mp2dir=NULL;
    }
	pr_info("mp2: module unloaded\n");
}


module_init(mp2_init);
module_exit(mp2_exit);

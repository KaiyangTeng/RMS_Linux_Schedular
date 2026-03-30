
# MP2: Rate-Monotonic Scheduler Kernel Module

## Overview

This project implements a simple Rate-Monotonic Scheduler (RMS) as a Linux kernel module for a single-core system. The scheduler is exposed to user space through `/proc/mp2/status` and supports three operations:

- task registration
- task yield
- task deregistration

A user-space test application is also included to simulate periodic real-time workloads and interact with the scheduler through ProcFS.

This project was developed for CS423 Spring 2026 MP2: *Rate-Monotonic CPU Scheduling*. The implementation follows the assignment architecture of:

- a custom per-task control block
- a global registered-task list
- a kernel dispatching thread
- per-task wake-up timers
- utilization-bound-based admission control
- a user-space periodic application for testing

## Repository Structure


.
├── mp2.c         # Kernel module implementation
├── userapp.c     # User-space periodic workload generator
├── mp2_given.h   # Helper definitions provided by the course
├── Makefile      # Kernel module build file
└── README.md


## Features

### Kernel module

* Creates `/proc/mp2/status` with read/write support
* Registers periodic tasks with `(pid, period, computation time)`
* Maintains task state as one of:

  * `SLEEPING`
  * `READY`
  * `RUNNING`
* Uses a kernel dispatching thread to select the highest-priority READY task
* Uses per-task timers to wake sleeping tasks at the beginning of their next period
* Applies RMS priority rule: shorter period means higher priority
* Uses fixed-point admission control without floating-point arithmetic
* Uses a slab cache to allocate and free task control blocks efficiently

### User-space test application

* Registers itself with the kernel module
* Verifies admission by reading `/proc/mp2/status`
* Repeatedly yields, runs a CPU-bound job, and yields again
* Deregisters on completion
* Prints per-job start/finish timing information

## Design

## 1. Scheduling Model

This scheduler implements a simplified Rate-Monotonic policy for periodic real-time tasks.

Each registered task has:

* a Linux PID
* a period in milliseconds
* a computation time in milliseconds
* a wake-up timer
* an RMS state
* a pointer to its corresponding `task_struct`

The scheduler always prefers the READY task with the shortest period.

The implementation assumes a cooperative periodic task model:

* tasks explicitly notify the scheduler when they finish a job by sending `Y,<pid>`
* the kernel then puts the task to sleep until its next release time
* when the timer expires, the task becomes READY again

## 2. Task Control Block

The kernel module defines a private PCB-like structure:

```c
struct mp2pcb {
    pid_t pid;
    unsigned long comp_time, period, nxt_wake_up_time;
    atomic_t RMSstate;
    struct task_struct *linux_task;
    struct list_head node;
    struct timer_list timer;
};
```

This structure stores both scheduling metadata and runtime state for each admitted task.

## 3. Global State

The module maintains the following global objects:

* `/proc/mp2` directory entry
* `/proc/mp2/status` file entry
* linked list of registered tasks
* slab cache for `struct mp2pcb`
* mutex protecting the task list and scheduler state
* pointer to the current RMS task
* kernel dispatching thread

## ProcFS Interface

The scheduler uses a single ProcFS entry:

```text
/proc/mp2/status
```

### Write commands

#### Register

```text
R,<pid>,<period>,<computation>
```

Example:

```text
R,1234,500,80
```

Registers a task with:

* `pid = 1234`
* `period = 500 ms`
* `computation time = 80 ms`

#### Yield

```text
Y,<pid>
```

Example:

```text
Y,1234
```

Indicates that the current job has completed and the task should sleep until the next period.

#### Deregister

```text
D,<pid>
```

Example:

```text
D,1234
```

Removes the task from the scheduler and frees its resources.

### Read format

Reading `/proc/mp2/status` returns one line per registered task:

```text
<pid>: <period>, <computation>
```

Example:

```text
243: 500, 80
244: 1000, 300
```

## Core Components

## 1. Registration Path

Task registration is handled through `taskregister()`.

### What it does

* validates `period` and `comp_time`
* allocates a new task control block from the slab cache
* initializes:

  * PID
  * Linux task pointer
  * period
  * computation time
  * wake-up timer
  * initial RMS state as `SLEEPING`
* checks for duplicate PID
* applies admission control
* appends the task to the registered-task list

### Admission control

The implementation uses a fixed-point utilization test:

```text
sum(Ci / Pi) <= 0.693
```

To avoid floating-point arithmetic in the kernel, utilization is scaled by 1000:

```c
usum += (comp_time * 1000) / period;
if (usum > 693) reject;
```

This matches the Liu and Layland utilization bound for the target assignment model.

## 2. Yield Path

Yield is handled through `taskyield()`.

### What it does

* verifies the caller is yielding its own PID
* changes task state to `SLEEPING`
* advances `nxt_wake_up_time` by one period
* arms the wake-up timer with `mod_timer(...)`
* clears `currtask` if the yielding task was the current running RMS task
* wakes the dispatching thread
* puts the current task to sleep using:

  * `set_current_state(TASK_UNINTERRUPTIBLE)`
  * `schedule()`

This is the point where the task voluntarily leaves the CPU after finishing one periodic job.

## 3. Wake-Up Timer

Each task owns a kernel timer.

When a timer expires, `timercb()`:

* marks the task `READY`
* wakes the dispatching thread

This implements the periodic release of tasks without busy waiting.

## 4. Dispatching Thread

The kernel dispatching thread is created in `mp2_init()` via `kthread_run(...)`.

Its main loop:

1. calls `dispatcher()`
2. sets itself to `TASK_INTERRUPTIBLE`
3. sleeps with `schedule()`
4. wakes again when a timer expires, a task yields, or a deregistration needs rescheduling

### Scheduling decision

`dispatcher()` scans the registered-task list and chooses the READY task with the shortest period.

It then handles three main cases:

#### No current running task

If `currtask == NULL`, the chosen READY task becomes `RUNNING`.

#### Current task is sleeping

If the previous running task has already transitioned to `SLEEPING`, the chosen READY task becomes `RUNNING`.

#### Current task is running

If a READY task exists with a shorter period than the running task, the running task is preempted and demoted to `READY`, and the higher-priority task becomes `RUNNING`.

### Linux scheduling primitives used

To run a task:

* `sched_setattr_nocheck(..., SCHED_FIFO, priority)`
* `wake_up_process(...)`

To demote a task:

* `sched_setattr_nocheck(..., SCHED_NORMAL, priority 0)`

In this implementation:

* the dispatching thread runs at `SCHED_FIFO` priority 99
* promoted user tasks run at `SCHED_FIFO` priority 98
* demoted tasks return to `SCHED_NORMAL`

This design ensures the dispatcher itself can run immediately to make scheduling decisions, while promoted RMS tasks still run with strong real-time priority.

## 5. Deregistration Path

Deregistration is handled through `deregister()`.

### What it does

* removes the task from the global list
* clears `currtask` if needed
* demotes the associated Linux task back to normal scheduling
* synchronously cancels its timer
* returns the control block to the slab cache
* wakes the dispatcher if the removed task was the current RMS task

## 6. Memory Management

The module uses a dedicated slab cache for `struct mp2pcb` objects:

```c
kmem_cache_create("cech4mp2", sizeof(struct mp2pcb), ...)
```

This avoids repeated general-purpose allocations for a fixed-size kernel object and keeps object lifecycle management explicit:

* allocate on registration
* free on deregistration
* free all remaining objects on module unload

Module unload also:

* stops the dispatching thread
* destroys the mutex
* destroys the slab cache
* removes ProcFS entries

## User Application

`userapp.c` is a simple single-threaded periodic application used to test the scheduler.

### Runtime flow

1. Parse command-line arguments:

   * period
   * computation time
   * loop count for synthetic work
2. Register with:

   ```text
   R,<pid>,<period>,<computation>
   ```
3. Read `/proc/mp2/status` to verify admission
4. Send an initial yield
5. Repeat for a fixed number of jobs:

   * record start time
   * execute `do_job(loops)`
   * record finish time
   * yield again
6. Deregister with:

   ```text
   D,<pid>
   ```

### Workload model

The test job is a CPU busy loop:

```c
for (i = 0; i < loops; i++);
```

This is used instead of a more complex workload to keep timing behavior simple and easy to reproduce.

### Output

The application prints:

* admission result
* start time of each job
* finish time of each job
* measured runtime of each job

This makes it easier to verify:

* periodic release behavior
* preemption behavior
* whether jobs approximately fit into their requested computation budget

## Build

Before building, update the kernel source path in the Makefile as required by the course environment.

### Build kernel module

```bash
make
```

### Clean build artifacts

```bash
make clean
```

### Build user application

If your Makefile does not already compile it, a typical command is:

```bash
gcc -O2 -Wall userapp.c -o userapp
```

## Run

### Load module

```bash
sudo insmod mp2.ko
```

### Check kernel log

```bash
dmesg | tail
```

### Read scheduler state

```bash
cat /proc/mp2/status
```

### Run one task

```bash
./userapp 500 80 3000000
```

### Run multiple tasks

```bash
./userapp 500 80 3000000 > outA.txt 2>&1 &
./userapp 1000 300 7000000 > outB.txt 2>&1 &
wait
```

### Unload module

```bash
sudo rmmod mp2
```

## Example Behavior

Given two admitted tasks:

* Task A: period 500 ms, computation 80 ms
* Task B: period 1000 ms, computation 300 ms

Task A has higher RMS priority because it has the shorter period. If both tasks are READY, Task A should run first. If Task B is running and Task A becomes READY, the dispatcher should preempt Task B and promote Task A.

## Concurrency and Synchronization

The module uses a mutex to protect shared scheduler state:

* registered-task list
* current RMS task pointer
* task state transitions that must be consistent with list traversal

Atomic state variables are used for per-task RMS state reads and writes.

A notable design choice is that actual task promotion/demotion is performed outside the mutex in `dispatcher()`. The dispatcher first computes which task should change scheduling class, releases the lock, and only then calls Linux scheduling APIs. This reduces lock hold time and avoids mixing list protection with potentially heavier scheduler operations.

## Limitations and Notes

This implementation is intentionally minimal and assignment-focused.

### Current assumptions

* single-core scheduling model
* cooperative periodic tasks
* well-behaved user applications that always deregister
* linked-list run queue, which is acceptable for a small number of tasks

### Important implementation notes

* the scheduler uses the classic bound `0.693` rather than the exact bound for arbitrary task counts
* the user test workload is a busy loop, not factorial computation
* ProcFS callbacks currently return `-1` on several failures rather than standard Linux error codes such as `-EINVAL` or `-EFAULT`
* the read buffer in `my_read()` is fixed-size
* this project is designed for the course kernel and testing environment, not as a production Linux scheduler extension

## Testing Strategy

Recommended tests include:

* single task registration / yield / deregistration
* multiple admitted tasks with different periods
* admission rejection when utilization exceeds bound
* verifying that `/proc/mp2/status` reflects the current registered set
* verifying preemption when a shorter-period task becomes READY
* repeated load/unload cycles to check cleanup correctness

## Implementation Summary

In summary, this project builds a small real-time scheduling subsystem inside the Linux kernel using:

* ProcFS for user-kernel communication
* slab allocation for task objects
* timers for periodic wake-up
* a dispatching kernel thread for RMS decisions
* Linux scheduler APIs for actual execution control

The final result is a compact teaching implementation of a Rate-Monotonic scheduler that demonstrates:

* admission control
* periodic release
* cooperative yield
* preemptive priority-based dispatch
* kernel/user coordination through a simple control interface

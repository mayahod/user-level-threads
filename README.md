# User-Level Threads Library

A C++ implementation of a user-level threading library developed as part of the Operating Systems course at the Hebrew University of Jerusalem.

The project implements thread creation and lifecycle management, preemptive round-robin scheduling, context switching, blocking and resuming, sleeping, and quantum accounting.

## Features

- User-level thread creation and termination
- Preemptive round-robin scheduling
- Context switching using `sigsetjmp` and `siglongjmp`
- Virtual timer-based scheduling using `SIGVTALRM` and `ITIMER_VIRTUAL`
- Thread blocking and resuming
- Thread sleeping for a specified number of quantums
- Per-thread and global quantum tracking
- Signal masking around scheduler-critical operations
- Static library generation with Make

## How It Works

Each thread maintains its own execution context and stack.

The scheduler uses a virtual timer to periodically generate `SIGVTALRM`. When a scheduling event occurs, the current thread context is saved and another ready thread is selected using a round-robin policy.

Thread execution contexts are saved and restored using `sigsetjmp` and `siglongjmp`.

The library maintains thread states such as:

- `RUNNING`
- `READY`
- `BLOCKED`

Sleeping and explicitly blocked threads are tracked separately so that a thread becomes ready only when all conditions preventing it from running have been removed.

## Public API

The library provides the following operations:

- `uthread_init` — initialize the threading library and scheduler
- `uthread_spawn` — create a new user-level thread
- `uthread_terminate` — terminate a thread
- `uthread_block` — block a thread
- `uthread_resume` — resume a blocked thread
- `uthread_sleep` — put the currently running thread to sleep
- `uthread_get_tid` — return the ID of the calling thread
- `uthread_get_total_quantums` — return the total number of elapsed quantums
- `uthread_get_quantums` — return the number of quantums executed by a specific thread

## Build

The project is intended to run in a Linux environment.

Requirements:

- `g++`
- `make`

Build the static library with:

```bash
make
```

This creates:

```text
libuthreads.a
```

To remove generated build files:

```bash
make clean
```

## Usage Example

A simple example of initializing the library and spawning a thread:

```cpp
#include "uthreads.h"
#include <cstdio>
#include <csignal>

volatile sig_atomic_t worker_finished = 0;

void worker()
{
    std::printf("Hello from thread %d\n", uthread_get_tid());
    worker_finished = 1;
    uthread_terminate(uthread_get_tid());
}

int main()
{
    if (uthread_init(100000) != 0)
    {
        return 1;
    }

    if (uthread_spawn(worker) < 0)
    {
        return 1;
    }

    while (!worker_finished)
    {
        // Keep the main thread running while the scheduler switches threads.
    }

    uthread_terminate(0);
}
```

## Project Structure

```text
.
├── uthreads.cpp
├── uthreads.h
├── Makefile
└── README.md
```

- `uthreads.cpp` — implementation of the user-level threading library
- `uthreads.h` — public API provided as part of the course infrastructure
- `Makefile` — builds the project as a static library
- `README.md` — project documentation

## Technical Concepts

This project demonstrates several operating-systems and low-level programming concepts:

- User-level vs. kernel-level threads
- CPU scheduling
- Context switching
- Process signals
- Signal masking
- Virtual timers
- Thread state management
- Manual stack and execution-context management
- Static libraries in C++

## Academic Context

This project was developed as an academic assignment in the Operating Systems course at the Hebrew University of Jerusalem.

The public API and architecture-specific context setup were based on course-provided infrastructure. The thread management and scheduling implementation was developed as part of the assignment.

The original assignment was completed in collaboration with another student.

/**
 * @file uthreads.cpp
 * @brief User-level threads library with preemptive round-robin scheduling.
 *
 * Implements thread creation, context switching, blocking, resuming, sleeping,
 * termination, and quantum accounting using SIGVTALRM, ITIMER_VIRTUAL, and
 * sigsetjmp/siglongjmp.
 *
 * Academic project developed as part of the Operating Systems course at the
 * Hebrew University of Jerusalem. The public API and architecture-specific
 * context setup are based on course-provided infrastructure.
 */

#include "uthreads.h"
#include <list>
#include <cstdio>
#include <csignal>
#include <sys/time.h>
#include <setjmp.h>
#include <cstdlib>
#include <new>


enum ThreadState {
    RUNNING,
    READY,
    BLOCKED
};

enum SchedulerEvent {
    BLOCK_EVENT = -1,
    TERMINATE_EVENT = -2,
    INIT_EVENT = -3
};

constexpr int CONTEXT_SWITCH_RETURN = 1;


#ifdef __x86_64__

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

// Architecture-specific address translation required by the course framework.
address_t translate_address(address_t addr) {
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g" (ret)
                 : "0"(addr));
    return ret;
}

#else

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

// Architecture-specific address translation required by the course framework.
address_t translate_address(address_t addr) {
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
                 : "=g" (ret)
                 : "0"(addr));
    return ret;
}

#endif


class Thread {
private:
    int _id;
    ThreadState _state;
    int _quantum;
    char _stack[STACK_SIZE];
    bool _sleep_blocked;
    bool _explicitly_blocked;
    sigjmp_buf _sjb;
    int _sleep_quantums_remaining;
    bool _sleeping;

    void update_blocked_state() {
        _state = (_sleep_blocked || _explicitly_blocked) ? BLOCKED : READY;
    }

public:
    explicit Thread(int id)
        : _id(id),
          _state(READY),
          _quantum(0),
          _sleep_blocked(false),
          _explicitly_blocked(false),
          _sleep_quantums_remaining(0),
          _sleeping(false) {}

    ~Thread() = default;

    int get_id() const {
        return _id;
    }

    ThreadState get_state() const {
        return _state;
    }

    address_t get_address_stack() const {
        return reinterpret_cast<address_t>(_stack);
    }

    void set_state(ThreadState state) {
        _state = state;
    }

    void increment_quantum() {
        ++_quantum;
    }

    int get_quantum() const {
        return _quantum;
    }

    bool is_explicitly_blocked() const {
        return _explicitly_blocked;
    }

    void set_explicitly_blocked(bool blocked) {
        _explicitly_blocked = blocked;
        update_blocked_state();
    }

    void set_sleep_blocked(bool blocked) {
        _sleep_blocked = blocked;
        update_blocked_state();
    }

    sigjmp_buf& get_sjb() {
        return _sjb;
    }

    int get_sleep_quantums() const {
        return _sleep_quantums_remaining;
    }

    void set_sleep_quantums(int quantums) {
        _sleep_quantums_remaining = quantums;
    }

    bool is_sleeping() const {
        return _sleeping;
    }

    void set_sleeping(bool sleeping) {
        _sleeping = sleeping;
    }

    void decrement_sleep_quantums() {
        if (_sleep_quantums_remaining > 0) {
            --_sleep_quantums_remaining;
        }
    }
};


Thread* all_threads[MAX_THREAD_NUM] = {nullptr};
std::list<Thread*> ready_threads;
Thread* cur_running = nullptr;
std::list<Thread*> sleeping_threads;

int total_quantum = 0;
int single_quant_sec = 0;
int single_quant_usec = 0;

sigset_t timer_signal_set;


int init_scheduler();
void round_robin(int event);
void remove_ready(int tid);
void quantum_timer_handler();


/**
 * @brief Blocks SIGVTALRM while scheduler data structures are being modified.
 */
void block_signal() {
    if (sigemptyset(&timer_signal_set) == -1) {
        std::fprintf(stderr, "system error: sigemptyset failed\n");
        std::exit(1);
    }

    if (sigaddset(&timer_signal_set, SIGVTALRM) == -1) {
        std::fprintf(stderr, "system error: sigaddset failed\n");
        std::exit(1);
    }

    if (sigprocmask(SIG_BLOCK, &timer_signal_set, nullptr) == -1) {
        std::fprintf(stderr, "system error: sigprocmask failed\n");
        std::exit(1);
    }
}


/**
 * @brief Unblocks SIGVTALRM after a scheduler critical section.
 */
void unblock_signal() {
    if (sigprocmask(SIG_UNBLOCK, &timer_signal_set, nullptr) == -1) {
        std::fprintf(stderr, "system error: sigprocmask failed\n");
        std::exit(1);
    }
}


/**
 * @brief Performs a round-robin scheduling decision and switches context.
 *
 * The current context is saved with sigsetjmp. Depending on the event, the
 * current thread is returned to the ready queue, blocked, or terminated. The
 * next ready thread is then selected and restored with siglongjmp.
 *
 * @param event SIGVTALRM for timer preemption, or one of SchedulerEvent.
 */
void round_robin(int event) {
    block_signal();

    int val = sigsetjmp(cur_running->get_sjb(), 1);
    if (val == CONTEXT_SWITCH_RETURN) {
        // siglongjmp restores the saved signal mask, which may include the
        // blocked timer signal. Ensure preemption is enabled before returning.
        unblock_signal();
        return;
    }

    if (event == SIGVTALRM || event == INIT_EVENT) {
        cur_running->set_state(READY);
        ready_threads.push_back(cur_running);
    }
    else if (event == BLOCK_EVENT) {
        cur_running->set_state(BLOCKED);
    }
    else if (event == TERMINATE_EVENT) {
        int id = cur_running->get_id();
        delete all_threads[id];
        all_threads[id] = nullptr;
    }

    quantum_timer_handler();

    if (ready_threads.empty()) {
        std::fprintf(stderr, "system error: no runnable threads\n");
        std::exit(1);
    }

    cur_running = ready_threads.front();
    ready_threads.pop_front();
    cur_running->set_state(RUNNING);
    cur_running->increment_quantum();
    ++total_quantum;

    if (init_scheduler() < 0) {
        std::exit(1);
    }

    unblock_signal();
    siglongjmp(cur_running->get_sjb(), CONTEXT_SWITCH_RETURN);
}


/**
 * @brief Installs the SIGVTALRM handler and starts a one-shot virtual timer.
 *
 * The timer is restarted at the beginning of every quantum.
 *
 * @return 0 on success, -1 on failure.
 */
int init_scheduler() {
    struct sigaction sa = {};
    sa.sa_handler = &round_robin;
    sa.sa_flags = 0;

    if (sigemptyset(&sa.sa_mask) == -1) {
        std::fprintf(stderr, "system error: sigemptyset failed\n");
        return -1;
    }

    if (sigaction(SIGVTALRM, &sa, nullptr) < 0) {
        std::fprintf(stderr, "system error: sigaction failed\n");
        return -1;
    }

    struct itimerval timer = {};
    timer.it_value.tv_sec = single_quant_sec;
    timer.it_value.tv_usec = single_quant_usec;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;

    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) < 0) {
        std::fprintf(stderr, "system error: setitimer failed\n");
        return -1;
    }

    return 0;
}


/**
 * @brief Removes a thread from the ready queue if it is present.
 *
 * @param tid Thread ID to remove.
 */
void remove_ready(int tid) {
    for (auto it = ready_threads.begin(); it != ready_threads.end(); ++it) {
        if ((*it)->get_id() == tid) {
            ready_threads.erase(it);
            return;
        }
    }
}


/**
 * @brief Advances sleep counters when a new quantum begins.
 *
 * A thread whose sleep period ends stops being sleep-blocked. If it is not
 * explicitly blocked as well, it becomes READY and is appended to the ready
 * queue. Otherwise it remains BLOCKED until uthread_resume is called.
 */
void quantum_timer_handler() {
    for (auto it = sleeping_threads.begin(); it != sleeping_threads.end(); ) {
        Thread* thread = *it;
        thread->decrement_sleep_quantums();

        if (thread->get_sleep_quantums() == 0) {
            thread->set_sleeping(false);
            thread->set_sleep_blocked(false);
            it = sleeping_threads.erase(it);

            if (thread->get_state() == READY) {
                ready_threads.push_back(thread);
            }
        }
        else {
            ++it;
        }
    }
}


/**
 * @brief Initializes the user-level threads library.
 *
 * @param quantum_usecs Length of a quantum in microseconds.
 * @return 0 on success, -1 on library error.
 */
int uthread_init(int quantum_usecs) {
    if (quantum_usecs <= 0) {
        std::fprintf(stderr, "thread library error: invalid quantum length\n");
        return -1;
    }

    single_quant_sec = quantum_usecs / 1000000;
    single_quant_usec = quantum_usecs % 1000000;
    total_quantum = 0;

    try {
        Thread* main_thread = new Thread(0);
        all_threads[0] = main_thread;
        cur_running = main_thread;
        cur_running->set_state(RUNNING);
    }
    catch (const std::bad_alloc&) {
        std::fprintf(stderr, "system error: failed to initialize main thread\n");
        std::exit(1);
    }

    if (init_scheduler() < 0) {
        return -1;
    }

    // Count the first quantum and initialize the main thread's saved context.
    round_robin(INIT_EVENT);
    return 0;
}


/**
 * @brief Creates a new thread and appends it to the ready queue.
 *
 * @param entry_point Function that the new thread will execute.
 * @return New thread ID on success, -1 on library error.
 */
int uthread_spawn(thread_entry_point entry_point) {
    if (entry_point == nullptr) {
        std::fprintf(stderr, "thread library error: null entry point\n");
        return -1;
    }

    block_signal();

    int next_id = -1;
    for (int i = 0; i < MAX_THREAD_NUM; ++i) {
        if (all_threads[i] == nullptr) {
            next_id = i;
            break;
        }
    }

    if (next_id == -1) {
        std::fprintf(stderr, "thread library error: maximum thread limit reached\n");
        unblock_signal();
        return -1;
    }

    try {
        Thread* thread = new Thread(next_id);
        address_t sp = thread->get_address_stack() + STACK_SIZE - sizeof(address_t);
        address_t pc = reinterpret_cast<address_t>(entry_point);

        sigsetjmp(thread->get_sjb(), 1);
        (thread->get_sjb()->__jmpbuf)[JB_SP] = translate_address(sp);
        (thread->get_sjb()->__jmpbuf)[JB_PC] = translate_address(pc);
        sigemptyset(&thread->get_sjb()->__saved_mask);

        all_threads[next_id] = thread;
        ready_threads.push_back(thread);
    }
    catch (const std::bad_alloc&) {
        std::fprintf(stderr, "system error: failed to create a new thread\n");
        unblock_signal();
        std::exit(1);
    }

    unblock_signal();
    return next_id;
}


/**
 * @brief Terminates a thread and releases its resources.
 *
 * Terminating thread 0 exits the entire process. If a thread terminates itself,
 * scheduling immediately continues with the next ready thread.
 *
 * @param tid Thread ID to terminate.
 * @return 0 on success, -1 on library error.
 */
int uthread_terminate(int tid) {
    block_signal();

    if (tid < 0 || tid >= MAX_THREAD_NUM || all_threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: invalid thread id\n");
        unblock_signal();
        return -1;
    }

    if (tid == 0) {
        for (int i = 1; i < MAX_THREAD_NUM; ++i) {
            delete all_threads[i];
            all_threads[i] = nullptr;
        }
        std::exit(0);
    }

    Thread* thread = all_threads[tid];

    if (thread == cur_running) {
        round_robin(TERMINATE_EVENT);
        return 0; // round_robin does not normally return to a terminated thread
    }

    if (thread->get_state() == READY) {
        remove_ready(tid);
    }

    if (thread->is_sleeping()) {
        sleeping_threads.remove(thread);
    }

    delete thread;
    all_threads[tid] = nullptr;

    unblock_signal();
    return 0;
}


/**
 * @brief Explicitly blocks a thread.
 *
 * A sleeping thread can also be explicitly blocked. In that case, finishing
 * its sleep period is not enough to make it READY; it must also be resumed.
 *
 * @param tid Thread ID to block.
 * @return 0 on success, -1 on library error.
 */
int uthread_block(int tid) {
    block_signal();

    if (tid <= 0 || tid >= MAX_THREAD_NUM || all_threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: invalid thread id\n");
        unblock_signal();
        return -1;
    }

    Thread* thread = all_threads[tid];

    if (thread->is_explicitly_blocked()) {
        unblock_signal();
        return 0;
    }

    ThreadState previous_state = thread->get_state();
    thread->set_explicitly_blocked(true);

    if (previous_state == RUNNING) {
        round_robin(BLOCK_EVENT);
    }
    else if (previous_state == READY) {
        remove_ready(tid);
    }

    unblock_signal();
    return 0;
}


/**
 * @brief Removes an explicit block from a thread.
 *
 * If the thread is still sleeping, it remains BLOCKED. Otherwise it is moved
 * to the ready queue.
 *
 * @param tid Thread ID to resume.
 * @return 0 on success, -1 on library error.
 */
int uthread_resume(int tid) {
    block_signal();

    if (tid < 0 || tid >= MAX_THREAD_NUM || all_threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: invalid thread id\n");
        unblock_signal();
        return -1;
    }

    Thread* thread = all_threads[tid];

    if (thread->is_explicitly_blocked()) {
        thread->set_explicitly_blocked(false);

        if (thread->get_state() == READY) {
            ready_threads.push_back(thread);
        }
    }

    unblock_signal();
    return 0;
}


/**
 * @brief Puts the running thread to sleep for a number of quantums.
 *
 * @param num_quantums Number of new quantums that must begin before the thread
 *                     becomes eligible to run again.
 * @return 0 on success, -1 on library error.
 */
int uthread_sleep(int num_quantums) {
    block_signal();

    if (cur_running->get_id() == 0) {
        std::fprintf(stderr, "thread library error: main thread cannot sleep\n");
        unblock_signal();
        return -1;
    }

    if (num_quantums <= 0) {
        std::fprintf(stderr, "thread library error: sleep duration must be positive\n");
        unblock_signal();
        return -1;
    }

    cur_running->set_sleep_blocked(true);
    cur_running->set_sleeping(true);
    cur_running->set_sleep_quantums(num_quantums);
    sleeping_threads.push_back(cur_running);

    round_robin(BLOCK_EVENT);

    unblock_signal();
    return 0;
}


/**
 * @brief Returns the calling thread's ID.
 */
int uthread_get_tid() {
    return cur_running->get_id();
}


/**
 * @brief Returns the total number of quantums since initialization.
 */
int uthread_get_total_quantums() {
    return total_quantum;
}


/**
 * @brief Returns the number of quantums in which a thread has run.
 *
 * @param tid Thread ID.
 * @return Quantum count on success, -1 on library error.
 */
int uthread_get_quantums(int tid) {
    block_signal();

    if (tid < 0 || tid >= MAX_THREAD_NUM || all_threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: invalid thread id\n");
        unblock_signal();
        return -1;
    }

    int result = all_threads[tid]->get_quantum();
    unblock_signal();
    return result;
}

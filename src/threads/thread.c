#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define PRIORITY_DONATION_DEPTH 8

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List of processes in THREAD_SLEEPING state, that is, processes
   that are sleeping. This list will always be a sorted list,
   so make use of insert_sorted_order while pushing threads into this
   list, it makes it efficient for processing in schedule method
*/
static struct list sleeping_list;

static struct list missed_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
	void *eip;			   /* Return address. */
	thread_func *function; /* Function to call. */
	void *aux;			   /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static bool is_thread(struct thread *) UNUSED;
static void *alloc_frame(struct thread *, size_t size);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&all_list);
	list_init(&sleeping_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pagedir != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	struct kernel_thread_frame *kf;
	struct switch_entry_frame *ef;
	struct switch_threads_frame *sf;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	enum intr_level old_level = intr_disable();

	/* Stack frame for kernel_thread(). */
	kf = alloc_frame(t, sizeof *kf);
	kf->eip = NULL;
	kf->function = function;
	kf->aux = aux;

	/* Stack frame for switch_entry(). */
	ef = alloc_frame(t, sizeof *ef);
	ef->eip = (void (*)(void))kernel_thread;

	/* Stack frame for switch_threads(). */
	sf = alloc_frame(t, sizeof *sf);
	sf->eip = switch_entry;
	sf->ebp = 0;
	intr_set_level(old_level);

	/* Add to run queue. */
	thread_unblock(t);

	// We disable the interrupts so that this the new thread should not be
	// scheduled till its completely un-blocked
	old_level = intr_disable();
	should_yield_current_thread();
	intr_set_level(old_level);

	return tid;
}

/*
 this new function creates a thread with deadline. it calls the same thread_create
 from before and just sets the deadline after.
 */
tid_t thread_create_deadline(const char *name, int priority,
							 thread_func *function, int64_t deadline,
							 void *aux)
{
	tid_t tid = thread_create(name, priority, function, aux);
	set_deadline(tid, deadline);
	return tid;
}

/*
 this function finds the thread and sets the deadline.
 */
void set_deadline(tid_t tid, int64_t deadline)
{
	struct list_elem *e;
	for (e = list_begin(&all_list); e != list_end(&all_list);
		 e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, allelem);
		if (t->tid == tid)
		{
			t->deadline = deadline;
			break;
		}
	}
}


/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);

	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));
	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	list_insert_ordered(
		&ready_list,
		&t->elem,
		(list_less_func *)&compare_thread_priority,
		NULL);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
	intr_disable();
	list_remove(&thread_current()->allelem);
	thread_current()->status = THREAD_DYING;
	schedule();
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *cur = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (cur != idle_thread)
	{
		list_insert_ordered(
			&ready_list,
			&cur->elem,
			(list_less_func *)&compare_thread_priority,
			NULL);
	}
	cur->status = THREAD_READY;
	schedule();
	intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func *func, void *aux)
{
	struct list_elem *e;

	ASSERT(intr_get_level() == INTR_OFF);

	for (e = list_begin(&all_list); e != list_end(&all_list);
		 e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, allelem);
		func(t, aux);
	}
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
{
	enum intr_level old_level = intr_disable();
	struct thread *current_thread = thread_current();
	int old_priority = current_thread->priority;
	current_thread->base_priority = new_priority;
	update_priority();
	// If new priority is greater, donate it
	if (old_priority < current_thread->priority)
	{
		priority_donate_chain();
	}
	// If new priority is less, test if the processor should be yielded
	if (old_priority > current_thread->priority)
	{
		should_yield_current_thread();
	}
	intr_set_level(old_level);
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/**
 * When the lock is released, update the current threads priority with highest
 * donated priority if higher than its current base_priority
 */
void update_priority(void)
{
	struct thread *current_thread = thread_current();
	current_thread->priority = current_thread->base_priority;
	if (!list_empty(&current_thread->donors))
	{
		struct thread *highest_doner_thread = list_entry(
			list_front(&current_thread->donors), struct thread, donation_elem);
		if (highest_doner_thread->priority > current_thread->priority)
		{
			current_thread->priority = highest_doner_thread->priority;
		}
	}
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* Not yet implemented. */
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* Not yet implemented. */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* Not yet implemented. */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;
	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
		asm volatile("sti hlt"
					 :
					 :
					 : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread(void)
{
	uint32_t *esp;

	/* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
	asm("mov %%esp, %0"
		: "=g"(esp));
	return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread(struct thread *t)
{
	return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	enum intr_level old_level;

	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->stack = (uint8_t *)t + PGSIZE;
	t->priority = priority;
	t->magic = THREAD_MAGIC;
	t->wake_time = 0;

	t->deadline = -1; // initial deadline for threads that don't have deadline.
					  // if thread has deadline will be set after creation.

	// Added initializations for priority donation
	t->base_priority = priority;
	t->waiting_for_lock = NULL;
	list_init(&t->donors);

	old_level = intr_disable();
	list_push_back(&all_list, &t->allelem);
	intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame(struct thread *t, size_t size)
{
	/* Stack data is always allocated in word-size units. */
	ASSERT(is_thread(t));
	ASSERT(size % sizeof(uint32_t) == 0);

	t->stack -= size;
	return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
	{
		return idle_thread;
	}
	int max = PRI_MIN;
	struct thread *e, *toRun;
	toRun = NULL;
	for (e = list_begin(&ready_list); e != list_end(&ready_list);
		 e = list_next(e))
	{
		if (e->priority > max)
		{
			max = e->priority;
			toRun = e;
		}
	}
	list_remove(toRun);
	toRun->status = THREAD_RUNNING;
	return toRun;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_schedule_tail(struct thread *prev)
{
	struct thread *cur = running_thread();

	ASSERT(intr_get_level() == INTR_OFF);

	/* Mark us as running. */
	cur->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate();
#endif

	/* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
	if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
	{
		ASSERT(prev != cur);
		palloc_free_page(prev);
	}
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule(void)
{
	// Get the first element from sleeping list
	struct list_elem *temp, *e = list_begin(&sleeping_list);
	int64_t cur_ticks = timer_ticks();

	// Iterate over sleeping threads list and if any of the threads have finished
	// the sleeping time /wake time, then move them to ready-threads list
	while (e != list_end(&sleeping_list))
	{
		struct thread *t = list_entry(e, struct thread, elem);
		// next element in Sleeping list
		temp = list_next(e);
		// If thread has finished the wait (sleep) time
		if (cur_ticks >= t->wake_time)
		{
			list_remove(e); /* Remove this thread from sleeping_list */
			list_insert_ordered(
				&ready_list,
				&t->elem,
				(list_less_func *)&compare_thread_priority,
				NULL);
			t->status = THREAD_READY;
		}
		e = temp;
	}

	// call this function to update the priority of all threads with deadline.
	update_deadline_priority();

	struct thread *cur = running_thread();
	struct thread *next = next_thread_to_run();
	struct thread *prev = NULL;

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(cur->status != THREAD_RUNNING);
	ASSERT(is_thread(next));

	if (cur != next)
		prev = switch_threads(cur, next);
	thread_schedule_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

/*
 * Sleeps the current thread for given number of ticks,
 * add it to sleeping threads list and calls schedule method to schedule
 * next thread, but doesn't do anything if its an idle thread.
 */
void thread_sleep(int64_t ticks)
{
	struct thread *current_thread = thread_current();
	// disable the interrupts
	enum intr_level previous_thread_level = intr_disable();
	if (current_thread != idle_thread)
	{
		current_thread->status = THREAD_SLEEPING;
		current_thread->wake_time = timer_ticks() + ticks;
		list_insert_ordered(
			&sleeping_list,
			&current_thread->elem,
			(list_less_func *)&compare_thread_ticks,
			NULL);
		schedule();
	}
	// restore the interrupt level
	intr_set_level(previous_thread_level);
}

/*
 go through the ready list, update the priority of every thread that has a deadline.
*/
void update_deadline_priority(void)
{
	struct list_elem *e = list_begin(&ready_list);

	while (e != list_end(&ready_list))
	{
		struct thread *t = list_entry(e, struct thread, allelem);
		if (t->deadline != -1)
		{
			e = list_next(e);

			if (t->deadline <= timer_ticks())
			{
				t->status = THREAD_MISSED;
				list_remove(t);
				list_insert(&missed_list, t);
			}
			t->priority = 1 / (t->deadline - timer_ticks()) * PRI_MAX;
			
			list_remove(t);
			list_insert_ordered(
				&ready_list,
				&t->elem,
				(list_less_func *)&compare_thread_priority,
				NULL);
		}
	}
}

/**
 *  Once the lock is released, remove all thread elements from donors list
 *  which were waiting for this lock (i.e. lock which is released now)
 */
void release_threads_waiting_for_lock(struct lock *lock)
{
	struct thread *current_thread = thread_current();
	struct list_elem *next, *elem = list_begin(&current_thread->donors);
	while (elem != list_end(&current_thread->donors))
	{
		struct thread *temp = list_entry(elem, struct thread, donation_elem);
		// Only remove threads which are waiting for this lock,
		// others will still remain in the donation list
		if (temp->waiting_for_lock == lock)
		{
			list_remove(elem);
		}
		next = list_next(elem);
		elem = next;
	}
}

/**
 * Compare the priorities of two given threads based on their current priority
 */
bool compare_thread_priority(const struct list_elem *t1_elem,
							 const struct list_elem *t2_elem, void *aux UNUSED)
{
	struct thread *thread1 = list_entry(t1_elem, struct thread, elem);
	struct thread *thread2 = list_entry(t2_elem, struct thread, elem);
	return (thread1->priority > thread2->priority) ? true : false;
}

/**
 * Compare wake-times of given two threads
 */
bool compare_thread_ticks(const struct list_elem *t1_elem,
						  const struct list_elem *t2_elem, void *aux UNUSED)
{
	struct thread *thread1 = list_entry(t1_elem, struct thread, elem);
	struct thread *thread2 = list_entry(t2_elem, struct thread, elem);
	return (thread1->wake_time > thread2->wake_time) ? true : false;
}

/**
 * Check should we yield the thread -->
 * 1) If there is any high priority thread exists in ready list,
 * if yes, then that high priority thread should be scheduled,
 * 2) If the next thread is of same priority and current thread
 * has finished its alloted time_slice,
 *
 * Then yield the current thread.
 */
void should_yield_current_thread(void)
{
	// If there is no other thread to run, then return
	if (list_empty(&ready_list))
	{
		return;
	}
	struct thread *next_ready_thread = list_entry(list_front(&ready_list), struct thread, elem);
	struct thread *current_thread = thread_current();
	// Are we processing external interrupts, like timer interrupt
	if (intr_context())
	{
		// increment the total thread ticks
		thread_ticks++;
		if (current_thread->priority < next_ready_thread->priority || (current_thread->priority == next_ready_thread->priority && thread_ticks >= TIME_SLICE))
		{
			intr_yield_on_return();
		}
		return;
	}
	// If next thread is of higher priority
	if (current_thread->priority < next_ready_thread->priority)
	{
		thread_yield();
	}
}

/**
 * If the lock is already acquired by a low-priority thread,
 * then donate current thread's priority to it, this priority donation is
 * chained i.e. till max depth of 8
 */
void priority_donate_chain(void)
{
	update_deadline_priority();
	struct thread *current_thread = thread_current();
	struct lock *waiting_for_lock = current_thread->waiting_for_lock;
	int depth;
	for (depth = 0; waiting_for_lock && depth < PRIORITY_DONATION_DEPTH; depth++)
	{
		// if no one is holding this lock then no need to donate the priority
		if (!waiting_for_lock->holder || waiting_for_lock->holder->priority >= current_thread->priority)
		{
			return;
		}
		// Raise the holder's priority - by donating priority
		waiting_for_lock->holder->priority = current_thread->priority;
		current_thread = waiting_for_lock->holder;
		waiting_for_lock = current_thread->waiting_for_lock;
	}
}

#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"

enum state 
{
	READY = 1,
	RUNNING = 2,
	EXIT = 3
};

/* This is the wait queue structure */
struct wait_queue {
	/* ... Fill this in Lab 3 ... */
};

/* This is the thread control block */
struct thread {
	Tid threadID;
	ucontext_t context;
	void *stack_ptr;
	struct thread* next;
};

//global vars
struct thread *ready_q;
struct thread *exit_q;
struct thread *current_thread;
int tids_available[THREAD_MAX_THREADS];

void addThreadToQ(struct thread* new_thread)
{
	if(ready_q == NULL)
	{
		ready_q = new_thread;
		new_thread->next = NULL;
	}
	else
	{
		struct thread *current = ready_q;
		while(current->next != NULL)
		{
			current = current->next;
		}

		current->next = new_thread;
		new_thread->next = NULL;
	}

	return;
}

struct thread* removeHead()
{
	struct thread *current = ready_q;
	if(ready_q != NULL)
	{
		ready_q = current->next;
		current->next = NULL;
	}

	return current;
}

struct thread* findAndRemoveThread(Tid id)
{
	struct thread *current = ready_q;
	struct thread *prev = NULL;

	if(current != NULL)
	{
		if(current->threadID == id)
		{
			ready_q = current->next;
			current->next = NULL;
			return current;
		}
		else
		{
			while(current)
			{
				prev = current;
				current = current->next;

				if(current->threadID == id)
				{
					prev->next = current->next;
					current->next = NULL;
					return current;
				}
			}
		}
	}
	
	return NULL;
}

void clearExitQ()
{
	struct thread *current = exit_q;
	while(current != NULL)
	{
		struct thread *remove = current;
		current = current->next;
		tids_available[remove->threadID] = 0;

		free(remove->stack_ptr);
		remove->stack_ptr = NULL;
		free(remove);
		remove = NULL;
	}

	exit_q = NULL;
}

void
thread_stub(void (*thread_main)(void *), void *arg)
{
	//Tid ret;

	//clearExitQ();

	thread_main(arg); // call thread_main() function with arg
	thread_exit();

	//we get here if we are on last thread
	//assert(ret == THREAD_NONE);
	exit(0); //exit program
}

void
thread_init(void)
{
	for(int i = 0; i < THREAD_MAX_THREADS; i++)
	{
		tids_available[i] = 0;
	}

	ready_q = NULL;
	exit_q = NULL;
	current_thread = NULL;

	struct thread *kernal_thread = (struct thread*)malloc(sizeof(struct thread));
	kernal_thread->stack_ptr = NULL;
	getcontext(&kernal_thread->context);
	kernal_thread->threadID = 0;

	tids_available[0] = 1; //TID 0 is taken by kernal thread
	current_thread = kernal_thread;
}

Tid
thread_id()
{
	return current_thread->threadID;
}

Tid
thread_create(void (*fn) (void *), void *parg)
{
	//error checks
	struct thread *new_thread = (struct thread*)malloc(sizeof(struct thread));
	if(new_thread == NULL)
		return THREAD_NOMEMORY;

	for(int i = 1; i < THREAD_MAX_THREADS; i++)
	{
		if(tids_available[i] == 0)
		{
			new_thread->threadID = i;
			tids_available[i] = 1;
			break;
		}
		else if(tids_available[i] == 1 && i == THREAD_MAX_THREADS-1)
			return THREAD_NOMORE;
	}

	//copy current context to new thread
	getcontext(&new_thread->context);

	//create stack ptr and check for mem err
	void* stack_ptr = malloc(THREAD_MIN_STACK);
	if(stack_ptr == NULL)
	{
		free(new_thread);
		return THREAD_NOMEMORY;
	}

	//set up registers
	new_thread->context.uc_mcontext.gregs[REG_RDI] = (unsigned long)fn;
	new_thread->context.uc_mcontext.gregs[REG_RSI] = (unsigned long)parg;
	new_thread->context.uc_mcontext.gregs[REG_RIP] = (unsigned long)&thread_stub;
	new_thread->context.uc_mcontext.gregs[REG_RSP] = (unsigned long)stack_ptr + THREAD_MIN_STACK - 8;
	new_thread->stack_ptr = stack_ptr;
	new_thread->next = NULL;

	//create queue node and add node to end of ready queue
	addThreadToQ(new_thread);

	return new_thread->threadID;
}

Tid
thread_yield(Tid want_tid)
{
	/*if(exit_q)
		clearExitQ();*/

	if(want_tid == THREAD_SELF || want_tid == current_thread->threadID)
		return current_thread->threadID;
	if(want_tid < -2 || want_tid > THREAD_MAX_THREADS -1 || (want_tid >=0 && tids_available[want_tid] == 0))
		return THREAD_INVALID;
	
	if(want_tid == THREAD_ANY)
	{
		if(ready_q != NULL)
		{
			volatile int in_loop = 1;
			struct thread *new_thread = removeHead();
			Tid newID = new_thread->threadID;
			addThreadToQ(current_thread);
			getcontext(&current_thread->context);

			if(in_loop)
			{
				in_loop = 0;
				current_thread = new_thread;
				setcontext(&new_thread->context);
			}
			return newID;
		}

		return THREAD_NONE;
	}
	else
	{
		if(tids_available[want_tid])
		{
			volatile int in_loop = 1;
			struct thread *new_thread = findAndRemoveThread(want_tid);
			Tid newID = new_thread->threadID;
			addThreadToQ(current_thread);
			getcontext(&current_thread->context);

			if(in_loop)
			{
				in_loop = 0;
				current_thread = new_thread;
				setcontext(&new_thread->context);
			}
			
			return newID;
		}

		return THREAD_INVALID;
	}
}

void
thread_exit()
{
	if(ready_q != NULL)
	{
		struct thread* new_thread = removeHead();

		//kill thread and switch
		tids_available[current_thread->threadID] = 0;
		free(current_thread->stack_ptr);
		current_thread->stack_ptr = NULL;
		free(current_thread);
		current_thread=NULL;


		current_thread = new_thread;
		setcontext(&current_thread->context);	
	}
	else
	{
		tids_available[current_thread->threadID] = 0;
		free(current_thread->stack_ptr);
		current_thread->stack_ptr = NULL;
		free(current_thread);
	}
	
}

Tid
thread_kill(Tid tid)
{
	Tid thread_murder = 0;
	if(tid < 1 || tid > THREAD_MAX_THREADS - 1 || tid == current_thread->threadID || tids_available[tid] == 0)
		return THREAD_INVALID;
	else
	{
		struct thread *thread_to_kill = findAndRemoveThread(tid);
		thread_murder = thread_to_kill->threadID;
		free(thread_to_kill->stack_ptr);
		thread_to_kill->stack_ptr = NULL;
		free(thread_to_kill);
		tids_available[tid] = 0;
	}
	
	return thread_murder;
}

/*******************************************************************
 * Important: The rest of the code should be implemented in Lab 3. *
 *******************************************************************/

/* make sure to fill the wait_queue structure defined above */
struct wait_queue *
wait_queue_create()
{
	struct wait_queue *wq;

	wq = malloc(sizeof(struct wait_queue));
	assert(wq);

	TBD();

	return wq;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
	TBD();
	free(wq);
}

Tid
thread_sleep(struct wait_queue *queue)
{
	TBD();
	return THREAD_FAILED;
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
int
thread_wakeup(struct wait_queue *queue, int all)
{
	TBD();
	return 0;
}

/* suspend current thread until Thread tid exits */
Tid
thread_wait(Tid tid)
{
	TBD();
	return 0;
}

struct lock {
	/* ... Fill this in ... */
};

struct lock *
lock_create()
{
	struct lock *lock;

	lock = malloc(sizeof(struct lock));
	assert(lock);

	TBD();

	return lock;
}

void
lock_destroy(struct lock *lock)
{
	assert(lock != NULL);

	TBD();

	free(lock);
}

void
lock_acquire(struct lock *lock)
{
	assert(lock != NULL);

	TBD();
}

void
lock_release(struct lock *lock)
{
	assert(lock != NULL);

	TBD();
}

struct cv {
	/* ... Fill this in ... */
};

struct cv *
cv_create()
{
	struct cv *cv;

	cv = malloc(sizeof(struct cv));
	assert(cv);

	TBD();

	return cv;
}

void
cv_destroy(struct cv *cv)
{
	assert(cv != NULL);

	TBD();

	free(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	TBD();
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	TBD();
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert(lock != NULL);

	TBD();
}

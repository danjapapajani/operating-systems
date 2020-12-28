#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include "thread.h"
#include "interrupt.h"
#include <stdbool.h>

enum state 
{
	READY = 1,
	RUNNING = 2,
	EXIT = 3,
	SLEEP = 4
};

/* This is the wait queue structure */
struct wait_queue {
	struct thread *blocked_q;
	struct wait_queue *next;
};

/* This is the thread control block */
struct thread {
	Tid threadID;
	ucontext_t context;
	void *stack_ptr;
	struct thread* next;
	bool homicide;
};

//global vars
struct thread *ready_q;
struct thread *exit_q;
struct thread *current_thread;
int tids_available[THREAD_MAX_THREADS];
struct wait_queue* wq_table[THREAD_MAX_THREADS];

void addThreadToReadyQ(struct thread* new_thread)
{
	if(new_thread == NULL)
		return;
		
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

void addThreadToExitQ(struct thread* new_thread)
{
	if(exit_q == NULL)
	{
		exit_q = new_thread;
		new_thread->next = NULL;
	}
	else
	{
		struct thread *current = exit_q;
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

struct thread* findAndRemoveThreadReadyQ(Tid id)
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

/*void clearExitQ()
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
}*/

void
thread_stub(void (*thread_main)(void *), void *arg)
{
	interrupts_on();
	//Tid ret;

	thread_main(arg);
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
	wq_table[0] = wait_queue_create();

	tids_available[0] = 1; 
	current_thread = kernal_thread;
}

Tid
thread_id()
{
	int enabled = interrupts_off();
	return current_thread->threadID;
	interrupts_set(enabled);
}

Tid
thread_create(void (*fn) (void *), void *parg)
{
	int enabled = interrupts_off();

	//error checks
	struct thread *new_thread = (struct thread*)malloc(sizeof(struct thread));
	if(new_thread == NULL)
	{
		interrupts_set(enabled);
		return THREAD_NOMEMORY;
	}

	for(int i = 1; i < THREAD_MAX_THREADS; i++)
	{
		if(tids_available[i] == 0)
		{
			new_thread->threadID = i;
			tids_available[i] = 1;
			break;
		}
		else if(tids_available[i] == 1 && i == THREAD_MAX_THREADS-1)
		{
			interrupts_set(enabled);
			return THREAD_NOMORE;
		}
	}

	//copy current context to new thread
	getcontext(&new_thread->context);

	//create stack ptr and check for mem err
	void* stack_ptr = malloc(THREAD_MIN_STACK);
	if(stack_ptr == NULL)
	{
		free(new_thread);
		interrupts_set(enabled);
		return THREAD_NOMEMORY;
	}

	//set up registers
	new_thread->context.uc_mcontext.gregs[REG_RDI] = (unsigned long)fn;
	new_thread->context.uc_mcontext.gregs[REG_RSI] = (unsigned long)parg;
	new_thread->context.uc_mcontext.gregs[REG_RIP] = (unsigned long)&thread_stub;
	new_thread->context.uc_mcontext.gregs[REG_RSP] = (unsigned long)stack_ptr + THREAD_MIN_STACK - 8;
	new_thread->stack_ptr = stack_ptr;
	new_thread->homicide = false;
	new_thread->next = NULL;
	wq_table[new_thread->threadID] = wait_queue_create();

	//create queue node and add node to end of ready queue
	addThreadToReadyQ(new_thread);

	interrupts_set(enabled);
	return new_thread->threadID;
}

Tid
thread_yield(Tid want_tid)
{
	int enabled = interrupts_off();

	if(want_tid == THREAD_SELF || want_tid == current_thread->threadID)
	{
		interrupts_set(enabled);
		return current_thread->threadID;
	}
	if(want_tid < -2 || want_tid > THREAD_MAX_THREADS -1 || (want_tid >=0 && tids_available[want_tid] == 0))
	{
		interrupts_set(enabled);
		return THREAD_INVALID;
	}
	
	if(want_tid == THREAD_ANY)
	{
		if(ready_q != NULL)
		{
			volatile int in_loop = 1;
			struct thread *new_thread = removeHead();
			Tid newID = new_thread->threadID;
			addThreadToReadyQ(current_thread);
			getcontext(&current_thread->context);

			if(in_loop)
			{
				in_loop = 0;
				current_thread = new_thread;
				if(new_thread->homicide)
				{
					interrupts_set(enabled);
					thread_exit();
				}

				setcontext(&new_thread->context);
			}

			interrupts_set(enabled);
			return newID;
		}

		interrupts_set(enabled);
		return THREAD_NONE;
	}
	else
	{
		if(tids_available[want_tid])
		{
			volatile int in_loop = 1;
			struct thread *new_thread = findAndRemoveThreadReadyQ(want_tid);
			Tid newID = new_thread->threadID;
			addThreadToReadyQ(current_thread);
			getcontext(&current_thread->context);

			if(in_loop)
			{
				in_loop = 0;
				current_thread = new_thread;
				if(new_thread->homicide)
				{
					interrupts_set(enabled);
					thread_exit();
				}

				setcontext(&new_thread->context);
			}
			
			interrupts_set(enabled);
			return newID;
		}

		interrupts_set(enabled);
		return THREAD_INVALID;
	}
}

void
thread_exit()
{	
	int enabled = interrupts_off();

	if(wq_table[current_thread->threadID] != NULL)
	{
		thread_wakeup(wq_table[current_thread->threadID], 1);
	}

	//kill thread and switch
	struct thread* new_thread = removeHead();

	tids_available[current_thread->threadID] = 0;
	free(current_thread->stack_ptr);
	current_thread->stack_ptr = NULL;
	wait_queue_destroy(wq_table[current_thread->threadID]);
	wq_table[current_thread->threadID] = NULL;
	free(current_thread);
	current_thread = NULL;

	current_thread = new_thread;
	setcontext(&current_thread->context);
	interrupts_set(enabled);
	
}

/*ok so we're gonna kill the thread if its in the readyq
 *but if shes sleeping, we'll set her to commit the next
 *time she runs
 */
Tid
thread_kill(Tid tid)
{
	int enabled = interrupts_off();

	Tid thread_murder = 0;
	bool thread_napping = false;
	if(tid > THREAD_MAX_THREADS - 1 || tid == current_thread->threadID || tids_available[tid] == 0)
	{
		interrupts_set(enabled);
		return THREAD_INVALID;
	}
	else
	{
		for(int i = 0; i < THREAD_MAX_THREADS; i++)
		{	
			bool found = false;
			if(wq_table[i] != NULL && wq_table[i]->blocked_q != NULL)
			{
				struct thread* current = wq_table[i]->blocked_q;
				while(current != NULL)
				{
					if(current->threadID == tid)
					{
						current->homicide = true;
						thread_murder = current->threadID;
						found = true;
						break;
					}

					current = current->next;
				}
			}

			if(found)
			{
				thread_napping = true;
				break;
			}
		}
		
		if(!thread_napping)
		{
			struct thread* thread_to_kill = findAndRemoveThreadReadyQ(tid);
			thread_murder = thread_to_kill->threadID;
			free(thread_to_kill->stack_ptr);
			thread_to_kill->stack_ptr = NULL;
			free(thread_to_kill);
			tids_available[tid] = 0;
		}
	}
	
	interrupts_set(enabled);
	return thread_murder;
}

/*******************************************************************
 * Important: The rest of the code should be implemented in Lab 3. *
 *******************************************************************/

/* make sure to fill the wait_queue structure defined above */
struct wait_queue *
wait_queue_create()
{
	int enabled = interrupts_off();
	struct wait_queue *wq;

	wq = malloc(sizeof(struct wait_queue));
	assert(wq);

	wq->blocked_q = NULL;
	wq->next = NULL;

	interrupts_set(enabled);
	return wq;
}

/*void printReadyQ()
{
	struct thread *current = ready_q;

	while(current)
	{
		printf("THREAD ID: %d\n HOMICIDE: %d\n",current->threadID, current->homicide);
		current = current->next;
	}
}*/

void
wait_queue_destroy(struct wait_queue *wq)
{
	int enabled = interrupts_off();

	if(wq->blocked_q == NULL)
		free(wq);

	interrupts_set(enabled);
}

void addThreadToWaitQ(struct wait_queue *wq, struct thread *new_thread)
{
	if(wq->blocked_q == NULL)
	{
		wq->blocked_q = new_thread;
		new_thread->next = NULL;
	}
	else
	{
		struct thread *current = wq->blocked_q;
		while(current->next != NULL)
		{
			current = current->next;
		}

		current->next = new_thread;
		new_thread->next = NULL;
	}

	return;
}

struct thread* findAndRemoveThreadWaitQ(int id, struct wait_queue* wq)
{
	struct thread *current = wq->blocked_q;

	if(current != NULL)
	{
		if(id == 1)
		{
			wq->blocked_q = NULL;
			return current;
		}
		else
		{
			wq->blocked_q = current->next;
			current->next = NULL;
			return current;
		}
	}

	return NULL;
}

Tid
thread_sleep(struct wait_queue *queue)
{
	int enabled = interrupts_off();

	if(queue == NULL)
	{
		interrupts_set(enabled);
		return THREAD_INVALID;
	}

	if(ready_q != NULL)
	{
		volatile int in_loop = 1;
		struct thread *new_thread = removeHead();
		Tid newID = new_thread->threadID;
		addThreadToWaitQ(queue, current_thread);
		getcontext(&current_thread->context);

		if(in_loop)
		{
			in_loop = 0;
			current_thread = new_thread;
			setcontext(&new_thread->context);
		}

		interrupts_set(enabled);
		return newID;
	}

	interrupts_set(enabled);
	return THREAD_NONE;
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
int
thread_wakeup(struct wait_queue *queue, int all)
{
	int enabled = interrupts_off();

	if(queue == NULL)
	{
		interrupts_set(enabled);
		return 0;
	}
	else if(all == 1)
	{
		struct thread* ll_of_threads = findAndRemoveThreadWaitQ(1, queue);
		int num_of_threads = 0;

		while(ll_of_threads != NULL)
		{
			num_of_threads++;

			struct thread* current = ll_of_threads;
			ll_of_threads = ll_of_threads->next;
			addThreadToReadyQ(current);
		}

		interrupts_set(enabled);
		return num_of_threads;
	}
	else
	{
		struct thread* current = findAndRemoveThreadWaitQ(0, queue);
		addThreadToReadyQ(current);

		interrupts_set(enabled);
		return 1;
	}
}

/* suspend current thread until Thread tid exits */
/* please don't come for me 
 * I know this function is soooo ugly and inefficient but I have
 * a midterm for a grad course that is in the field I want to
 * do my MASc in so I had to get this done BFC style */
Tid
thread_wait(Tid tid)
{
	int enabled = interrupts_off();
	if(tid > THREAD_MAX_THREADS - 1 || tid == current_thread->threadID || tids_available[tid] == 0)
	{
		interrupts_set(enabled);
		return THREAD_INVALID;
	}

	Tid id = thread_sleep(wq_table[tid]);

	if(current_thread->homicide && wq_table[current_thread->threadID]->blocked_q == NULL && ready_q == NULL)
		exit(0);
	else if(current_thread->homicide)
		thread_exit();
	
	if(tid > THREAD_MAX_THREADS - 1 || tid == current_thread->threadID || tids_available[tid] == 0)
	{
		interrupts_set(enabled);
		return THREAD_INVALID;
	}

	interrupts_set(enabled);
	return id;
}

struct lock {
	int acquired;
	struct wait_queue* wq;
};

int faux_tset(struct lock* curr_lock)
{
	int old = curr_lock->acquired;
	curr_lock->acquired = 1;
	return old;
}

struct lock *
lock_create()
{
	int enabled = interrupts_off();

	struct lock *lock;
	lock = malloc(sizeof(struct lock));
	assert(lock);

	lock->acquired = 0;
	lock->wq = wait_queue_create();

	interrupts_set(enabled);
	return lock;
}

void
lock_destroy(struct lock *lock)
{
	int enabled = interrupts_off();
	assert(lock != NULL);

	if(!lock->acquired)
	{
		wait_queue_destroy(lock->wq);
		lock->wq = NULL;
		free(lock);
	}

	interrupts_set(enabled);
}

void
lock_acquire(struct lock *lock)
{
	int enabled = interrupts_off();
	assert(lock != NULL);

	while(faux_tset(lock))
		thread_sleep(lock->wq);

	interrupts_set(enabled);
}

void
lock_release(struct lock *lock)
{
	int enabled = interrupts_off();
	assert(lock != NULL);

	if(lock->acquired)
	{
		lock->acquired = 0;
		thread_wakeup(lock->wq, 1);
	}

	interrupts_set(enabled);
}

struct cv {
	struct wait_queue* wq;
};

struct cv *
cv_create()
{
	int enabled = interrupts_off();
	struct cv *cv;

	cv = malloc(sizeof(struct cv));
	assert(cv);
	cv->wq = wait_queue_create();

	interrupts_set(enabled);
	return cv;
}

void
cv_destroy(struct cv *cv)
{
	int enabled = interrupts_off();
	assert(cv != NULL);

	if(!cv->wq)
	{
		wait_queue_destroy(cv->wq);
		free(cv->wq);
		free(cv);
	}

	interrupts_set(enabled);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_off();
	assert(cv != NULL);
	assert(lock != NULL);

	if(lock->acquired)
	{
		lock_release(lock);
		thread_sleep(cv->wq);
		lock_acquire(lock);
	}

	interrupts_set(enabled);
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_off();
	assert(cv != NULL);
	assert(lock != NULL);

	if(lock->acquired)
	{
		thread_wakeup(cv->wq, 0);
	}
	
	interrupts_set(enabled);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_off();
	assert(cv != NULL);
	assert(lock != NULL);

	if(lock->acquired)
	{
		thread_wakeup(cv->wq, 1);
	}
	
	interrupts_set(enabled);
}

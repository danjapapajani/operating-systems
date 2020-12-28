#include "request.h"
#include "server_thread.h"
#include "common.h"
#include <pthread.h>
#include <stdbool.h>

pthread_cond_t full = PTHREAD_COND_INITIALIZER; 
pthread_cond_t empty = PTHREAD_COND_INITIALIZER; 
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache = PTHREAD_MUTEX_INITIALIZER;

struct entry
{
	struct file_data *data;
	struct entry *next;
	int in_use;
	bool removed;
}; 

struct LRU 
{
	struct LRU *next;
	char* name;
	int size;
};

struct cache_table
{
	struct entry** hash_table;
	int size;
	int curr_size;
	int max_cache_size;

	struct LRU *LRU;
	bool init;
};

struct cache_table *cache_table;

int hashFunc(char *word, long size)
{
	unsigned long hash = 5381;
    int c;

    while ((c = *word++))
        hash = ((hash << 5) + hash) + c; 

    return hash % size;
}

struct server {
	int nr_threads;
	int max_requests;
	int max_cache_size;
	int exiting;
	int* req_buffer;
	int in;
	int out;
	pthread_t* t_workers;
};

/* static functions */

/* initialize file data */
static struct file_data *
file_data_init(void)
{
	struct file_data *data;

	data = Malloc(sizeof(struct file_data));
	data->file_name = NULL;
	data->file_buf = NULL;
	data->file_size = 0;
	return data;
}

/* free all file data */
static void
file_data_free(struct file_data *data)
{
	free(data->file_name);
	free(data->file_buf);
	free(data);
}

void lruAdd(struct file_data* data)
{
	struct LRU* curr = cache_table->LRU;
	struct LRU* prev = NULL;

	struct LRU* new = (struct LRU*)malloc(sizeof(struct LRU));
	new->name = strdup(data->file_name);
	new->size = data->file_size;
	new->next = NULL;

	if(cache_table->init != false) 
	{
		//get to end and add
		while(curr)
		{
			prev = curr;
			curr = curr->next;
		}

		prev->next = new;
		cache_table->init = true;
	}
	else
	{
		cache_table->LRU = new;
		cache_table->LRU->next = NULL;
	}
}

void lruUpdate(struct entry* entry) 
{
	if(cache_table->LRU == NULL)
		return;
	else if (cache_table->LRU->next == NULL)
		return;
	else if(strcmp(cache_table->LRU->name, entry->data->file_name) == 0) //at front
	{
		//get to end
		struct LRU* last = cache_table->LRU;
		struct LRU* temp = cache_table->LRU;
		while(last->next)
			last = last->next;

		cache_table->LRU = cache_table->LRU->next;
		last->next = last;
		temp->next = NULL;
	}
	else
	{
		struct LRU* curr = cache_table->LRU;
		int found = 0;

		//traverse to find
		while(curr->next)
		{
			if(strcmp(curr->next->name, entry->data->file_name) == 0) 
			{
				found = 1;
				break;
			}

			curr = curr->next;
		}

		if(curr->next != NULL && curr->next->next != NULL && found)
		{
			struct LRU* remove = curr->next;

			//get to end
			while(remove->next)
				remove = remove->next;

			//swap positions
			struct LRU* temp = curr->next;
			curr->next = curr->next->next;
			remove->next = temp;
			temp->next = NULL;
		}
	}
	
}


struct entry* cache_lookup(char* word)
{
	int hash = hashFunc(word, cache_table->size);

	if(cache_table->hash_table[hash] == NULL)
		return NULL;
	else
	{
		struct entry* current = cache_table->hash_table[hash];

		while(current)
		{
			if(strcmp(current->data->file_name, word) == 0) return current;
			else
				current = current->next;
		}
	}

	return NULL;
}


bool cache_evict(int file_size)
{
	struct LRU* curr = cache_table->LRU;
	int bytes = 0;

	while(curr != NULL && bytes < cache_table->curr_size + file_size - cache_table->max_cache_size)
	{
		bytes = bytes + curr->size;
		
		//remove from LRU
		struct LRU* front = cache_table->LRU;
		cache_table->LRU = cache_table->LRU->next;

		struct entry* remove = cache_lookup(front->name);
		remove = remove->next;

		curr = curr->next;
	}
	
	if(bytes >= cache_table->curr_size + file_size - cache_table->max_cache_size)
		return true;

	return false;
}

struct entry* cache_insert(const struct file_data* data){

	if(data->file_size > cache_table->max_cache_size) 
		return NULL;

	if(cache_table->curr_size + data->file_size > cache_table->max_cache_size)
    {
        bool has_space = cache_evict(data->file_size);
        if(!has_space)
			return NULL;
    }

	cache_table->curr_size = cache_table->curr_size + data->file_size;

	int hash = hashFunc(data->file_name, cache_table->size);
	int repeat = 0;

    struct entry* cache_entry = (struct entry*)malloc(sizeof(struct entry));
    cache_entry->next = NULL;
    cache_entry->data = file_data_init();
	cache_entry->data->file_buf = strdup(data->file_buf);
	cache_entry->data->file_name = strdup(data->file_name);
	cache_entry->data->file_size = data->file_size;
    cache_entry->in_use = 0;
	cache_entry->removed = false;

	if(cache_table->hash_table[hash] == NULL) {
		cache_table->hash_table[hash] = cache_entry;
	}
	else
	{
		struct entry *prev = NULL;
		struct entry *curr = cache_table->hash_table[hash];

		while(curr != NULL) 
		{
			if(strcmp(curr->data->file_name, data->file_name) == 0) {
				repeat = 1;
				break;
			}

			prev = curr;
			curr = prev->next;
		}

		if(repeat == 0)
			prev->next = cache_entry;
	}

	lruAdd(cache_entry->data);
	return cache_entry;
}


static void
do_server_request(struct server *sv, int connfd)
{
	int ret;
	struct request *rq;
	struct file_data *data;

	data = file_data_init();

	rq = request_init(connfd, data);
	if (!rq) {
		file_data_free(data);
		return;
	}
	
	if(sv->max_cache_size < 1)
	{
		
		ret = request_readfile(rq);
        if (ret != 0)
            request_sendfile(rq);

        request_destroy(rq);
        file_data_free(data);
		return;
	}

	pthread_mutex_lock(&cache);
	struct entry* to_cache = cache_lookup(data->file_name);

	if(to_cache == NULL)
	{
		pthread_mutex_unlock(&cache);
		ret = request_readfile(rq);
		if (ret == 0) 
		{
			goto out;
		}
		pthread_mutex_lock(&cache);

		to_cache = cache_lookup(data->file_name);
		if(to_cache != NULL)
		{ 
			request_set_data(rq,to_cache->data);
			to_cache->in_use++;

			lruUpdate(to_cache);
		}
		else
		{
			to_cache = cache_insert(data);
			if(to_cache != NULL)
				to_cache->in_use++;
		}
	}
	else
	{
		request_set_data(rq,to_cache->data);
		to_cache->in_use++;
		lruUpdate(to_cache);
	}

	pthread_mutex_unlock(&cache);
	request_sendfile(rq);
out:
	if(to_cache != NULL)
	{
		pthread_mutex_lock(&cache);
		to_cache->in_use--;
		pthread_mutex_unlock(&cache);
	}

	request_destroy(rq);
	file_data_free(data);
}

/* entry point functions */

//prod-consume RECIEVE
void startRequest(struct server *sv)
{
	while(1)
	{
		pthread_mutex_lock(&lock);

		while(sv->in == sv->out)
		{
			pthread_cond_wait(&empty, &lock);
			if(sv->exiting)
			{
				pthread_mutex_unlock(&lock);
				pthread_exit(NULL);
			}
		}

		int elem = sv->req_buffer[sv->out];

		if(((sv->in - sv->out + sv->max_requests) % sv->max_requests) == sv->max_requests - 1)
			pthread_cond_broadcast(&full);

		sv->out = (sv->out + 1) % sv->max_requests;
		pthread_mutex_unlock(&lock);
		do_server_request(sv, elem);
	}

}


struct server *
server_init(int nr_threads, int max_requests, int max_cache_size)
{
	struct server *sv;

	pthread_mutex_lock(&lock);

	sv = Malloc(sizeof(struct server));
	sv->nr_threads = nr_threads;
	sv->max_requests = max_requests + 1;
	sv->max_cache_size = max_cache_size;
	sv->exiting = 0;
	sv->in = 0;
	sv->out = 0;
	sv->req_buffer = NULL;
	
	if (nr_threads > 0 || max_requests > 0 || max_cache_size > 0) {

		if(nr_threads > 0)
		{
			sv->t_workers = (pthread_t*)malloc(nr_threads * sizeof(pthread_t));

			for(int i = 0; i < nr_threads; i++)
				pthread_create(&sv->t_workers[i], NULL, (void*) &startRequest, (void*) sv);

		}

		if(max_requests > 0)
			sv->req_buffer = (int*)malloc(max_requests * sizeof(int));

		if(max_cache_size > 0)
		{
			cache_table = (struct cache_table *)malloc(sizeof(struct cache_table));
			cache_table->size = max_cache_size;
			cache_table->max_cache_size = max_cache_size;
			cache_table->hash_table = (struct entry**)malloc(cache_table->size *sizeof(struct entry *));
			cache_table->curr_size = 0;
			cache_table->LRU = NULL;
			cache_table->init = false;

			for(int i = 0; i < cache_table->size; i++)
				cache_table->hash_table[i] = NULL;
		}
	}

	/* Lab 4: create queue of max_request size when max_requests > 0 */

	/* Lab 5: init server cache and limit its size to max_cache_size */

	/* Lab 4: create worker threads when nr_threads > 0 */

	pthread_mutex_unlock(&lock);
	return sv;
}

//prod-cons SEND
void
server_request(struct server *sv, int connfd)
{
	if (sv->nr_threads == 0) { /* no worker threads */
		do_server_request(sv, connfd);
	} else {
		/*  Save the relevant info in a buffer and have one of the
		 *  worker threads do the work. */
		pthread_mutex_lock(&lock);

		while(((sv->in - sv->out + sv->max_requests) % sv->max_requests) == sv->max_requests - 1 && sv->exiting == 0)
			pthread_cond_wait(&full, &lock);

		sv->req_buffer[sv->in] = connfd;

		if(sv->in == sv->out)
			pthread_cond_broadcast(&empty);

		sv->in = (sv->in + 1) % sv->max_requests;
		pthread_mutex_unlock(&lock);
	}
}

void
server_exit(struct server *sv)
{
	/* when using one or more worker threads, use sv->exiting to indicate to
	 * these threads that the server is exiting. make sure to call
	 * pthread_join in this function so that the main server thread waits
	 * for all the worker threads to exit before exiting. */
	sv->exiting = 1;

	//printf("we're leaving\n");

	//wake up all sleeping threads
	pthread_cond_broadcast(&empty);
	pthread_cond_broadcast(&full);

	for(int i = 0; i < sv->nr_threads; i++)
		pthread_join(sv->t_workers[i], NULL);


	free(cache_table->hash_table);

	free(sv->t_workers);
	free(sv->req_buffer);

	/* make sure to free any allocated resources */
	free(sv);
}

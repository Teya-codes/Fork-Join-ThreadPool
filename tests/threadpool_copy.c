#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "threadpool.h"
#include "list.h"

struct thread_pool {

    //list of all threads.
    struct list workers;
    
    //global queue of futures.
    struct list global_queue;

    //Lock for thredpool.
    pthread_mutex_t lock;

    pthread_cond_t cond;

    pthread_barrier_t barrier;

    int num_threads;

    //Shutdown Marker Flag used in function of same name
    bool shutdown;

};

//wrapper struct for task (& its result.)
struct future {

    //function pointer for task.
    fork_join_task_t task;

    //holds data for computation.
    void * data;

    //holds the result of computation.
    void * result;
    
    //int to indicate status.
    // 0 means the task has not been started.
    // 1 means the task is in progress.
    // 2 means the task is finished.
    int status;

    //condition variable.
    pthread_cond_t task_is_done;

    //Used to add future_elem to pool's global queue and threads' local queues
    struct list_elem elem;

    //Have to Add threadpool to access locking and unlocking
    struct thread_pool *pool;


};

//This is Essentially the Worker Struct
struct worker {
    pthread_t thread_id;
    struct list local_queue;
    struct list_elem thread_elem;
    bool is_worker_thread;

    struct list_elem elem;
};

static _Thread_local struct worker * current_worker;

//The function run by each thread.
static void * thread_function(void *);


struct thread_pool * thread_pool_new(int nthreads) {

    //allocating space.
    struct thread_pool * pool = malloc(sizeof(struct thread_pool));
    if (pool == NULL) {
        printf("Error: Malloc returned null for thread_pool");
        return NULL;
    }
    //initializing variables.
    if (pthread_mutex_init(&pool->lock, NULL) != 0){ 
        printf("Error: pthread_mutex_init returned null");
        return NULL;
    }
    if (pthread_cond_init(&pool->cond, NULL) != 0){ 
        printf("Error: pthread_cond_init returned null");
        return NULL;
    }
    if (pthread_barrier_init(&pool->barrier, NULL, nthreads + 1) != 0){ 
        printf("Error: pthread_barrier_init returned null");
        return NULL;
    }

    //TODO: Initialize other variables.

    //locking.
    pthread_mutex_lock(&pool->lock);

    //TODO: Main code goes here.

    list_init(&pool->workers);
    list_init(&pool->global_queue);
    pool->num_threads = nthreads;
    pool->shutdown= false; //init it false beforehand

    for (int i = 0; i < nthreads; i++) {
        
        //Making new thread wrapper to hold thread_id and provide local_queue.
        struct worker* new_worker = malloc(sizeof(struct worker));
        if (new_worker == NULL) {
            printf("Error: malloc returned null for worker");
            return NULL;
        }

        //Initializaing thread wrapper.(Worker)
        list_init(&new_worker->local_queue);

        //Adding thread_wrapper (worker) to pool's internal threads list.
        list_push_front(&pool->workers, &new_worker->thread_elem);

        if (pthread_create(&new_worker->thread_id, NULL, thread_function, pool) != 0) {
            printf("Error: tried creating new thread.");
            return NULL;
        }
    }

    //Setting up current_thread variable to allow the program execution to know what thread it is in.
    current_worker = malloc(sizeof(struct worker));
    if (current_worker == NULL) {
        printf("Error: tried mallocing space for current_thread.");
        return NULL;
    }

    //Initializing values for current_thread.
    current_worker->thread_id = pthread_self();
    current_worker->is_worker_thread = false;
    list_init(&current_worker->local_queue);

    //Unlocking.
    pthread_mutex_unlock(&pool->lock);

    //waiting for all threads to catch up.
    pthread_barrier_wait(&pool->barrier);

    return pool;
}

//adds shutdwon flag and frees the variable 
void thread_pool_shutdown_and_destroy(struct thread_pool * pool) {


   //Locking.
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = true;
    
    //wake the threads up to let them shutdown.
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    //Joining all the threads.
    for (struct list_elem * c_worker_elem = list_begin(&pool->workers);
         c_worker_elem != list_end(&pool->workers);
         c_worker_elem = list_next(c_worker_elem)) {

        struct worker * c_worker = list_entry(c_worker_elem, struct worker, elem);
        if (pthread_join(c_worker->thread_id, NULL) != 0) {
            printf("Error: Couldn't join threads.");
        } 
    }
   
    //freeing the structs.
    while (!list_empty(&pool->workers)) {
        struct list_elem * c_worker_elem = list_pop_front(&pool->workers);
        struct worker * c_worker = list_entry(c_worker_elem, struct worker, elem);
        free(c_worker);
    }
   
   //destroy the old thread info 
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    pthread_barrier_destroy(&pool->barrier);
    free(pool);
}

struct future * thread_pool_submit(
        struct thread_pool *pool, 
        fork_join_task_t task, 
        void * data) {

    //Locking
    pthread_mutex_lock(&pool->lock);

    //Allocating space for new_future (new task wrapper)
    struct future * new_future = malloc(sizeof(struct future));
    
    if (new_future == NULL) {
        printf("Error: malloc returned null for new_task in thread_pool_submit()");
        return NULL;
    } 

    //Initializing all variables for new_future.

    //new_future->cond is used to tell if the task new_future represents is done.
    if (pthread_cond_init(&new_future->task_is_done, NULL) != 0) {
        printf("Error: pthread_cond_init() returned NULL for new_future->cond");
        return NULL;
    }

    new_future->task = task;
    new_future->data = data;
    new_future->result = NULL; //result not calculated yet.
    new_future->status = 1;

    //if current thread is a worker, the task should be pushed to the worker's local_queue.
    //else, the task should be pushed to the pool's global_queue.
    if (current_worker->is_worker_thread) {
        //TODO: list_push_front or list_push_back ? 
        //      Should local_queue be treated as a stack ?
        //In the video @6:40 he mentions how workers handle tasks workers in their
        //local queue want to finish the first task/newest

        list_pop_front(&current_worker->local_queue);
        // ^^^ do we need to pop from the future does future need list_elem added to struct?
        //might this later for work stealing/helping later?
    } else {
        //TODO: list_push_front or list_push_back ? 
        //      Should global_queue be treated as a stack ?
    }

    //Letting the pool know that it has work to do.
    pthread_cond_signal(&pool->cond);

    //Unlocking
    pthread_mutex_unlock(&pool->lock);


    return new_future;
}

//Looks at task made
void * future_get(struct future * f) {

    void * retVal = NULL;


    //Locking.
    pthread_mutex_lock(&f->pool->lock);

    if (f->status == 0) {
        
        //Getting the future elem.
        list_remove(&f->elem);
        
        //Changing the future's status to in progress.
        f->status = 1;

        //Unlocking to let the called task execute properly
        pthread_mutex_unlock(&f->pool->lock);

        //Running the task.
        f->result = (f->task) (f->pool, f->data);

        //Locking.
        pthread_mutex_lock(&f->pool->lock);

        retVal = f->result;

        //Changing the future's status to completed.
        f->status = 2;

    } else {
        while (f->status != 2) {
            pthread_cond_wait(&f->task_is_done, &f->pool->lock);
        }
    }

    pthread_mutex_unlock(&f->pool->lock);

    return retVal;
}

//Deallocates the future used in Submit used after future_get();
void future_free(struct future * f) {

    //Nulling out the variables just in case.
    f->task = NULL;
    f->data = NULL;
    f->result = NULL;
    
    //"Destroying" the condition variable.
    pthread_cond_destroy(&f->task_is_done);

    //Freeing f.
    free(f);

}

static bool is_pool_inactive(struct thread_pool * pool) {
    
    if (!list_empty(&pool->global_queue)) return false;
    if (pool->shutdown) return false;
    
    for (struct list_elem * current_thread_elem = list_begin(&pool->workers);
         current_thread_elem != list_end(&pool->workers);
         current_thread_elem = list_next(current_thread_elem)) {

        struct worker * current_thread = list_entry(current_thread_elem, struct worker, elem);

        if (!list_empty(&current_thread->local_queue)) return false;

    }

    return true;
}

static struct list_elem * steal_task(struct thread_pool * pool) {

    for (struct list_elem * current_worker_elem = list_begin(&pool->workers);
         current_worker_elem != list_end(&pool->workers);
         current_worker_elem = list_next(current_worker_elem)) {

        struct worker * c_worker = list_entry(current_worker_elem, struct worker, elem);
        if (!list_empty(&c_worker->local_queue)) return list_pop_back(&c_worker->local_queue);
    }
    return NULL;
}

//param is a void pointer for the threadpool struct.
//worker thread funciton
static void * thread_function(void * param) {

    struct thread_pool * pool = (struct thread_pool *) param;

    bool need_setup = true;

    //Use barrier to check whether all the threads have been created.
    pthread_barrier_wait(&pool->barrier);

    while (true) {

        //Locking.
        pthread_mutex_lock(&pool->lock);

        if (pool->shutdown) break;

        //While pool is inactive, no tasks to do, keep looping and checking for a new task.
        while(is_pool_inactive(pool)) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        //When the thread is first created, we need to setup some values.
        if (need_setup) {
            for (struct list_elem * current_worker_elem = list_begin(&pool->workers);
                 current_worker_elem != list_end(&pool->workers);
                 current_worker_elem = list_next(current_worker_elem)) {
                
                struct worker * current_worker_set = list_entry(current_worker_elem, struct worker, elem);
                if (current_worker_set->thread_id == pthread_self()) {
                    
                    current_worker = current_worker_set;
                    current_worker->is_worker_thread = true;
                    need_setup = false;
                    
                    break;
                }

            }

        }

        //Giving task to thread.
        struct list_elem * future_to_run_elem;

        //Checking if there are any tasks in the local queue.
        
        //If not, we check the global queue.
        if (list_empty(&current_worker->local_queue)) {
            
            // Checking if there any tasks in the global queue.

            //If not, we steal a task.
            if (list_empty(&pool->global_queue)){
                future_to_run_elem = steal_task(pool);
            }
            //If there are tasks in the global queue we take one.
            else {
                future_to_run_elem = list_pop_front(&pool->global_queue);
            }

        }
        //If there are tasks in the local queue.
        else {
            future_to_run_elem = list_pop_front(&current_worker->local_queue);
        }

        if (future_to_run_elem == NULL) {
            printf("No task found. is_pool_inactive is not working correctly.");
            break;
        }

        struct future * future_to_run = list_entry(future_to_run_elem, struct future, elem);


        future_to_run->status = 1; //running the task

        //Unlocking and Locking to allow the task to execute.

        pthread_mutex_unlock(&pool->lock);

        future_to_run->result = (future_to_run->task)(pool, future_to_run->data);

        pthread_mutex_unlock(&pool->lock);

        future_to_run->status = 2; //completed the task.


        //sending the signal that the task is done.
        pthread_cond_signal(&future_to_run->task_is_done);




        //Unlocking.
        pthread_mutex_unlock(&pool->lock);
    }
    
    //Unlocking.
    pthread_mutex_unlock(&pool->lock);

    pthread_exit(0);
    return NULL;

}
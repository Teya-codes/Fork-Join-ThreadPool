#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#include "threadpool.h"
#include "list.h"

/**
 * @brief Thread_pool is essentially just a collection of workers 
 * (struct workers) that holds all the global variables these workers need
 * to synchronize with each other.
 * 
 * lock is the global lock used to protect all the "global" variables.
 * cond is the condition variable that is used to wake up all the threads during shutdown.
 * barrier is used to sync up the start of each of the workers' execution of the thread_function.
 * 
 * shutdown is a boolean value for when the shutdown_and_destroy function is called.
 * 
 * workers is the list containing all the worker threads.
 * global queue is the list of all tasks that are submitted externally.
 * 
 */
struct thread_pool {

    //list of all threads.
    struct list workers;
    
    //global queue of futures.
    struct list global_queue;

    //Lock for threadpool.
    pthread_mutex_t lock;

    pthread_cond_t cond;

    pthread_barrier_t barrier;

    //Shutdown Marker Flag used in function of same name
    bool shutdown;

};
/**
 * @brief future can be seen as a task-wrapper, it holds 
 * fork_join_task_t, a function pointer for the task it needs to run, 
 * data, the data it needs to pass to the function.
 * result, the result of the computation that will be figured out once 
 *         the task is done running.
 * status, which indicates which stage the task is currently in. 
 *         0, if it hasn't started yet.
 *         1, if it is in progress.
 *         2, if it has completed its computation.
 * task_is_done, which is a pthread_cond_t value used to signal to future_get
 *         whether the task has been completed.
 * pool, which is used to access the pool, and through the pool, the workers 
 *         to assign this task to.
 * 
 */
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

    //Have to add threadpool to access locking and unlocking for future_get
    struct thread_pool *pool;


};


/**
 * @brief The worker struct can be viewed as a thread-wrapper.
 * It holds the local queue that each thread pulls tasks out of.
 * And it contains a list_elem to allow the thread_pool to use 
 * list.c to traverse through a list of workers. Aside from that the 
 * worker struct holds thread_id, a pthread_t that refers to the thread 
 * this specific worker is meant to hold values for.
 * 
 */
struct worker {
    pthread_t thread_id;
    struct list local_queue;
    struct thread_pool * pool;

    struct list_elem elem;
};

static _Thread_local struct worker * current_worker;

//Used to check when the pool has no tasks, and hasn't shutdown yet.
static bool is_pool_inactive(struct thread_pool * pool);

//Used for stealing tasks when the global_queue is empty.
static struct list_elem * steal_task(struct thread_pool * pool);

//The function run by each thread.
static void * thread_function(void *);

/**
 * @brief thread_pool_new is used to initialize the worker threads, all related variables,
 * and call pthread_create 'nthreads' times to create the appropriate amount of threads.
 * 
 * @param nthreads - the number of threads to be created.
 * @return struct thread_pool* 
 */
struct thread_pool * thread_pool_new(int nthreads) {

    //Allocating space for the thread pool.
    struct thread_pool * pool = malloc(sizeof(struct thread_pool));
    if (pool == NULL) {
        printf("Error: Malloc returned null for thread_pool");
        return NULL;
    }

    //Initializing variables.

    //Initializing lock.
    if (pthread_mutex_init(&pool->lock, NULL) != 0){ 
        printf("Error: pthread_mutex_init returned null");
        return NULL;
    }
    //Initializing condition variable.
    if (pthread_cond_init(&pool->cond, NULL) != 0){ 
        printf("Error: pthread_cond_init returned null");
        return NULL;
    }
    //Initializing barrier.
    if (pthread_barrier_init(&pool->barrier, NULL, nthreads + 1) != 0){ 
        printf("Error: pthread_barrier_init returned null");
        return NULL;
    }

    //Initializing worker list.
    list_init(&pool->workers);
    //Initializing global queue (also a list.)
    list_init(&pool->global_queue);
    //Setting shutdown to false.
    pool->shutdown= false;



    //Locking.
    pthread_mutex_lock(&pool->lock);

    for (int i = 0; i < nthreads; i++) {

        //Allocating space for new_worker
        struct worker* new_worker = malloc(sizeof(struct worker));
        if (new_worker == NULL) {
            printf("Error: malloc returned null for worker");
            return NULL;
        }

        //Initializing local_queue for the new_worker.
        list_init(&new_worker->local_queue);
        //Allowing the new_worker the option to access thread_pool via pool.
        new_worker->pool = pool;

        //Adding worker to pool's internal workers list.
        list_push_front(&pool->workers, &new_worker->elem);

        //Creating the thread and storing the thread_id in the new_worker's thread_id field.
        if (pthread_create(&new_worker->thread_id, NULL, thread_function, new_worker) != 0) {
            printf("Error: tried creating new thread.");
            return NULL;
        }
    }

    //Unlocking.
    pthread_mutex_unlock(&pool->lock);

    //Waiting for all the threads to catchup.
    pthread_barrier_wait(&pool->barrier);

    //Returning the thread pool we just created.
    return pool;
}

/**
 * @brief thread_pool_shutdown_and_destroy() toggles on the shutdown flag,
 * and waits for all the threads to shutdown, using pthread_join. Then it proceeds to
 * empty out and free all the workers in the worker list.
 * 
 * @param pool 
 */
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

/**
 * @brief thread_pool_submit is used to submit new tasks to the threadpool.
 * 
 * @param pool - thread pool
 * @param task - new task to submit.
 * @param data - data associated with new task
 * @return struct future* - returns new future.
 */
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

    //Initializing task_is_done (condition variable)
    if (pthread_cond_init(&new_future->task_is_done, NULL) != 0) {
        printf("Error: pthread_cond_init() returned NULL for new_future->cond");
        return NULL;
    }

    //The task the future needs to do.
    new_future->task = task;
    //The data needed by the task.
    new_future->data = data;

    //The result of the computation.
    new_future->result = NULL; //Result not calculated yet.
    
    //The status of the task
    new_future->status = 0;   //Not started yet.

    //Pointer to pool, to allow giving tasks to workers in pool's internal worker list.
    new_future->pool = pool;


    //If the current_worker is NOT null, then the task is internal,
    //and it should be added to the current_worker's internal queue.
    if (current_worker != NULL) {
        //push_front() since the local_queue is treated like a stack (LIFO),
        //internal tasks that are spawned are typically depended on by previous tasks.
        list_push_front(&current_worker->local_queue, &new_future->elem);
    }
    //If the current_worker is null, the the task is external,
    //and it should be added to the pool's global queue. 
    else {
        //push_back() since the global_queue is treated like a convential queue (FIFO)
        list_push_back(&pool->global_queue, &new_future->elem);
    }

    //Sending the pool a signal to let it know it has more work to do.
    //Helps if the pool is currently inactive.
    pthread_cond_signal(&pool->cond);

    //Unlocking
    pthread_mutex_unlock(&pool->lock);


    return new_future;
}

/**
 * @brief future_get is used by external caller's to get the result of a computation.
 * if the task associated with the computation hasn't been started yet, future_gets runs the
 * computation right then and there, otherwise it waits until the future is done executing.
 * 
 * @param f - future to get result for.
 * @return void* - result.
 */
void * future_get(struct future * f) {

    //Locking.
    pthread_mutex_lock(&f->pool->lock);

    void * retVal = NULL;

    //Executing task if future hasn't started yet.
    if (f->status == 0) {
        
        //Getting the future elem.
        list_remove(&f->elem);
        
        //Changing the future's status to in progress.
        f->status = 1;

        //Unlocking to let the called task execute properly
        pthread_mutex_unlock(&f->pool->lock);

        //Running the task.
        f->result = f->task(f->pool, f->data);

        //Locking again.
        pthread_mutex_lock(&f->pool->lock);

        //Setting the return value.
        retVal = f->result;

        //Changing the future's status to completed.
        f->status = 2;

    } 
    //Waiting on future if it has already started.
    else {
        
        while (f->status != 2) {
            pthread_cond_wait(&f->task_is_done, &f->pool->lock);
        }
        retVal = f->result;
    }
    //Unlocking.
    pthread_mutex_unlock(&f->pool->lock);

    return retVal;
}

//Deallocates the future used in Submit used after future_get();
/**
 * @brief future_free is used to free all the space associated with the
 * future 'f' and destroy it's task_is_done condition variable.
 * 
 * @param f 
 */
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

/**
 * @brief thread_function is the function run by all the threads in the thread_pool.
 * @param param - used to pass in the current_worker to the thread_function.
 * @return void* 
 */
static void * thread_function(void * param) {

    //Getting current_worker from param.
    current_worker = (struct worker *) param;

    //Getting pool from current_worker.
    struct thread_pool * pool = current_worker->pool;

    //Using barrier to check whether all the threads have been created.
    pthread_barrier_wait(&pool->barrier);

    while (true) {

        //Locking.
        pthread_mutex_lock(&pool->lock);

        while (is_pool_inactive(pool)) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        // while (list_empty(&pool->global_queue) && list_empty(&current_worker->local_queue)) {
        //     if (pool->shutdown) break;
        //     pthread_cond_wait(&pool->cond, &pool->lock);
        // }

        if (pool->shutdown) break;


        assert(current_worker != NULL);
        //Giving task to thread.
        struct list_elem * future_to_run_elem;

        //Checking if there are any tasks in the local queue.
        
        //If not, we check the global queue.
        if (list_empty(&current_worker->local_queue)) {

            // if (list_empty(&pool->global_queue)){
            //     future_to_run_elem = NULL;
            // } else {
            //     future_to_run_elem = list_pop_front(&pool->global_queue);
            // }

            if (list_empty(&pool->global_queue)){
                future_to_run_elem = steal_task(pool);
            } else {
                future_to_run_elem = list_pop_front(&pool->global_queue);
            }

        }
        //If there are tasks in the local queue.
        else {
            future_to_run_elem = list_pop_front(&current_worker->local_queue);
        }

        if (future_to_run_elem == NULL) {
            printf("Error: No task found. is_pool_inactive is not working correctly.");
            break;
        }

        struct future * future_to_run = list_entry(future_to_run_elem, struct future, elem);


        future_to_run->status = 1; //running the task

        //Unlocking and Locking to allow the task to execute.

        pthread_mutex_unlock(&pool->lock);

        future_to_run->result = future_to_run->task(pool, future_to_run->data);

        pthread_mutex_lock(&pool->lock);

        future_to_run->status = 2; //completed the task.


        //sending the signal that the task is done.
        pthread_cond_signal(&future_to_run->task_is_done);




        //Unlocking.
        pthread_mutex_unlock(&pool->lock);
    }
    
    //Unlocking.
    pthread_mutex_unlock(&pool->lock);

    pthread_exit(NULL);
    return 0;

}

/**
 * @brief is_pool_inactive is used for checking whether
 * any tasks are left in the pool.
 * 
 * @param pool the pool to look at.
 * @return true when no tasks are left, and shutdown is false.
 * @return false when some tasks are left or shutdown is true.
 */
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
/**
 * @brief steal_task is used to steal a task from another worker.
 * 
 * @param pool - thread_pool which has the workers list to look at
 * @return struct list_elem* - the future elem that has been stolen.
 */
static struct list_elem * steal_task(struct thread_pool * pool) {

    for (struct list_elem * current_worker_elem = list_begin(&pool->workers);
         current_worker_elem != list_end(&pool->workers);
         current_worker_elem = list_next(current_worker_elem)) {

        struct worker * c_worker = list_entry(current_worker_elem, struct worker, elem);
        if (!list_empty(&c_worker->local_queue)) return list_pop_back(&c_worker->local_queue);
    }
    return NULL;
}
#include "thread_pool.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

struct thread_task {
    thread_task_f function;
    void *arg;
    void *result;
    bool is_finished;
    bool is_running;
    bool is_pushed;
    bool is_detached;
    bool is_freed;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct thread_task *next;
    struct thread_pool *pool;
};

struct thread_pool {
    int max_threads;
    int thread_count;
    int idle_threads;
    bool shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct thread_task *head;
    struct thread_task *tail;
    int task_count;
    pthread_t *threads;
};

static void *worker_thread(void *arg);



int
thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
    if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS)
        return TPOOL_ERR_INVALID_ARGUMENT;

    struct thread_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return TPOOL_ERR_INVALID_ARGUMENT; 

    p->max_threads = max_thread_count;
    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->cond, NULL);

    p->threads = calloc(max_thread_count, sizeof(pthread_t));
    if (!p->threads) {
        pthread_mutex_destroy(&p->mutex);
        pthread_cond_destroy(&p->cond);
        free(p);
        return TPOOL_ERR_INVALID_ARGUMENT; 
    }

    *pool = p;
    return 0;
}

int
thread_pool_thread_count(const struct thread_pool *pool)
{
    pthread_mutex_lock((pthread_mutex_t *)&pool->mutex);
    int c = pool->thread_count;
    pthread_mutex_unlock((pthread_mutex_t *)&pool->mutex);
    return c;
}

int
thread_pool_delete(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    if (pool->task_count != 0) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_HAS_TASKS;
    }
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL);

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);
    return 0;
}

static int
maybe_spawn_worker(struct thread_pool *pool)
{
    if (pool->thread_count < pool->max_threads) {
        pthread_t tid;
        int idx = pool->thread_count;
        if (pthread_create(&tid, NULL, worker_thread, pool) != 0)
            return -1;
        pool->threads[idx] = tid;
        pool->thread_count++;
    }
    return 0;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
    pthread_mutex_lock(&pool->mutex);
    if (pool->task_count >= TPOOL_MAX_TASKS) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_TOO_MANY_TASKS;
    }

    pthread_mutex_lock(&task->mutex);
    task->is_finished = false;
    task->is_running  = false;
    task->result      = NULL;
    task->is_pushed   = true;
    task->is_detached = false;
    task->is_freed    = false;
    task->pool        = pool;
    pthread_mutex_unlock(&task->mutex);

    pool->task_count++;
    task->next = NULL;
    if (!pool->head) {
        pool->head = task;
        pool->tail = task;
    } else {
        pool->tail->next = task;
        pool->tail = task;
    }

    if (pool->idle_threads == 0 && pool->thread_count < pool->max_threads)
        maybe_spawn_worker(pool);

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}



static void *
worker_thread(void *arg)
{
    struct thread_pool *pool = (struct thread_pool *)arg;

    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->shutdown && pool->head == NULL) {
            pool->idle_threads++;
            pthread_cond_wait(&pool->cond, &pool->mutex);
            pool->idle_threads--;
            if (pool->shutdown) {
                pthread_mutex_unlock(&pool->mutex);
                return NULL;
            }
        }
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        struct thread_task *task = pool->head;
        if (task) {
            pool->head = task->next;
            if (!pool->head)
                pool->tail = NULL;
        }
        pthread_mutex_unlock(&pool->mutex);

        if (!task)
            continue;

        pthread_mutex_lock(&task->mutex);
        task->is_running = true;
        pthread_mutex_unlock(&task->mutex);

        void *r = task->function(task->arg);

        pthread_mutex_lock(&task->mutex);
        task->result      = r;
        task->is_running  = false;
        task->is_finished = true;
        pthread_cond_broadcast(&task->cond);
        bool detached = task->is_detached;
        bool freed    = task->is_freed;
        pthread_mutex_unlock(&task->mutex);

        if (detached && !freed) {
            pthread_mutex_lock(&pool->mutex);
            if (!task->is_freed) {
                pool->task_count--;
                task->is_freed = true;
            }
            pthread_mutex_unlock(&pool->mutex);

            pthread_mutex_destroy(&task->mutex);
            pthread_cond_destroy(&task->cond);
            free(task);
        }
    }
    return NULL;
}



int
thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
    struct thread_task *t = calloc(1, sizeof(*t));
    if (!t)
        return TPOOL_ERR_INVALID_ARGUMENT; 
    t->function = function;
    t->arg      = arg;
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->cond, NULL);
    *task = t;
    return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
    pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
    bool f = task->is_finished;
    pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
    return f;
}

bool
thread_task_is_running(const struct thread_task *task)
{
    pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
    bool r = task->is_running;
    pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
    return r;
}

int
thread_task_join(struct thread_task *task, void **result)
{
    pthread_mutex_lock(&task->mutex);
    if (!task->is_pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    while (!task->is_finished)
        pthread_cond_wait(&task->cond, &task->mutex);
    if (result)
        *result = task->result;
    bool detached = task->is_detached;
    bool freed    = task->is_freed;
    struct thread_pool *p = task->pool;
    pthread_mutex_unlock(&task->mutex);

    if (!detached && !freed) {
        pthread_mutex_lock(&p->mutex);
        if (!task->is_freed) {
            p->task_count--;
            task->is_freed = true;
        }
        pthread_mutex_unlock(&p->mutex);
    }

    pthread_mutex_lock(&task->mutex);
    task->is_pushed = false;
    pthread_mutex_unlock(&task->mutex);
    return 0;
}


#if NEED_TIMED_JOIN
static void
timespec_add(struct timespec *ts, double timeout_s)
{
    if (timeout_s < 0)
        timeout_s = 0;
    clock_gettime(CLOCK_REALTIME, ts);
    time_t sec = (time_t)timeout_s;
    long nsec  = (long)((timeout_s - sec) * 1e9);
    ts->tv_sec  += sec;
    ts->tv_nsec += nsec;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
    pthread_mutex_lock(&task->mutex);
    if (!task->is_pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    if (task->is_finished) {
        if (result)
            *result = task->result;
        bool detached = task->is_detached;
        bool freed    = task->is_freed;
        struct thread_pool *p = task->pool;
        pthread_mutex_unlock(&task->mutex);

        if (!detached && !freed) {
            pthread_mutex_lock(&p->mutex);
            if (!task->is_freed) {
                p->task_count--;
                task->is_freed = true;
            }
            pthread_mutex_unlock(&p->mutex);
        }
        pthread_mutex_lock(&task->mutex);
        task->is_pushed = false;
        pthread_mutex_unlock(&task->mutex);
        return 0;
    }

    if (timeout <= 0) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TIMEOUT;
    }

    struct timespec ts;
    timespec_add(&ts, timeout);
    while (!task->is_finished) {
        int rc = pthread_cond_timedwait(&task->cond, &task->mutex, &ts);
        if (rc == ETIMEDOUT && !task->is_finished) {
            pthread_mutex_unlock(&task->mutex);
            return TPOOL_ERR_TIMEOUT;
        }
        if (task->is_finished)
            break;
    }
    if (result)
        *result = task->result;
    bool detached = task->is_detached;
    bool freed    = task->is_freed;
    struct thread_pool *p = task->pool;
    pthread_mutex_unlock(&task->mutex);

    if (!detached && !freed) {
        pthread_mutex_lock(&p->mutex);
        if (!task->is_freed) {
            p->task_count--;
            task->is_freed = true;
        }
        pthread_mutex_unlock(&p->mutex);
    }

    pthread_mutex_lock(&task->mutex);
    task->is_pushed = false;
    pthread_mutex_unlock(&task->mutex);
    return 0;
}
#endif

int
thread_task_delete(struct thread_task *task)
{
    pthread_mutex_lock(&task->mutex);
    bool in_pool    = task->is_pushed;
    bool is_freed   = task->is_freed;
    bool is_detach  = task->is_detached;
    pthread_mutex_unlock(&task->mutex);

    if (!is_freed && (in_pool || is_detach))
        return TPOOL_ERR_TASK_IN_POOL;

    if (!is_freed) {
        pthread_mutex_destroy(&task->mutex);
        pthread_cond_destroy(&task->cond);
        free(task);
    }
    return 0;
}

#if NEED_DETACH
int
thread_task_detach(struct thread_task *task)
{
    pthread_mutex_lock(&task->mutex);
    if (!task->is_pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    if (task->is_detached) {
        pthread_mutex_unlock(&task->mutex);
        return 0;
    }
    task->is_detached = true;
    bool finished = task->is_finished;
    bool freed    = task->is_freed;
    pthread_mutex_unlock(&task->mutex);

    if (finished && !freed) {
        struct thread_pool *p = task->pool;
        pthread_mutex_lock(&p->mutex);
        if (!task->is_freed) {
            p->task_count--;
            task->is_freed = true;
        }
        pthread_mutex_unlock(&p->mutex);

        pthread_mutex_destroy(&task->mutex);
        pthread_cond_destroy(&task->cond);
        free(task);
    }
    return 0;
}
#endif

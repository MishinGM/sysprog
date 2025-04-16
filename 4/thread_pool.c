#include "thread_pool.h"
#include "rlist.h"
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#define TIMESPEC_ADD(ts, s, ns)           \
	do {                              \
		(ts)->tv_sec += (s);      \
		(ts)->tv_nsec += (ns);    \
		if ((ts)->tv_nsec >= 1000000000L) { \
			(ts)->tv_sec += 1; \
			(ts)->tv_nsec -= 1000000000L; \
		}                           \
	} while (0)

enum task_state { TASK_NEW, TASK_QUEUED, TASK_RUNNING, TASK_FINISHED };

struct thread_pool;

struct thread_task {
	thread_task_f function;
	void *arg;
	void *result;
	enum task_state state;
	bool joined;
	bool detached;
	struct thread_pool *pool;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct rlist link;
};

struct thread_pool {
	int max_threads;
	pthread_t *threads;
	int thread_count;
	struct rlist queue;
	int queued;
	int running;
	int tasks_total;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool shutting_down;
};

static void *
worker_f(void *ap)
{
	struct thread_pool *p = ap;
	while (1) {
		pthread_mutex_lock(&p->mutex);
		while (rlist_empty(&p->queue) && !p->shutting_down)
			pthread_cond_wait(&p->cond, &p->mutex);
		if (p->shutting_down && rlist_empty(&p->queue)) {
			pthread_mutex_unlock(&p->mutex);
			break;
		}
		struct thread_task *t =
			rlist_shift_entry(&p->queue, struct thread_task, link);
		p->queued--;
		p->running++;
		t->state = TASK_RUNNING;
		pthread_mutex_unlock(&p->mutex);

		void *res = t->function(t->arg);

		pthread_mutex_lock(&t->mutex);
		t->result = res;
		t->state = TASK_FINISHED;
		pthread_cond_broadcast(&t->cond);
		bool detach = t->detached;
		pthread_mutex_unlock(&t->mutex);

		pthread_mutex_lock(&p->mutex);
		p->running--;
		if (detach)
			p->tasks_total--;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);

		if (detach) {
			pthread_cond_destroy(&t->cond);
			pthread_mutex_destroy(&t->mutex);
			free(t);
		}
	}
	return NULL;
}

static void
task_account_join(struct thread_task *t)
{
	if (t->joined || t->detached || t->pool == NULL)
		return;
	t->joined = true;
	struct thread_pool *p = t->pool;
	pthread_mutex_lock(&p->mutex);
	p->tasks_total--;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mutex);
}

static int
start_worker_if_needed(struct thread_pool *p)
{
	if (p->queued <= p->thread_count || p->thread_count == p->max_threads)
		return 0;
	pthread_t th;
	int rc = pthread_create(&th, NULL, worker_f, p);
	if (rc)
		return rc;
	p->threads[p->thread_count++] = th;
	return 0;
}

int
thread_pool_new(int max_thread_count, struct thread_pool **out)
{
	if (!out || max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS)
		return TPOOL_ERR_INVALID_ARGUMENT;
	struct thread_pool *p = calloc(1, sizeof(*p));
	if (!p)
		return ENOMEM;
	p->threads = calloc(max_thread_count, sizeof(pthread_t));
	if (!p->threads) {
		free(p);
		return ENOMEM;
	}
	p->max_threads = max_thread_count;
	rlist_create(&p->queue);
	pthread_mutex_init(&p->mutex, NULL);
	pthread_cond_init(&p->cond, NULL);
	*out = p;
	return 0;
}

int
thread_pool_thread_count(const struct thread_pool *p)
{
	return p ? p->thread_count : 0;
}

int
thread_pool_delete(struct thread_pool *p)
{
	if (!p)
		return 0;
	pthread_mutex_lock(&p->mutex);
	if (p->tasks_total != 0) {
		pthread_mutex_unlock(&p->mutex);
		return TPOOL_ERR_HAS_TASKS;
	}
	p->shutting_down = true;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mutex);
	for (int i = 0; i < p->thread_count; ++i)
		pthread_join(p->threads[i], NULL);
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->mutex);
	free(p->threads);
	free(p);
	return 0;
}

int
thread_pool_push_task(struct thread_pool *p, struct thread_task *t)
{
	if (!p || !t)
		return TPOOL_ERR_INVALID_ARGUMENT;
	pthread_mutex_lock(&p->mutex);
	if (p->tasks_total >= TPOOL_MAX_TASKS) {
		pthread_mutex_unlock(&p->mutex);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}
	pthread_mutex_lock(&t->mutex);
	bool reusable = (t->state == TASK_NEW) ||
			(t->state == TASK_FINISHED && t->joined);
	if (!reusable) {
		pthread_mutex_unlock(&t->mutex);
		pthread_mutex_unlock(&p->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}
	t->state = TASK_QUEUED;
	t->joined = false;
	t->pool = p;
	rlist_add_tail_entry(&p->queue, t, link);
	p->queued++;
	p->tasks_total++;
	pthread_mutex_unlock(&t->mutex);
	start_worker_if_needed(p);
	pthread_cond_signal(&p->cond);
	pthread_mutex_unlock(&p->mutex);
	return 0;
}

int
thread_task_new(struct thread_task **out, thread_task_f f, void *arg)
{
	if (!out || !f)
		return TPOOL_ERR_INVALID_ARGUMENT;
	struct thread_task *t = calloc(1, sizeof(*t));
	if (!t)
		return ENOMEM;
	t->function = f;
	t->arg = arg;
	t->state = TASK_NEW;
	rlist_create(&t->link);
	pthread_mutex_init(&t->mutex, NULL);
	pthread_cond_init(&t->cond, NULL);
	*out = t;
	return 0;
}

bool
thread_task_is_finished(const struct thread_task *t)
{
	return t && t->state == TASK_FINISHED;
}

bool
thread_task_is_running(const struct thread_task *t)
{
	return t && t->state == TASK_RUNNING;
}

static int
do_join(struct thread_task *t, const struct timespec *abs, void **res)
{
	pthread_mutex_lock(&t->mutex);
	if (t->state == TASK_NEW) {
		pthread_mutex_unlock(&t->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	int rc = 0;
	while (t->state != TASK_FINISHED && rc == 0) {
		rc = abs ? pthread_cond_timedwait(&t->cond, &t->mutex, abs)
			 : pthread_cond_wait(&t->cond, &t->mutex);
	}
	if (rc == ETIMEDOUT) {
		pthread_mutex_unlock(&t->mutex);
		return TPOOL_ERR_TIMEOUT;
	}
	if (res)
		*res = t->result;
	pthread_mutex_unlock(&t->mutex);
	task_account_join(t);
	return 0;
}

int
thread_task_join(struct thread_task *t, void **res)
{
	return do_join(t, NULL, res);
}

#if NEED_TIMED_JOIN
int
thread_task_timed_join(struct thread_task *t, double timeout, void **res)
{
	if (timeout < 0)
		timeout = 0;
	struct timespec abs;
	clock_gettime(CLOCK_REALTIME, &abs);
	if (timeout > 0) {
		time_t sec = (time_t)timeout;
		long ns = (long)((timeout - sec) * 1e9);
		TIMESPEC_ADD(&abs, sec, ns);
	}
	return do_join(t, &abs, res);
}
#endif

int
thread_task_delete(struct thread_task *t)
{
	if (t == NULL)
		return 0;

	pthread_mutex_lock(&t->mutex);


	if (t->state == TASK_QUEUED ||
	    t->state == TASK_RUNNING ||
	    (t->state == TASK_FINISHED && !t->joined && !t->detached)) {
		pthread_mutex_unlock(&t->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	bool need_acc = (!t->joined && !t->detached &&
	                 t->state != TASK_NEW && t->pool);
	struct thread_pool *p = t->pool;

	pthread_mutex_unlock(&t->mutex);

	if (need_acc) {
		pthread_mutex_lock(&p->mutex);
		p->tasks_total--;
		pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);
	}

	pthread_cond_destroy(&t->cond);
	pthread_mutex_destroy(&t->mutex);
	free(t);
	return 0;
}

#if NEED_DETACH
int
thread_task_detach(struct thread_task *t)
{
	pthread_mutex_lock(&t->mutex);
	if (t->state == TASK_NEW) {
		pthread_mutex_unlock(&t->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	if (t->detached) {
		pthread_mutex_unlock(&t->mutex);
		return 0;
	}
	t->detached = true;
	bool finished = (t->state == TASK_FINISHED);
	pthread_mutex_unlock(&t->mutex);
	if (finished) {
		struct thread_pool *p = t->pool;
		if (p) {
			pthread_mutex_lock(&p->mutex);
			p->tasks_total--;
			pthread_cond_broadcast(&p->cond);
			pthread_mutex_unlock(&p->mutex);
		}
		return thread_task_delete(t);
	}
	return 0;
}
#endif

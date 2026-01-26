#include "libcoro.h"

#include "rlist.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <setjmp.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define handle_error() do {														\
	printf("Error %s\n", strerror(errno));										\
	exit(-1);																	\
} while(0)

enum coro_state {
	CORO_STATE_RUNNING,
	CORO_STATE_SUSPENDED,
	CORO_STATE_FINISHED,
};

/** Main coroutine structure, its context. */
struct coro {
	/** Coroutine state. */
	enum coro_state state;
	/** A value, returned by func. */
	void *ret;
	/** Stack, used by the coroutine. */
	uint8_t *stack;
	/** An argument for the function func. */
	void *func_arg;
	/** A function to call as a coroutine. */
	coro_f func;
	/** Last remembered coroutine context. */
	sigjmp_buf ctx;
	/**
	 * Coroutine which is trying to join this one right now.
	 */
	struct coro *joiner;
	/** Links in a coroutine list, used by the scheduler. */
	struct rlist link;
};

struct coro_engine {
	/**
	 * Scheduler is the main coroutine - it represents the
	 * context in which the scheduler itself runs.
	 */
	struct coro sched;
	/** Which coroutine works at this moment. */
	struct coro *this_coro;

	/**
	 * Coroutines to run in this iteration of the loop. The
	 * list gets populated once at the start of the iteration.
	 */
	struct rlist coros_running_now;
	/**
	 * Coroutines to run in the next iteration of the loop.
	 * The list gets populated by wakeups and yields and new
	 * coros.
	 */
	struct rlist coros_running_next;
	/** Joined coroutines to be reused. */
	struct rlist coros_pool;
	/** Total number of coroutines, including the pool. */
	size_t coro_count;
	/**
	 * Buffer, used by the coroutine constructor to escape
	 * from the signal handler back into the constructor to
	 * rollback sigaltstack etc.
	 */
	sigjmp_buf start_point;
};

static void
coro_engine_create(struct coro_engine *engine)
{
	memset(engine, 0, sizeof(*engine));
	rlist_create(&engine->sched.link);
	rlist_create(&engine->coros_running_now);
	rlist_create(&engine->coros_running_next);
	rlist_create(&engine->coros_pool);
}

static void
coro_engine_resume_next(struct coro_engine *engine)
{
	assert(!rlist_empty(&engine->coros_running_now));
	struct coro *to = rlist_shift_entry(&engine->coros_running_now,
		struct coro, link);
	struct coro *from = engine->this_coro;
	assert(from != NULL);

	engine->this_coro = NULL;
	if (sigsetjmp(from->ctx, 0) == 0)
		siglongjmp(to->ctx, 1);
	assert(rlist_empty(&from->link));
	assert(engine->this_coro == NULL);
	engine->this_coro = from;
}

static void
coro_engine_suspend(struct coro_engine *engine)
{
	struct coro *this_coro = engine->this_coro;
	if (this_coro == NULL) {
		printf("Error: deadlock - suspension with no active "
			"coroutines\n");
		exit(-1);
	}
	assert(rlist_empty(&this_coro->link));
	assert(this_coro->state == CORO_STATE_RUNNING);
	this_coro->state = CORO_STATE_SUSPENDED;
	coro_engine_resume_next(engine);
}

static void
coro_engine_yield(struct coro_engine *engine)
{
	struct coro *this_coro = engine->this_coro;
	assert(rlist_empty(&this_coro->link));
	assert(this_coro->state == CORO_STATE_RUNNING);
	rlist_add_tail_entry(&engine->coros_running_next, this_coro, link);
	coro_engine_resume_next(engine);
}

static void
coro_engine_wakeup(struct coro_engine *engine, struct coro *coro)
{
	if (coro->state == CORO_STATE_RUNNING)
		return;
	if (coro->state == CORO_STATE_FINISHED)
		return;
	assert(coro->state == CORO_STATE_SUSPENDED);
	assert(rlist_empty(&coro->link));
	coro->state = CORO_STATE_RUNNING;
	rlist_add_tail_entry(&engine->coros_running_next, coro, link);
}

static void
coro_engine_run(struct coro_engine *engine)
{
	while (true) {
		assert(rlist_empty(&engine->coros_running_now));
		rlist_splice_tail(&engine->coros_running_now,
			&engine->coros_running_next);
		if (rlist_empty(&engine->coros_running_now))
			break;

		assert(engine->this_coro == NULL);
		engine->this_coro = &engine->sched;
		assert(rlist_empty(&engine->sched.link));
		/*
		 * Add the scheduler to the tail so the control
		 * comes back in the end of this iteration of the
		 * loop.
		 */
		rlist_add_tail_entry(&engine->coros_running_now,
			&engine->sched, link);
		coro_engine_resume_next(engine);
		assert(rlist_empty(&engine->coros_running_now));
		assert(engine->this_coro == &engine->sched);
		engine->this_coro = NULL;
	}
}

static void
coro_engine_destroy(struct coro_engine *engine)
{
	assert(engine->this_coro == NULL);
	assert(rlist_empty(&engine->coros_running_now));
	assert(rlist_empty(&engine->coros_running_next));
	while (!rlist_empty(&engine->coros_pool)) {
		struct coro *c = rlist_shift_entry(&engine->coros_pool,
			struct coro, link);
		delete[] c->stack;
		delete c;
		assert(engine->coro_count > 0);
		--engine->coro_count;
	}
	assert(engine->coro_count == 0);
	memset(engine, '#', sizeof(*engine));
}

static __thread struct coro_engine *new_coro_engine = NULL;

/**
 * The core part of the coroutines creation - this signal handler
 * runs on a separate stack using sigaltstack. At invocation it
 * remembers its current context and jumps back to the coroutine
 * constructor. Later the coroutine continues from here.
 */
static void
coro_body(int signum)
{
	(void)signum;
	struct coro_engine *my_engine = new_coro_engine;
	new_coro_engine = NULL;

	struct coro *c = my_engine->this_coro;
	my_engine->this_coro = NULL;
	/*
	 * On invocation jump back to the constructor right after
	 * remembering the context.
	 */
	if (sigsetjmp(c->ctx, 0) == 0)
		siglongjmp(my_engine->start_point, 1);
	/*
	 * If the execution is here, then the coroutine should
	 * finally start work.
	 */
	my_engine->this_coro = c;
	while (true) {
		c->ret = c->func(c->func_arg);
		c->func = NULL;
		assert(c->state == CORO_STATE_RUNNING);
		c->state = CORO_STATE_FINISHED;
		if (c->joiner != NULL)
			coro_engine_wakeup(my_engine, c->joiner);
		coro_engine_resume_next(my_engine);
		/*
		 * Here it is restarted already, must have its
		 * state restored.
		 */
		assert(c->state == CORO_STATE_RUNNING);
		assert(c->func != NULL);
	}
}

static struct coro *
coro_engine_spawn_new(struct coro_engine *engine, coro_f func, void *func_arg)
{
	struct coro *c = new coro();
	c->state = CORO_STATE_RUNNING;
	c->ret = NULL;
	int stack_size = 1024 * 1024;
	if (stack_size < SIGSTKSZ)
		stack_size = SIGSTKSZ;
	c->stack = new uint8_t[stack_size];
	c->func = func;
	c->func_arg = func_arg;
	c->joiner = NULL;
	rlist_create(&c->link);
	/*
	 * SIGUSR2 is used. First of all, block new signals to be
	 * able to set a new handler.
	 */
	sigset_t news, olds, suss;
	sigemptyset(&news);
	sigaddset(&news, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &news, &olds) != 0)
		handle_error();
	/*
	 * New handler should jump onto a new stack and remember
	 * that position. Afterwards the stack is disabled and
	 * becomes dedicated to that single coroutine.
	 */
	struct sigaction newsa, oldsa;
	newsa.sa_handler = coro_body;
	newsa.sa_flags = SA_ONSTACK;
	sigemptyset(&newsa.sa_mask);
	if (sigaction(SIGUSR2, &newsa, &oldsa) != 0)
		handle_error();
	/* Create that new stack. */
	stack_t oldst, newst;
	newst.ss_sp = c->stack;
	newst.ss_size = stack_size;
	newst.ss_flags = 0;
	if (sigaltstack(&newst, &oldst) != 0)
		handle_error();

	/* Jump onto the stack and remember its position. */
	assert(new_coro_engine == NULL);
	new_coro_engine = engine;
	struct coro *old_this = engine->this_coro;
	engine->this_coro = c;
	sigemptyset(&suss);
	if (sigsetjmp(engine->start_point, 1) == 0) {
		raise(SIGUSR2);
		while (engine->this_coro != NULL)
			sigsuspend(&suss);
	}
	assert(new_coro_engine == NULL);
	engine->this_coro = old_this;

	/*
	 * Return the old stack, unblock SIGUSR2. In other words,
	 * rollback all global changes. The newly created stack
	 * now is remembered only by the new coroutine, and can be
	 * used by it only.
	 */
	if (sigaltstack(NULL, &newst) != 0)
		handle_error();
	newst.ss_flags = SS_DISABLE;
	if (sigaltstack(&newst, NULL) != 0)
		handle_error();
	if ((oldst.ss_flags & SS_DISABLE) == 0 &&
	    sigaltstack(&oldst, NULL) != 0)
		handle_error();
	if (sigaction(SIGUSR2, &oldsa, NULL) != 0)
		handle_error();
	if (sigprocmask(SIG_SETMASK, &olds, NULL) != 0)
		handle_error();

	/* Now scheduler can work with that coroutine. */
	++engine->coro_count;
	assert(rlist_empty(&c->link));
	rlist_add_tail_entry(&engine->coros_running_next, c, link);
	return c;
}

static struct coro *
coro_engine_spawn(struct coro_engine *engine, coro_f func, void *func_arg)
{
	if (rlist_empty(&engine->coros_pool))
		return coro_engine_spawn_new(engine, func, func_arg);

	struct coro *c = rlist_shift_entry(&engine->coros_pool,
		struct coro, link);
	c->func = func;
	c->func_arg = func_arg;
	c->state = CORO_STATE_RUNNING;
	assert(rlist_empty(&c->link));
	rlist_add_tail_entry(&engine->coros_running_next, c, link);
	return c;
}

static void *
coro_engine_join(struct coro_engine *engine, struct coro *coro)
{
	assert(coro->joiner == NULL);
	coro->joiner = engine->this_coro;
	while (coro->state == CORO_STATE_RUNNING ||
		coro->state == CORO_STATE_SUSPENDED)
		coro_engine_suspend(engine);
	assert(coro->state == CORO_STATE_FINISHED);
	assert(coro->joiner == engine->this_coro);
	coro->joiner = NULL;
	void *ret = coro->ret;
	coro->ret = NULL;
	assert(rlist_empty(&coro->link));
	rlist_add_entry(&engine->coros_pool, coro, link);
	return ret;
}

//////////////////////////////////////////////////////////////////

static struct coro_engine glob_engine;

void
coro_sched_init(void)
{
	coro_engine_create(&glob_engine);
}

void
coro_sched_run(void)
{
	coro_engine_run(&glob_engine);
}

void
coro_sched_destroy(void)
{
	coro_engine_destroy(&glob_engine);
}

struct coro *
coro_this(void)
{
	return glob_engine.this_coro;
}

struct coro *
coro_new(coro_f func, void *func_arg)
{
	return coro_engine_spawn(&glob_engine, func, func_arg);
}

void *
coro_join(struct coro *coro)
{
	return coro_engine_join(&glob_engine, coro);
}

void
coro_suspend(void)
{
	coro_engine_suspend(&glob_engine);
}

void
coro_yield(void)
{
	coro_engine_yield(&glob_engine);
}

void
coro_wakeup(struct coro *coro)
{
	coro_engine_wakeup(&glob_engine, coro);
}

#include "stdafx.h"
#define __USE_GNU
#include <dlfcn.h>

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#include "fiber/lib_fiber.h"
#include "event_epoll.h"  /* just for hook_epoll */
#include "fiber.h"

#define	MAX_CACHE	1000

typedef int  *(*errno_fn)(void);
typedef int   (*fcntl_fn)(int, int, ...);

#ifdef __DEBUG_MEM
typedef void *(*malloc_fn)(size_t);
typedef void *(*calloc_fn)(size_t, size_t);
typedef void *(*realloc_fn)(void*, size_t);
#endif

static errno_fn __sys_errno     = NULL;
static fcntl_fn __sys_fcntl     = NULL;

#ifdef __DEBUG_MEM
static malloc_fn __sys_malloc   = NULL;
//static calloc_fn __sys_calloc   = NULL;
static realloc_fn __sys_realloc = NULL;
#endif

typedef struct {
	ACL_RING       ready;		/* ready fiber queue */
	ACL_RING       dead;		/* dead fiber queue */
	ACL_FIBER    **fibers;
	size_t         size;
	size_t         slot;
	int            exitcode;
	ACL_FIBER     *running;
	ACL_FIBER      original;
	int            errnum;
	size_t         idgen;
	int            count;
	int            switched;
	int            nlocal;
} FIBER_TLS;

static FIBER_TLS *__main_fiber = NULL;
static __thread FIBER_TLS *__thread_fiber = NULL;
__thread int acl_var_hook_sys_api = 0;

static acl_pthread_key_t __fiber_key;

/* forward declare */
static ACL_FIBER *fiber_alloc(void (*fn)(ACL_FIBER *, void *),
	void *arg, size_t size);
static void fiber_init(void);

void acl_fiber_hook_api(int onoff)
{
	acl_var_hook_sys_api = onoff;
}

static void thread_free(void *ctx)
{
	FIBER_TLS *tf = (FIBER_TLS *) ctx;

	if (__thread_fiber == NULL)
		return;

	if (tf->fibers)
		acl_myfree(tf->fibers);
	if (tf->original.context)
		acl_myfree(tf->original.context);
	acl_myfree(tf);

	if (__main_fiber == __thread_fiber)
		__main_fiber = NULL;
	__thread_fiber = NULL;
}

static void fiber_schedule_main_free(void)
{
	if (__main_fiber) {
		thread_free(__main_fiber);
		if (__thread_fiber == __main_fiber)
			__thread_fiber = NULL;
		__main_fiber = NULL;
	}
}

static void thread_init(void)
{
	acl_assert(acl_pthread_key_create(&__fiber_key, thread_free) == 0);
}

static acl_pthread_once_t __once_control = ACL_PTHREAD_ONCE_INIT;

static void fiber_check(void)
{
	if (__thread_fiber != NULL)
		return;

	acl_assert(acl_pthread_once(&__once_control, thread_init) == 0);

	__thread_fiber = (FIBER_TLS *) acl_mycalloc(1, sizeof(FIBER_TLS));
#ifdef	USE_JMP
	/* set context NULL when using setjmp that setcontext will not be
	 * called in fiber_swap.
	 */
	__thread_fiber->original.context = NULL;
#else
	__thread_fiber->original.context = (ucontext_t *)
		acl_mycalloc(1, sizeof(ucontext_t));
#endif
	__thread_fiber->fibers = NULL;
	__thread_fiber->size   = 0;
	__thread_fiber->slot   = 0;
	__thread_fiber->idgen  = 0;
	__thread_fiber->count  = 0;
	__thread_fiber->nlocal = 0;

	acl_ring_init(&__thread_fiber->ready);
	acl_ring_init(&__thread_fiber->dead);

	if ((unsigned long) acl_pthread_self() == acl_main_thread_self()) {
		__main_fiber = __thread_fiber;
		atexit(fiber_schedule_main_free);
	} else if (acl_pthread_setspecific(__fiber_key, __thread_fiber) != 0)
		acl_msg_fatal("acl_pthread_setspecific error!");
}

/* see /usr/include/bits/errno.h for __errno_location */
int *__errno_location(void)
{
	if (!acl_var_hook_sys_api)
		return __sys_errno();

	if (__thread_fiber == NULL)
		fiber_check();

	if (__thread_fiber->running)
		return &__thread_fiber->running->errnum;
	else
		return &__thread_fiber->original.errnum;
}

int fcntl(int fd, int cmd, ...)
{
	long arg;
	struct flock *lock;
	va_list ap;
	int ret;

	va_start(ap, cmd);

	switch (cmd) {
	case F_GETFD:
	case F_GETFL:
		ret = __sys_fcntl(fd, cmd);
		break;
	case F_SETFD:
	case F_SETFL:
		arg = va_arg(ap, long);
		ret = __sys_fcntl(fd, cmd, arg);
		break;
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
		lock = va_arg(ap, struct flock*);
		ret = __sys_fcntl(fd, cmd, lock);
		break;
	default:
		ret = -1;
		acl_msg_error("%s(%d), %s: unknown cmd: %d, fd: %d",
			__FILE__, __LINE__, __FUNCTION__, cmd, fd);
		break;
	}

	va_end(ap);

	if (ret < 0)
		fiber_save_errno();

	return ret;
}

#ifdef __DEBUG_MEM
void *malloc(size_t size)
{
	if (__sys_malloc == NULL)
		fiber_init();
	//assert(size < 64000000);
	return __sys_malloc(size);
}

/*
void *calloc(size_t nmemb, size_t size)
{
	if (__sys_calloc == NULL)
		fiber_init();
	assert(size < 64000000);
	return __sys_calloc(nmemb, size);
}
*/

void *realloc(void *ptr, size_t size)
{
	if (__sys_realloc == NULL)
		fiber_init();
	//assert(size < 64000000);
	return __sys_realloc(ptr, size);
}
#endif

void acl_fiber_set_errno(ACL_FIBER *fiber, int errnum)
{
	fiber->errnum = errnum;
}

int acl_fiber_errno(ACL_FIBER *fiber)
{
	return fiber->errnum;
}

void acl_fiber_keep_errno(ACL_FIBER *fiber, int yesno)
{
	if (yesno)
		fiber->flag |= FIBER_F_SAVE_ERRNO;
	else
		fiber->flag &= ~FIBER_F_SAVE_ERRNO;
}

void fiber_save_errno(void)
{
	ACL_FIBER *curr;

	if (__thread_fiber == NULL)
		fiber_check();

	if ((curr = __thread_fiber->running) == NULL)
		curr = &__thread_fiber->original;

	if (curr->flag & FIBER_F_SAVE_ERRNO) {
		//curr->flag &= ~FIBER_F_SAVE_ERRNO;
		return;
	}

	if (__sys_errno != NULL)
		acl_fiber_set_errno(curr, *__sys_errno());
	else
		acl_fiber_set_errno(curr, errno);
}

#if defined(__x86_64__)

# if defined(__AVX__)
#  define CLOBBER \
        , "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",\
        "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15"
# else
#  define CLOBBER
# endif

# define SETJMP(ctx) ({\
    int ret;\
    asm("lea     LJMPRET%=(%%rip), %%rcx\n\t"\
        "xor     %%rax, %%rax\n\t"\
        "mov     %%rbx, (%%rdx)\n\t"\
        "mov     %%rbp, 8(%%rdx)\n\t"\
        "mov     %%r12, 16(%%rdx)\n\t"\
        "mov     %%rsp, 24(%%rdx)\n\t"\
        "mov     %%r13, 32(%%rdx)\n\t"\
        "mov     %%r14, 40(%%rdx)\n\t"\
        "mov     %%r15, 48(%%rdx)\n\t"\
        "mov     %%rcx, 56(%%rdx)\n\t"\
        "mov     %%rdi, 64(%%rdx)\n\t"\
        "mov     %%rsi, 72(%%rdx)\n\t"\
        "LJMPRET%=:\n\t"\
        : "=a" (ret)\
        : "d" (ctx)\
        : "memory", "rcx", "r8", "r9", "r10", "r11",\
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",\
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"\
          CLOBBER\
          );\
    ret;\
})

# define LONGJMP(ctx) \
    asm("movq   (%%rax), %%rbx\n\t"\
	"movq   8(%%rax), %%rbp\n\t"\
	"movq   16(%%rax), %%r12\n\t"\
	"movq   24(%%rax), %%rdx\n\t"\
	"movq   32(%%rax), %%r13\n\t"\
	"movq   40(%%rax), %%r14\n\t"\
	"mov    %%rdx, %%rsp\n\t"\
	"movq   48(%%rax), %%r15\n\t"\
	"movq   56(%%rax), %%rdx\n\t"\
	"movq   64(%%rax), %%rdi\n\t"\
	"movq   72(%%rax), %%rsi\n\t"\
	"jmp    *%%rdx\n\t"\
        : : "a" (ctx) : "rdx" \
    )

#elif defined(__i386__)

# define SETJMP(ctx) ({\
    int ret;\
    asm("movl   $LJMPRET%=, %%eax\n\t"\
	"movl   %%eax, (%%edx)\n\t"\
	"movl   %%ebx, 4(%%edx)\n\t"\
	"movl   %%esi, 8(%%edx)\n\t"\
	"movl   %%edi, 12(%%edx)\n\t"\
	"movl   %%ebp, 16(%%edx)\n\t"\
	"movl   %%esp, 20(%%edx)\n\t"\
	"xorl   %%eax, %%eax\n\t"\
	"LJMPRET%=:\n\t"\
	: "=a" (ret) : "d" (ctx) : "memory");\
	ret;\
    })

# define LONGJMP(ctx) \
    asm("movl   (%%eax), %%edx\n\t"\
	"movl   4(%%eax), %%ebx\n\t"\
	"movl   8(%%eax), %%esi\n\t"\
	"movl   12(%%eax), %%edi\n\t"\
	"movl   16(%%eax), %%ebp\n\t"\
	"movl   20(%%eax), %%esp\n\t"\
	"jmp    *%%edx\n\t"\
	: : "a" (ctx) : "edx" \
   )

#else

# define SETJMP(ctx) \
    sigsetjmp(ctx, 0)
# define LONGJMP(ctx) \
    siglongjmp(ctx, 1)
#endif

static void fiber_kick(int max)
{
	ACL_RING *head;
	ACL_FIBER *fiber;

	while (max > 0) {
		head = acl_ring_pop_head(&__thread_fiber->dead);
		if (head == NULL)
			break;
		fiber = ACL_RING_TO_APPL(head, ACL_FIBER,me);
		fiber_free(fiber);
		max--;
	}
}

static void fiber_swap(ACL_FIBER *from, ACL_FIBER *to)
{
	if (from->status == FIBER_STATUS_EXITING) {
		size_t slot = from->slot;
		int n = acl_ring_size(&__thread_fiber->dead);

		/* if the cached dead fibers reached the limit,
		 * some will be freed
		 */
		if (n > MAX_CACHE) {
			n -= MAX_CACHE;
			fiber_kick(n);
		}

		if (!from->sys)
			__thread_fiber->count--;

		__thread_fiber->fibers[slot] =
			__thread_fiber->fibers[--__thread_fiber->slot];
		__thread_fiber->fibers[slot]->slot = slot;

		acl_ring_prepend(&__thread_fiber->dead, &from->me);
	}

#ifdef	USE_JMP
	/* use setcontext() for the initial jump, as it allows us to set up
	 * a stack, but continue with longjmp() as it's much faster.
	 */
	if (SETJMP(from->env) == 0) {
		/* context just be used once for set up a stack, which will
		 * be freed in fiber_start. The context in __thread_fiber
		 * was set NULL.
		 */
		if (to->context != NULL)
			setcontext(to->context);
		else
			LONGJMP(to->env);
	}
#else
	if (swapcontext(from->context, to->context) < 0)
		acl_msg_fatal("%s(%d), %s: swapcontext error %s",
			__FILE__, __LINE__, __FUNCTION__, acl_last_serror());
#endif
}

ACL_FIBER *acl_fiber_running(void)
{
	fiber_check();
	return __thread_fiber->running;
}

void acl_fiber_kill(ACL_FIBER *fiber)
{
	ACL_FIBER *curr = __thread_fiber->running;

	if (fiber == NULL) {
		acl_msg_error("%s(%d), %s: fiber NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return;
	}

	if (curr == NULL) {
		acl_msg_error("%s(%d), %s: current fiber NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return;
	}

	fiber->flag |= FIBER_F_KILLED;

	if (fiber == curr) // just return if kill myself
		return;

	acl_ring_detach(&curr->me);
	acl_ring_detach(&fiber->me);
	acl_fiber_ready(fiber);
	acl_fiber_yield();
}

int acl_fiber_killed(ACL_FIBER *fiber)
{
	return fiber->flag & FIBER_F_KILLED;
}

void fiber_exit(int exit_code)
{
	fiber_check();

	__thread_fiber->exitcode = exit_code;
	__thread_fiber->running->status = FIBER_STATUS_EXITING;

	acl_fiber_switch();
}

void acl_fiber_ready(ACL_FIBER *fiber)
{
	if (fiber->status != FIBER_STATUS_EXITING) {
		fiber->status = FIBER_STATUS_READY;
		acl_ring_prepend(&__thread_fiber->ready, &fiber->me);
	}
}

int acl_fiber_yield(void)
{
	int  n;

	if (acl_ring_size(&__thread_fiber->ready) == 0)
		return 0;

	n = __thread_fiber->switched;
	acl_fiber_ready(__thread_fiber->running);
	acl_fiber_switch();

	return __thread_fiber->switched - n - 1;
}

union cc_arg
{
	void *p;
	int   i[2];
};

static void fiber_start(unsigned int x, unsigned int y)
{
	union  cc_arg arg;
	ACL_FIBER *fiber;
	int i;

	arg.i[0] = x;
	arg.i[1] = y;
	
	fiber = (ACL_FIBER *) arg.p;

#ifdef	USE_JMP
	/* when using setjmp/longjmp, the context just be used only once */
	if (fiber->context != NULL) {
		acl_myfree(fiber->context);
		fiber->context = NULL;
	}
#endif

	fiber->fn(fiber, fiber->arg);

	for (i = 0; i < fiber->nlocal; i++) {
		if (fiber->locals[i] == NULL)
			continue;
		if (fiber->locals[i]->free_fn)
			fiber->locals[i]->free_fn(fiber->locals[i]->ctx);
		acl_myfree(fiber->locals[i]);
	}

	if (fiber->locals) {
		acl_myfree(fiber->locals);
		fiber->locals = NULL;
		fiber->nlocal = 0;
	}

	fiber_exit(0);
}

int acl_fiber_ndead(void)
{
	if (__thread_fiber == NULL)
		return 0;
	return acl_ring_size(&__thread_fiber->dead);
}

void fiber_free(ACL_FIBER *fiber)
{
#ifdef USE_VALGRIND
	VALGRIND_STACK_DEREGISTER(fiber->vid);
#endif
	if (fiber->context)
		acl_myfree(fiber->context);
	acl_myfree(fiber->buff);
	acl_myfree(fiber);
}

static ACL_FIBER *fiber_alloc(void (*fn)(ACL_FIBER *, void *),
	void *arg, size_t size)
{
	ACL_FIBER *fiber;
	sigset_t zero;
	union cc_arg carg;
	ACL_RING *head;

	fiber_check();

#define	APPL	ACL_RING_TO_APPL

	/* try to reuse the fiber memory in dead queue */
	head = acl_ring_pop_head(&__thread_fiber->dead);
	if (head == NULL) {
		fiber = (ACL_FIBER *) acl_mycalloc(1, sizeof(ACL_FIBER));
		fiber->buff = (char *) acl_mymalloc(size);
	} else if ((fiber = APPL(head, ACL_FIBER, me))->size < size)
		fiber->buff = (char *) acl_myrealloc(fiber->buff, size);
	else
		size = fiber->size;

	fiber->errnum = 0;
	fiber->fn     = fn;
	fiber->arg    = arg;
	fiber->size   = size;
	fiber->id     = ++__thread_fiber->idgen;
	fiber->flag   = 0;
	fiber->status = FIBER_STATUS_READY;

	carg.p = fiber;

	if (fiber->context == NULL)
		fiber->context = (ucontext_t *) acl_mymalloc(sizeof(ucontext_t));
	sigemptyset(&zero);
	sigprocmask(SIG_BLOCK, &zero, &fiber->context->uc_sigmask);

	if (getcontext(fiber->context) < 0)
		acl_msg_fatal("%s(%d), %s: getcontext error: %s",
			__FILE__, __LINE__, __FUNCTION__, acl_last_serror());

	fiber->context->uc_stack.ss_sp   = fiber->buff + 8;
	fiber->context->uc_stack.ss_size = fiber->size - 64;

#ifdef	USE_JMP
	fiber->context->uc_link = NULL;
#else
	fiber->context->uc_link = __thread_fiber->original.context;
#endif

#ifdef USE_VALGRIND
	/* avoding the valgrind's warning */
	fiber->vid = VALGRIND_STACK_REGISTER(fiber->context->uc_stack.ss_sp,
			fiber->context->uc_stack.ss_sp
			+ fiber->context->uc_stack.ss_size);
#endif
	makecontext(fiber->context, (void(*)(void)) fiber_start,
		2, carg.i[0], carg.i[1]);

	return fiber;
}

ACL_FIBER *acl_fiber_create(void (*fn)(ACL_FIBER *, void *),
	void *arg, size_t size)
{
	ACL_FIBER *fiber = fiber_alloc(fn, arg, size);

	__thread_fiber->count++;

	if (__thread_fiber->slot >= __thread_fiber->size) {
		__thread_fiber->size += 128;
		__thread_fiber->fibers = (ACL_FIBER **) acl_myrealloc(
			__thread_fiber->fibers, 
			__thread_fiber->size * sizeof(ACL_FIBER *));
	}

	fiber->slot = __thread_fiber->slot;
	__thread_fiber->fibers[__thread_fiber->slot++] = fiber;

	acl_fiber_ready(fiber);

	return fiber;
}

int acl_fiber_id(const ACL_FIBER *fiber)
{
	return fiber ? fiber->id : 0;
}

int acl_fiber_self(void)
{
	ACL_FIBER *curr = acl_fiber_running();
	return acl_fiber_id(curr);
}

int acl_fiber_status(const ACL_FIBER *fiber)
{
	return fiber->status;
}

static void fiber_init(void) __attribute__ ((constructor));

static void fiber_init(void)
{
	static int __called = 0;

	if (__called != 0)
		return;

	__called++;

#ifdef __DEBUG_MEM
	//__sys_calloc  = (calloc_fn) dlsym(RTLD_NEXT, "calloc");
	__sys_malloc  = (malloc_fn) dlsym(RTLD_NEXT, "malloc");
	__sys_realloc = (realloc_fn) dlsym(RTLD_NEXT, "realloc");
#endif

	__sys_errno   = (errno_fn) dlsym(RTLD_NEXT, "__errno_location");
	__sys_fcntl   = (fcntl_fn) dlsym(RTLD_NEXT, "fcntl");

	hook_io();
	hook_net();
	hook_epoll();
}

void acl_fiber_schedule(void)
{
	ACL_FIBER *fiber;
	ACL_RING *head;

	acl_fiber_hook_api(1);

	for (;;) {
		head = acl_ring_pop_head(&__thread_fiber->ready);
		if (head == NULL) {
			acl_msg_info("------- NO ACL_FIBER NOW --------");
			break;
		}

		fiber = ACL_RING_TO_APPL(head, ACL_FIBER, me);
		fiber->status = FIBER_STATUS_READY;

		__thread_fiber->running = fiber;
		__thread_fiber->switched++;

		fiber_swap(&__thread_fiber->original, fiber);
		__thread_fiber->running = NULL;
	}

	/* release dead fiber */
	while ((head = acl_ring_pop_head(&__thread_fiber->dead)) != NULL) {
		fiber = ACL_RING_TO_APPL(head, ACL_FIBER, me);
		fiber_free(fiber);
	}

	acl_fiber_hook_api(0);
}

void fiber_system(void)
{
	if (!__thread_fiber->running->sys) {
		__thread_fiber->running->sys = 1;
		__thread_fiber->count--;
	}
}

void fiber_count_inc(void)
{
	__thread_fiber->count++;
}

void fiber_count_dec(void)
{
	__thread_fiber->count--;
}

void acl_fiber_switch(void)
{
	ACL_FIBER *fiber, *current = __thread_fiber->running;
	ACL_RING *head;

#ifdef _DEBUG
	acl_assert(current);
#endif

	head = acl_ring_pop_head(&__thread_fiber->ready);

	if (head == NULL) {
		fiber_swap(current, &__thread_fiber->original);
		return;
	}

	fiber = ACL_RING_TO_APPL(head, ACL_FIBER, me);
	//fiber->status = FIBER_STATUS_READY;

	__thread_fiber->running = fiber;
	__thread_fiber->switched++;
	fiber_swap(current, __thread_fiber->running);
}

int acl_fiber_set_specific(int *key, void *ctx, void (*free_fn)(void *))
{
	FIBER_LOCAL *local;
	ACL_FIBER *curr;

	if (key == NULL) {
		acl_msg_error("%s(%d), %s: key NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return -1;
	}

	if (__thread_fiber == NULL) {
		acl_msg_error("%s(%d), %s: __thread_fiber: NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return -1;
	} else if (__thread_fiber->running == NULL) {
		acl_msg_error("%s(%d), %s: running: NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return -1;
	} else
		curr = __thread_fiber->running;

	if (*key <= 0)
		*key = ++__thread_fiber->nlocal;
	else if (*key > __thread_fiber->nlocal) {
		acl_msg_error("%s(%d), %s: invalid key: %d > nlocal: %d",
			__FILE__, __LINE__, __FUNCTION__,
			*key, __thread_fiber->nlocal);
		return -1;
	}

	if (curr->nlocal < __thread_fiber->nlocal) {
		curr->nlocal = __thread_fiber->nlocal;
		curr->locals = (FIBER_LOCAL **) acl_myrealloc(curr->locals,
			curr->nlocal * sizeof(FIBER_LOCAL*));
	}

	local = (FIBER_LOCAL *) acl_mycalloc(1, sizeof(FIBER_LOCAL));
	local->ctx = ctx;
	local->free_fn = free_fn;
	curr->locals[*key - 1] = local;

	return *key;
}

void *acl_fiber_get_specific(int key)
{
	FIBER_LOCAL *local;
	ACL_FIBER *curr;

	if (key <= 0)
		return NULL;

	if (__thread_fiber == NULL) {
		acl_msg_error("%s(%d), %s: __thread_fiber NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return NULL;
	} else if (__thread_fiber->running == NULL) {
		acl_msg_error("%s(%d), %s: running fiber NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return NULL;
	} else
		curr = __thread_fiber->running;

	if (key > curr->nlocal) {
		acl_msg_error("%s(%d), %s: invalid key: %d > nlocal: %d",
			__FILE__, __LINE__, __FUNCTION__,
			key, curr->nlocal);
		return NULL;
	}

	local = curr->locals[key - 1];
	return local ? local->ctx : NULL;
}

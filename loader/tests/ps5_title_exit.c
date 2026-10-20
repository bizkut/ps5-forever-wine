/* Host regression for production title exit and native TLS teardown. */
#define _GNU_SOURCE 1
#define WINE_PS5_TITLE_HOST_TEST 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "wine/ps5_title_private.h"

/* Only the fields accessed by the extracted destructor are needed here. */
struct ntdll_thread_data
{
    struct wine_ps5_title_thread title_thread;
    struct { unsigned int armed, tid; } hitch;
};

static struct wine_ps5_title_threads title_threads = WINE_PS5_TITLE_THREADS_INIT;
static struct wine_ps5_title_callbacks title_callbacks = WINE_PS5_TITLE_CALLBACKS_INIT;
static struct { void (*exit)(int); } title_runtime;
static sigset_t ps5_title_block_set;
static pthread_key_t thread_key;
static unsigned int stop_calls;
static volatile sig_atomic_t quit_delivered;

struct exit_case
{
    struct ntdll_thread_data data;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned int id;
    int status, wait_callback, initially_blocked, queue_before_exit, queue_in_destructor;
    int ready, release, callback_calls, callback_blocked;
    int destructors, before_blocked, after_blocked, pending;
};
static struct exit_case *current;

static void ps5_title_stop_coalesced_writes(void)
{
    stop_calls++;
}

#define UIntToPtr(value) ((void *)(uintptr_t)(unsigned int)(value))
#include "wine-title-exit.inc"

static void quit_handler(int signal)
{
    (void)signal;
    quit_delivered++;
}

static int quit_is_blocked(void)
{
    sigset_t mask;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
    return sigismember(&mask, SIGQUIT);
}

static void observed_unregister(void *entry)
{
    struct exit_case *test = current;
    sigset_t pending;

    assert(entry == &test->data.title_thread);
    test->destructors++;
    test->before_blocked = quit_is_blocked();
    if (test->queue_in_destructor) assert(!pthread_kill(pthread_self(), SIGQUIT));
    unregister_title_thread(entry);
    test->after_blocked = quit_is_blocked();
    assert(!sigpending(&pending));
    test->pending = sigismember(&pending, SIGQUIT);
    assert(!test->data.hitch.armed && !test->data.hitch.tid);
}

static void exit_callback(int status)
{
    struct exit_case *test = current;
    assert(status == test->status);
    test->callback_calls++;
    test->callback_blocked = quit_is_blocked();
    if (test->wait_callback)
    {
        pthread_mutex_lock(&test->mutex);
        test->ready = 1;
        pthread_cond_signal(&test->condition);
        while (!test->release) pthread_cond_wait(&test->condition, &test->mutex);
        pthread_mutex_unlock(&test->mutex);
    }
}

static void *exit_client(void *arg)
{
    struct exit_case *test = arg;
    sigset_t quit;

    sigemptyset(&quit);
    sigaddset(&quit, SIGQUIT);
    assert(!pthread_sigmask(test->initially_blocked ? SIG_BLOCK : SIG_UNBLOCK, &quit, NULL));
    test->data.hitch.armed = 1;
    test->data.hitch.tid = test->id;
    assert(!wine_ps5_title_register_current_thread(&title_threads, &test->data.title_thread,
                                                  test->id, &test->data, thread_key, pthread_setspecific));
    if (test->queue_before_exit) assert(!pthread_kill(pthread_self(), SIGQUIT));
    ps5_title_request_exit(test->status);
    abort();
}

static void run_exit_case(unsigned int id, int status, int wait_callback,
                          int initially_blocked, int queue_before_exit,
                          int queue_in_destructor, int closed_gate)
{
    struct exit_case test = {
        .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
        .id = id, .status = status, .wait_callback = wait_callback,
        .initially_blocked = initially_blocked, .queue_before_exit = queue_before_exit,
        .queue_in_destructor = queue_in_destructor
    };
    pthread_t thread;
    void *result = NULL;

    current = &test;
    quit_delivered = 0;
    stop_calls = 0;
    assert(!pthread_create(&thread, NULL, exit_client, &test));
    if (wait_callback)
    {
        pthread_mutex_lock(&test.mutex);
        while (!test.ready) pthread_cond_wait(&test.condition, &test.mutex);
        assert(!pthread_kill(thread, SIGQUIT));
        test.release = 1;
        pthread_cond_signal(&test.condition);
        pthread_mutex_unlock(&test.mutex);
    }
    assert(!pthread_join(thread, &result));
    assert(result == UIntToPtr(status));
    assert(stop_calls == 1 && test.destructors == 1);
    assert(!quit_delivered && test.before_blocked && test.after_blocked);
    assert(test.pending == !!(wait_callback || queue_before_exit || queue_in_destructor));
    assert(test.callback_calls == !closed_gate);
    if (!closed_gate) assert(test.callback_blocked);
    assert(!title_callbacks.active);
    assert(wine_ps5_title_signal_thread(&title_threads, id, &test.data, 0) == -1 && errno == ESRCH);
    pthread_mutex_destroy(&test.mutex);
    pthread_cond_destroy(&test.condition);
}

int main(void)
{
    struct sigaction action = {.sa_handler = quit_handler}, previous;

    alarm(15);
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGQUIT, &action, &previous));
    wine_ps5_title_critical_signals(&ps5_title_block_set);
    title_runtime.exit = exit_callback;
    assert(!pthread_key_create(&thread_key, observed_unregister));
    run_exit_case(101, 37, 1, 0, 0, 0, 0);
    run_exit_case(102, 38, 0, 0, 0, 1, 0);
    run_exit_case(103, -1, 0, 0, 0, 0, 0);
    wine_ps5_title_close_callbacks(&title_callbacks);
    run_exit_case(104, 39, 0, 1, 1, 0, 1);
    assert(!pthread_key_delete(thread_key));
    assert(!sigaction(SIGQUIT, &previous, NULL));
    alarm(0);
    puts("TITLE_EXIT_HOST_PASS callback_return=1 pending_sigquit=1 tls_destructor=1 "
         "closed_gate=1 native_pthread=REAL");
    return 0;
}

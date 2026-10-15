/* Standalone PE qualification for process exit from non-supervisor threads.
 * Run each mode directly in title boot.txt; this deliberately creates no
 * child process. Expected native terminal statuses: worker-exit=37,
 * initial-exit=43, last-thread=47.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

struct exit_args
{
    HANDLE initial;
    DWORD status;
    int last_thread;
};

static struct exit_args args;

static DWORD WINAPI exit_worker( void *arg )
{
    struct exit_args *exit = arg;

    if (exit->initial)
    {
        /* This is a real Windows thread handle, so the worker cannot request
         * shutdown until the initial Windows thread has completely exited. */
        if (WaitForSingleObject( exit->initial, INFINITE ) != WAIT_OBJECT_0) ExitProcess( 1 );
        CloseHandle( exit->initial );
    }
    if (exit->last_thread) ExitThread( exit->status );
    ExitProcess( exit->status );
}

int main( int argc, char **argv )
{
    HANDLE worker;
    int initial_exit;

    if (argc != 2)
    {
        fputs( "usage: ps5_title_windows.exe worker-exit|initial-exit|last-thread\n", stderr );
        return 1;
    }
    if (!strcmp( argv[1], "worker-exit" ))
    {
        args.status = 37;
        initial_exit = 0;
    }
    else if (!strcmp( argv[1], "initial-exit" ) || !strcmp( argv[1], "last-thread" ))
    {
        args.last_thread = !strcmp( argv[1], "last-thread" );
        args.status = args.last_thread ? 47 : 43;
        initial_exit = 1;
        if (!DuplicateHandle( GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &args.initial,
                              SYNCHRONIZE, FALSE, 0 )) return 1;
    }
    else return 1;
    worker = CreateThread( NULL, 0, exit_worker, &args, 0, NULL );
    if (!worker)
    {
        if (args.initial) CloseHandle( args.initial );
        return 1;
    }
    if (initial_exit)
    {
        CloseHandle( worker );
        ExitThread( 11 );
    }
    WaitForSingleObject( worker, INFINITE );
    CloseHandle( worker );
    return 1;  /* worker-exit must terminate the process, not return here */
}

/*
 * recomp_exit_hook.c - the hook a window runs before ExitProcess
 * (recomp_exit_hook.h).
 */
#include "recomp_exit_hook.h"

#include <stddef.h>

/* Set by whichever thread notes the first miss, read by the window thread:
 * an aligned pointer store, so the reader sees the old or the new hook. */
static recomp_exit_hook_fn volatile s_hook;

void recomp_exit_hook_set(recomp_exit_hook_fn fn)
{
    s_hook = fn;
}

void recomp_exit_hook_run(const char *why)
{
    recomp_exit_hook_fn fn = s_hook;
    if (fn != NULL)
        fn(why);
}

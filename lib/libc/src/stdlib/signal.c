/* signal, raise — C11 §7.14.
 *
 * A table of handlers and a dispatcher, and nothing else: the C
 * standard's signal model does not include a way for the OS to DELIVER
 * one, because C does not know what an OS is. `raise` is therefore the
 * only path a signal takes here, and it is a direct call.
 *
 * That is not a simplification of the real thing -- it IS the real
 * thing for a freestanding target. A hosted OS delivers signals by
 * interrupting a thread and calling the handler itself; when a backend
 * gains that ability, it calls __raise_signal below and everything
 * above this line is already correct.
 */
#include <signal.h>
#include <stdlib.h>

#define NSIG 32

static void (*handlers[NSIG])(int);

void (*signal(int sig, void (*handler)(int)))(int)
{
    if (sig <= 0 || sig >= NSIG)
        return SIG_ERR;
    void (*prev)(int) = handlers[sig];
    handlers[sig] = handler;
    return prev ? prev : SIG_DFL;
}

/* The default action for the signals C defines: all six are abnormal
 * termination. A real OS distinguishes core-dumping from not; from here
 * the distinction is invisible and inventing it would be a lie. */
static void default_action(int sig)
{
    (void)sig;
    abort();
}

/* The dispatcher. A backend that can deliver a signal calls this; raise
 * calls it directly. The handler is cleared BEFORE the call, as C11
 * §7.14.1.1p3 requires: a handler that does not reinstall itself runs
 * once, and a signal arriving during its own handler therefore takes
 * the default rather than recursing. */
int __raise_signal(int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return -1;
    void (*h)(int) = handlers[sig];
    if (h == SIG_IGN)
        return 0;
    if (h == SIG_DFL || !h) {
        default_action(sig);
        return 0;
    }
    handlers[sig] = SIG_DFL;
    h(sig);
    return 0;
}

int raise(int sig) { return __raise_signal(sig); }

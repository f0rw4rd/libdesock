#include <sys/select.h>
#include <errno.h>
#include <string.h>
#include <signal.h>  /* for _NSIG */

#include "util.h"
#include "desock.h"
#include "syscall.h"

static int has_desock_fds (int n, fd_set* rfds, fd_set* wfds) {
    for (int i = 0; i < n; ++i) {
        if (rfds && FD_ISSET(i, rfds) && DESOCK_FD(i)) {
            return 1;
        }
        
        if (wfds && FD_ISSET(i, wfds) && DESOCK_FD(i)) {
            return 1;
        }
    }
    
    return 0;
}

static int do_select (int n, fd_set* rfds, fd_set* wfds, fd_set* efds) {
    int ret = 0;
    int server_sock = -1;

    for (int i = 0; i < n; ++i) {
        if (rfds && FD_ISSET(i, rfds)) {
            if (DESOCK_FD(i)) {
                if (fd_table[i].listening) {
                    server_sock = i;
                }

                ++ret;
            } else {
                FD_CLR(i, rfds);
            }
        }

        if (wfds && FD_ISSET(i, wfds)) {
            if (DESOCK_FD(i) && !fd_table[i].listening) {
                ++ret;
            } else {
                FD_CLR(i, wfds);
            }
        }
    }

    if (UNLIKELY(efds)) {
        explicit_bzero(efds, sizeof(fd_set));
    }

    if (server_sock > -1) {
        accept_block = 0;
        
        if (UNLIKELY(sem_trywait(&sem) == -1)) {
            if (UNLIKELY(errno != EAGAIN)) {
                _error("select(): sem_trywait failed");
            }

            if (ret <= 1) {
                sem_wait(&sem);
            } else {
                FD_CLR(server_sock, rfds);
                --ret;
                accept_block = 1;
            }
        }
    }
    
    return ret;
}

#define IS32BIT(x) !((x)+0x80000000ULL>>32)
#define CLAMP(x) (int)(IS32BIT(x) ? (x) : 0x7fffffffU+((0ULL+(x))>>63))

static int musl_select (int n, fd_set* rfds, fd_set* wfds, fd_set* efds, struct timeval* tv) {
    time_t s = tv ? tv->tv_sec : 0;
    suseconds_t us = tv ? tv->tv_usec : 0;
    long ns = 0;
    const time_t max_time = (1ULL << (8 * sizeof(time_t) - 1)) - 1;

    if (s < 0 || us < 0)
        return __syscall_ret(-EINVAL);
    
    if (us / 1000000 > max_time - s) {
        s = max_time;
        us = 999999;
        ns = 999999999;
    } else {
        s += us / 1000000;
        us %= 1000000;
        ns = us * 1000;
    }

#ifdef SYS_pselect6_time64
    int r = -ENOSYS;
    if (SYS_pselect6 == SYS_pselect6_time64 || !IS32BIT(s))
        r = __syscall_cp(SYS_pselect6_time64, n, rfds, wfds, efds, tv ? ((long long[]) { s, ns }
                          ) : 0,
                          ((syscall_arg_t[]) { 0, _NSIG / 8 }));
    if (SYS_pselect6 == SYS_pselect6_time64 || r != -ENOSYS)
        return __syscall_ret(r);
#endif
#ifdef SYS_select
    (void) ns;
    return syscall_cp(SYS_select, n, rfds, wfds, efds, tv ? ((long[]) { s, us }) : 0);
#else
    return syscall_cp(SYS_pselect6, n, rfds, wfds, efds, tv ? ((long[]) { s, ns }) : 0, ((syscall_arg_t[]) { 0, _NSIG / 8 }));
#endif
}

VISIBLE
int select (int n, fd_set* rfds, fd_set* wfds, fd_set* efds, struct timeval* tv) {
    if (UNLIKELY(!has_desock_fds(n, rfds, wfds) || (!rfds && !wfds && !efds))) {
        return musl_select(n, rfds, wfds, efds, tv);
    }
    
    DEBUG_LOG("select(%d, %p, %p, %p)", n, rfds, wfds, efds);
    int r = do_select(n, rfds, wfds, efds);
    DEBUG_LOG(" => %d", r);
    return r;
}
VERSION(select)

VISIBLE
int pselect (int n, fd_set* rfds, fd_set* wfds, fd_set* efds, const struct timespec* ts, const sigset_t* mask) {
    (void) mask;
    
    if (UNLIKELY(!has_desock_fds(n, rfds, wfds) || (!rfds && !wfds && !efds))) {
        return musl_select(n, rfds, wfds, efds, (struct timeval*) ts);
    }
    
    DEBUG_LOG("pselect(%d, %p, %p, %p)", n, rfds, wfds, efds);
    int r = do_select(n, rfds, wfds, efds);
    DEBUG_LOG(" => %d", r);
    return r;
}
VERSION(pselect)

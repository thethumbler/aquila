#ifndef _AQUILA_SIGNAL_H
#define _AQUILA_SIGNAL_H

/*
 * Signal numbers, following newlib's numbering for i386 so they match
 * what user space sees in <signal.h>.
 */

#define SIGHUP      1   /* hangup */
#define SIGINT      2   /* interrupt */
#define SIGQUIT     3   /* quit */
#define SIGILL      4   /* illegal instruction (not reset when caught) */
#define SIGTRAP     5   /* trace trap (not reset when caught) */
#define SIGIOT      6   /* IOT instruction */
#define SIGABRT     6   /* used by abort, replace SIGIOT in the future */
#define SIGEMT      7   /* EMT instruction */
#define SIGFPE      8   /* floating point exception */
#define SIGKILL     9   /* kill (cannot be caught or ignored) */
#define SIGBUS      10  /* bus error */
#define SIGSEGV     11  /* segmentation violation */
#define SIGSYS      12  /* bad argument to system call */
#define SIGPIPE     13  /* write on a pipe with no one to read it */
#define SIGALRM     14  /* alarm clock */
#define SIGTERM     15  /* software termination signal from kill */
#define SIGURG      16  /* urgent condition on IO channel */
#define SIGSTOP     17  /* sendable stop signal not from tty */
#define SIGTSTP     18  /* stop signal from tty */
#define SIGCONT     19  /* continue a stopped process */
#define SIGCHLD     20  /* to parent on child stop or exit */
#define SIGCLD      20  /* System V name for SIGCHLD */
#define SIGTTIN     21  /* to readers pgrp upon background tty read */
#define SIGTTOU     22  /* like TTIN for output if (tp->t_local&LTOSTOP) */
#define SIGIO       23  /* input/output possible signal */
#define SIGPOLL     SIGIO /* System V name for SIGIO */
#define SIGXCPU     24  /* exceeded CPU time limit */
#define SIGXFSZ     25  /* exceeded file size limit */
#define SIGVTALRM   26  /* virtual time alarm */
#define SIGPROF     27  /* profiling time alarm */
#define SIGWINCH    28  /* window changed */
#define SIGLOST     29  /* resource lost (eg, record-lock lost) */
#define SIGUSR1     30  /* user defined signal 1 */
#define SIGUSR2     31  /* user defined signal 2 */

#define NSIG        32  /* signal 0 implied */

/* signal sets are bit masks, signal n is bit (1 << n) */
#define SIG_SETMASK 0   /* set mask with sigprocmask() */
#define SIG_BLOCK   1   /* set of signals to block */
#define SIG_UNBLOCK 2   /* set of signals to unblock */

/*
 * What a signal handler gets as its second argument (the C library's
 * siginfo_t), and sigtimedwait(): why the signal came (si_code), who sent
 * it, and for SIGCHLD how the child changed. The third argument is the
 * interrupted context, a struct aquila_ucontext (<aquila/ucontext.h>).
 *
 * A handler returns into a page the kernel maps in every process (below
 * the stack), which makes the sigreturn system call: nothing in user
 * space has to know about it.
 */
struct aquila_siginfo {
    int           si_signo;
    int           si_code;      /* SI_USER, SI_KERNEL, or the signal's own */
    int           si_errno;     /* 0 */
    int           si_pid;       /* SI_USER: the sender; SIGCHLD: the child */
    unsigned int  si_uid;       /* and its user id */
    int           si_status;    /* SIGCHLD: the exit code, or the signal */
    unsigned long si_addr;      /* SIGSEGV, SIGBUS: the address; SIGILL,
                                 * SIGFPE, SIGTRAP: the instruction */
    unsigned long si_value;     /* sigqueue()'s: not used */
};

/* si_code: who sent the signal, or what caused it; the values are
 * POSIX's names with Linux's numbers (the C library defines the same) */
#define SI_USER         1       /* kill(), pthread_kill(), raise() */
#define SI_QUEUE        2
#define SI_TIMER        3
#define SI_ASYNCIO      4
#define SI_MESGQ        5
#define SI_KERNEL       0x80    /* the kernel: a terminal, a timer, a pipe */

#define ILL_ILLOPC      1       /* SIGILL: illegal opcode */
#define ILL_ILLOPN      2       /* illegal operand */
#define ILL_ILLADR      3       /* illegal addressing mode */
#define ILL_ILLTRP      4       /* illegal trap */
#define ILL_PRVOPC      5       /* privileged opcode */
#define ILL_PRVREG      6       /* privileged register */
#define ILL_COPROC      7       /* coprocessor error */
#define ILL_BADSTK      8       /* internal stack error */

#define FPE_INTDIV      1       /* SIGFPE: integer divide by zero */
#define FPE_INTOVF      2       /* integer overflow */
#define FPE_FLTDIV      3       /* floating-point divide by zero */
#define FPE_FLTOVF      4       /* floating-point overflow */
#define FPE_FLTUND      5       /* floating-point underflow */
#define FPE_FLTRES      6       /* floating-point inexact result */
#define FPE_FLTINV      7       /* invalid floating-point operation */
#define FPE_FLTSUB      8       /* subscript out of range */

#define SEGV_MAPERR     1       /* SIGSEGV: address not mapped */
#define SEGV_ACCERR     2       /* mapped, but not for that access */

#define BUS_ADRALN      1       /* SIGBUS: misaligned address */
#define BUS_ADRERR      2       /* address that doesn't exist */
#define BUS_OBJERR      3       /* object-specific hardware error */

#define TRAP_BRKPT      1       /* SIGTRAP: breakpoint */
#define TRAP_TRACE      2       /* trace trap */

#define CLD_EXITED      1       /* SIGCHLD: the child exited */
#define CLD_KILLED      2       /* a signal ended it */
#define CLD_DUMPED      3       /* and it dumped core (never here) */
#define CLD_TRAPPED     4       /* traced and trapped (never here) */
#define CLD_STOPPED     5       /* it stopped */
#define CLD_CONTINUED   6       /* it continued */

/* sigaltstack(2): an alternate stack for the handlers with SA_ONSTACK
 * (the C library's stack_t) */
struct aquila_stack {
    unsigned long ss_sp;
    int           ss_flags;
    unsigned long ss_size;
};

#define SS_ONSTACK  0x1     /* the thread is running on it */
#define SS_DISABLE  0x2     /* there is none */
#ifndef MINSIGSTKSZ
#define MINSIGSTKSZ 2048    /* the least size: a frame fits */
#define SIGSTKSZ    8192
#endif

/* sigaction(2) flags */
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x1    /* no SIGCHLD when a child stops or continues */
#define SA_SIGINFO   0x2    /* every handler gets the siginfo anyway */
#define SA_NOCLDWAIT 0x4    /* children don't stay zombies (SIGCHLD) */
#define SA_RESTART   0x8    /* a call the signal interrupts is restarted */
#define SA_NODEFER   0x10   /* not blocked while its handler runs */
#define SA_RESETHAND 0x20   /* the handler goes (SIG_DFL) once it's run */
#define SA_ONSTACK   0x40   /* the handler runs on the alternate stack */
#endif

/* sigaction(2) argument, same layout as newlib's struct sigaction */
struct aquila_sigaction {
    unsigned long sa_handler;
    unsigned long sa_mask;
    int           sa_flags;
};

#endif /* ! _AQUILA_SIGNAL_H */

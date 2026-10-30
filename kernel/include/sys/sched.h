#ifndef _SYS_SCHED_H
#define _SYS_SCHED_H

#include <core/system.h>
#include <sys/proc.h>
#include <sys/cpu.h>
#include <sys/waitq.h>

/* The scheduler (sys/sched.c). */

/* The thread running on this CPU (NULL when it's idle), and its process. */
#define curthread (curcpu()->thread)
#define curproc (curthread->owner)

/* Whether processes run, and a thread may sleep (before that, at boot,
 * only the kernel runs). */
extern int sched_running;
void kernel_idle(void);
void scheduler_init(void);
void sched_thread_spawn(struct thread *thread);
void sched_init_spawn(struct proc *init);
void schedule(void);
void sched_idle_wakeup(void);

/* Makes a thread ready to run; makes a woken thread ready. */
void sched_thread_ready(struct thread *thread);
void sched_thread_wake(struct thread *thread);

/* Lets the other ready threads run first. */
void sched_yield(void);

/* An urgent thread was woken up (thread->urgent): it runs once the
 * interrupt returns. */
extern int sched_urgent;

/* The ticks: the boot CPU's, and every CPU's. */
void sched_timer_tick(uint32_t period, int user);
void sched_cpu_tick(uint32_t period, int user);

/* Finds the other CPU that runs `thread`; makes it enter the kernel now;
 * ends the current thread, which was killed. */
struct cpu *sched_thread_cpu(struct thread *thread);
void sched_thread_kick(struct thread *thread);
void sched_thread_killed(void);

/* Sleeping until a time, or on a wait queue with a deadline; the real
 * time timers. */
int  sched_sleep(uint64_t until);
int  sched_wait(struct waitq *waitq, uint64_t until);
void sched_timer_cancel(struct thread *thread);
void sched_alarm(struct proc *proc, uint64_t value, uint64_t interval,
                 uint64_t *old_value, uint64_t *old_interval);
void sched_alarm_get(struct proc *proc, uint64_t *value, uint64_t *interval);

#endif /* ! _SYS_SCHED_H */

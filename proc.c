#include <stddef.h>
#include "schedulinginterface.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define RR    1
#define PR    2
#define EPR    3

int curr_policy = RR;
long long time_quant = 0;

extern PriorityQueue pq;
extern RoundRobinQueue rrq;
extern RunningProcessesHolder rpholder;

long long getAccumulator(struct proc *p) {
    return p->accumulator;
}

struct {
    struct spinlock lock;
    struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;

extern void forkret(void);

extern void trapret(void);

static void wakeup1(void *chan);

boolean enqueue(struct proc *p) {
    switch (curr_policy) {
        case RR:
            return rrq.enqueue(p);
        case PR:
            return pq.put(p);
        case EPR:
            return pq.put(p);
        default:
            return rrq.enqueue(p);
    }
}

struct proc *dequeue() {
    switch (curr_policy) {
        case RR:
            return rrq.dequeue();
        case PR:
            return pq.extractMin();
        case EPR:
            return pq.extractMin();
        default:
            return rrq.dequeue();
    }
}

boolean isEmpty() {
    switch (curr_policy) {
        case RR:
            return rrq.isEmpty();
        case PR:
            return pq.isEmpty();
        case EPR:
            return pq.isEmpty();
        default:
            return rrq.isEmpty();
    }
}

void init_accumulator(struct proc *p) {
    long long tmp1;
    long long tmp2;
    if (isEmpty()) {
        p->accumulator = 0;
    } else {
        pq.getMinAccumulator(&tmp1);
        rpholder.getMinAccumulator(&tmp2);
        if (tmp1 < tmp2)
            p->accumulator = tmp1;
        else p->accumulator = tmp2;
    }
}

void
pinit(void) {
    initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
    return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void) {
    int apicid, i;

    if (readeflags() & FL_IF)
        panic("mycpu called with interrupts enabled\n");

    apicid = lapicid();
    // APIC IDs are not guaranteed to be contiguous. Maybe we should have
    // a reverse map, or reserve a register to store &cpus[i].
    for (i = 0; i < ncpu; ++i) {
        if (cpus[i].apicid == apicid)
            return &cpus[i];
    }
    panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void) {
    struct cpu *c;
    struct proc *p;
    pushcli();
    c = mycpu();
    p = c->proc;
    popcli();
    return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *
allocproc(void) {
    struct proc *p;
    char *sp;

    acquire(&ptable.lock);

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == UNUSED)
            goto found;

    release(&ptable.lock);
    return 0;

    found:
    p->state = EMBRYO;
    p->pid = nextpid++;

    p->ctime = ticks;
    p->retime = 0;
    p->rutime = 0;
    p->stime = 0;

    release(&ptable.lock);

    // Allocate kernel stack.
    if ((p->kstack = kalloc()) == 0) {
        p->state = UNUSED;
        return 0;
    }
    sp = p->kstack + KSTACKSIZE;

    // Leave room for trap frame.
    sp -= sizeof *p->tf;
    p->tf = (struct trapframe *) sp;

    // Set up new context to start executing at forkret,
    // which returns to trapret.
    sp -= 4;
    *(uint *) sp = (uint) trapret;

    sp -= sizeof *p->context;
    p->context = (struct context *) sp;
    memset(p->context, 0, sizeof *p->context);
    p->context->eip = (uint) forkret;

    return p;
}

struct proc *get_runnable_p();

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void) {
    struct proc *p;
    extern char _binary_initcode_start[], _binary_initcode_size[];

    p = allocproc();

    initproc = p;
    if ((p->pgdir = setupkvm()) == 0)
        panic("userinit: out of memory?");
    inituvm(p->pgdir, _binary_initcode_start, (int) _binary_initcode_size);
    p->sz = PGSIZE;
    p->ctime = ticks;
    memset(p->tf, 0, sizeof(*p->tf));
    p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
    p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
    p->tf->es = p->tf->ds;
    p->tf->ss = p->tf->ds;
    p->tf->eflags = FL_IF;
    p->tf->esp = PGSIZE;
    p->tf->eip = 0;  // beginning of initcode.S

    safestrcpy(p->name, "initcode", sizeof(p->name));
    p->cwd = namei("/");

    // this assignment to p->state lets other cores
    // run this process. the acquire forces the above
    // writes to be visible, and the lock is also needed
    // because the assignment might not be atomic.
    acquire(&ptable.lock);

    p->state = RUNNABLE;

    p->wait_start = time_quant;

//    cprintf("USERINIT process %d wait time: %d\n",p->pid, p->wait_start);

    p->priority = 5;

    init_accumulator(p);

    enqueue(p);

    release(&ptable.lock);
}

struct proc *get_runnable_p() {
    if (curr_policy == EPR && time_quant % 100 == 0 && time_quant != 0) {
        struct proc *tmp_p;
        struct proc *p = 0;
        long long max_wait = -1;

        for (tmp_p = ptable.proc; tmp_p < &ptable.proc[NPROC]; tmp_p++) {
            if (tmp_p->state == RUNNABLE) {
                if ((time_quant - tmp_p->wait_start) > max_wait) {
                    max_wait = time_quant - tmp_p->wait_start;
                    p = tmp_p;
                }
            }
        }
        pq.extractProc(p);
        return p;
    } else
        return dequeue();
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n) {
    uint sz;
    struct proc *curproc = myproc();

    sz = curproc->sz;
    if (n > 0) {
        if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
            return -1;
    } else if (n < 0) {
        if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
            return -1;
    }
    curproc->sz = sz;
    switchuvm(curproc);
    return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void) {
    int i, pid;
    struct proc *np;
    struct proc *curproc = myproc();

    // Allocate process.
    if ((np = allocproc()) == 0) {
        return -1;
    }

    // Copy process state from proc.
    if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0) {
        kfree(np->kstack);
        np->kstack = 0;
        np->state = UNUSED;
        return -1;
    }
    np->sz = curproc->sz;
    np->parent = curproc;
    *np->tf = *curproc->tf;

    // Clear %eax so that fork returns 0 in the child.
    np->tf->eax = 0;

    for (i = 0; i < NOFILE; i++)
        if (curproc->ofile[i])
            np->ofile[i] = filedup(curproc->ofile[i]);
    np->cwd = idup(curproc->cwd);

    safestrcpy(np->name, curproc->name, sizeof(curproc->name));

    pid = np->pid;

    acquire(&ptable.lock);

    np->state = RUNNABLE;
    np->wait_start = time_quant;
    np->priority = 5;
    init_accumulator(np);
    enqueue(np);

    release(&ptable.lock);

    return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait(0) to find out it exited.
void
exit(int status) {
    struct proc *curproc = myproc();
    struct proc *p;
    int fd;

    if (curproc == initproc)
        panic("init exiting");

    // Close all open files.
    for (fd = 0; fd < NOFILE; fd++) {
        if (curproc->ofile[fd]) {
            fileclose(curproc->ofile[fd]);
            curproc->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;

    acquire(&ptable.lock);

    // Parent might be sleeping in wait(0).
    wakeup1(curproc->parent);

    // Pass abandoned children to init.
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->parent == curproc) {
            p->parent = initproc;
            if (p->state == ZOMBIE)
                wakeup1(initproc);
        }
    }
    curproc->status = status;
    curproc->ttime = ticks;
    // Jump into the scheduler, never to return.
    curproc->state = ZOMBIE;
    rpholder.remove(curproc);
    sched();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(int *status) {
    struct proc *p;
    int havekids, pid;
    struct proc *curproc = myproc();

    acquire(&ptable.lock);
    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->parent != curproc)
                continue;
            havekids = 1;
            if (p->state == ZOMBIE) {
                // Found one.
                pid = p->pid;
                kfree(p->kstack);
                p->kstack = 0;
                freevm(p->pgdir);
                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->ctime = 0;
                p->ttime = 0;
                p->stime = 0;
                p->retime = 0;
                p->rutime = 0;
                p->state = UNUSED;
                if (status != 0) {
                    *status = p->status;
                }
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || curproc->killed) {
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(curproc, &ptable.lock);  //DOC: wait-sleep
    }
}

int
detach(int pid) {
    struct proc *curproc = myproc();
    struct proc *p;

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->pid == pid && p->parent == curproc) {
            if (p->state != ZOMBIE) {
                p->parent = initproc;
                p->ttime = ticks;
                return p->status;
            } else return -1;
        }
    }
    return -1;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void) {
    struct proc *p;
    struct cpu *c = mycpu();
    c->proc = 0;

    for (;;) {
        // Enable interrupts on this processor.
        sti();

        // Loop over process table looking for process to run.
        acquire(&ptable.lock);
        if (!isEmpty()) {
            p = get_runnable_p();
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;
            p->wait_start = 0;
            // add RUNNING process to running process holder
            rpholder.add(p);

            swtch(&(c->scheduler), p->context);
            switchkvm();

            // Process is done running for now.
            // It should have changed its p->state before coming back.
            c->proc = 0;
        }
        release(&ptable.lock);

    }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void) {
    int intena;
    struct proc *p = myproc();

    if (!holding(&ptable.lock))
        panic("sched ptable.lock");
    if (mycpu()->ncli != 1)
        panic("sched locks");
    if (p->state == RUNNING)
        panic("sched running");
    if (readeflags() & FL_IF)
        panic("sched interruptible");
    intena = mycpu()->intena;
    swtch(&p->context, mycpu()->scheduler);
    mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void) {
    acquire(&ptable.lock);  //DOC: yieldlock
    time_quant++;
//    cprintf("time quant: %d\n", time_quant);
    myproc()->state = RUNNABLE;
    myproc()->wait_start = time_quant;
//    cprintf("YIELD p %d started waiting at: %d\n", myproc()->pid, myproc()->wait_start);
    enqueue(myproc());

    myproc()->accumulator += myproc()->priority;

    rpholder.remove(myproc());

    sched();
    release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void) {
    static int first = 1;
    // Still holding ptable.lock from scheduler.
    release(&ptable.lock);

    if (first) {
        // Some initialization functions must be run in the context
        // of a regular process (e.g., they call sleep), and thus cannot
        // be run from main().
        first = 0;
        iinit(ROOTDEV);
        initlog(ROOTDEV);
    }

    // Return to "caller", actually trapret (see allocproc).cprintf
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk) {
    struct proc *p = myproc();

    if (p == 0)
        panic("sleep");

    if (lk == 0)
        panic("sleep without lk");

    // Must acquire ptable.lock in order to
    // change p->state and then call sched.
    // Once we hold ptable.lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup runs with ptable.lock locked),
    // so it's okay to release lk.
    if (lk != &ptable.lock) {  //DOC: sleeplock0
        acquire(&ptable.lock);  //DOC: sleeplock1
        release(lk);
    }
    // Go to sleep.
    p->chan = chan;
    p->state = SLEEPING;
    rpholder.remove(p);

    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    if (lk != &ptable.lock) {  //DOC: sleeplock2
        release(&ptable.lock);
        acquire(lk);
    }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan) {
    struct proc *p;

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
        if (p->state == SLEEPING && p->chan == chan) {
            p->state = RUNNABLE;
            p->wait_start = time_quant;
            init_accumulator(p);
            enqueue(p);
        }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan) {
    acquire(&ptable.lock);
    wakeup1(chan);
    release(&ptable.lock);

}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid) {
    struct proc *p;
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->pid == pid) {
            p->killed = 1;
            // Wake process from sleep if necessary.
            if (p->state == SLEEPING) {
                p->state = RUNNABLE;
                p->wait_start = time_quant;
//                cprintf("KILL p %d started waiting at: %d\n", p->pid, p->wait_start);
                init_accumulator(p);
                enqueue(p);
            }
            release(&ptable.lock);
            return 0;
        }
    }
    release(&ptable.lock);
    return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void) {
    static char *states[] = {
            [UNUSED]    "unused",
            [EMBRYO]    "embryo",
            [SLEEPING]  "sleep ",
            [RUNNABLE]  "runble",
            [RUNNING]   "run   ",
            [ZOMBIE]    "zombie"
    };
    int i;
    struct proc *p;
    char *state;
    uint pc[10];

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        cprintf("%d %s %s", p->pid, state, p->name);
        if (p->state == SLEEPING) {
            getcallerpcs((uint *) p->context->ebp + 2, pc);
            for (i = 0; i < 10 && pc[i] != 0; i++)
                cprintf(" %p", pc[i]);
        }
        cprintf("\n");
    }
}

void
priority(int priority) {
    if (curr_policy == PR) {
        if (priority > 10 || priority < 1) {
            return;
        }
    }
    struct proc *p = myproc();
    p->priority = priority;
}

void
policy(int policy) {
    acquire(&ptable.lock);
    struct proc *tmp_p;
    // RR is selected - init all accumulators to 0
    if (policy == RR) {
        for (tmp_p = ptable.proc; tmp_p < &ptable.proc[NPROC]; tmp_p++) {
            tmp_p->accumulator = 0;
        }
        if (curr_policy != RR)
            pq.switchToRoundRobinPolicy();

    }
    // PR is selected - change priority 0 to 1
    if (policy == PR || policy == EPR) {
        if (policy == PR) {
            for (tmp_p = ptable.proc; tmp_p < &ptable.proc[NPROC]; tmp_p++) {
                if (tmp_p->priority == 0) tmp_p->priority = 1;
            }
        }
        if (curr_policy == RR)
            rrq.switchToPriorityQueuePolicy();
    }
    curr_policy = policy;
    release(&ptable.lock);
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait_stat(int *status, struct perf *perfPtr) {
    struct proc *p;
    int havekids, pid;

    acquire(&ptable.lock);
    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;
        for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
            if (p->parent != myproc())
                continue;
            havekids = 1;
            if (p->state == ZOMBIE) {
                if (status != 0) {
                    *status = p->status;
                }
                perfPtr->ctime = p->ctime;
                perfPtr->ttime = p->ttime;
                perfPtr->stime = p->stime;
                perfPtr->retime = p->retime;
                perfPtr->rutime = p->rutime;

                // Found one.
                pid = p->pid;
                kfree(p->kstack);
                p->kstack = 0;
                freevm(p->pgdir);
                p->pid = 0;
                p->parent = 0;
                p->name[0] = 0;
                p->killed = 0;
                p->ctime = 0;
                p->ttime = 0;
                p->stime = 0;
                p->retime = 0;
                p->rutime = 0;

                p->state = UNUSED;
                release(&ptable.lock);
                return pid;
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || myproc()->killed) {
            release(&ptable.lock);
            return -1;
        }

        // Wait for children to exit.  (See wakeup1 call in proc_exit.)
        sleep(myproc(), &ptable.lock);  //DOC: wait-sleep
    }
}

void incCounters(void) {
    struct proc *p;
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->state == SLEEPING)
            p->stime++;
        if (p->state == RUNNABLE)
            p->retime++;
        if (p->state == RUNNING)
            p->rutime++;
    }
    release(&ptable.lock);
}
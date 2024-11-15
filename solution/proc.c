#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "pstat.h"

#define STRIDE1 (1<<10)
#define MAX_TICKETS (1<<5)

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

// global variables we need
extern int useStrideScheduler;
extern int globalTickets;
extern int globalStride;
extern int globalPass;
struct pstat pstats = {0};

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
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
struct proc*
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
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

 found:
  uint proc_index = p - ptable.proc;
  
  p->state = EMBRYO;
  p->pid = nextpid++;
  pstats.inuse[proc_index] = 1;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    // MARKER_PSTATS_UPDATE
    pstats.inuse[proc_index] = 0;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  p->tickets = 8;
  p->stride = STRIDE1 / p->tickets;
  p->remain = 0;
  p->pass = globalPass;
  updatePstatsForProcess(p); // update pstats

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
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
  processJoinsQueue(p);
  cprintf("Userinit : %d\n", p->pid);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
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
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  uint proc_index = np - ptable.proc;
  
  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    // MARKER_PSTATS_UPDATE
    pstats.inuse[proc_index] = 0;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  processJoinsQueue(np);
  
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;
  //  uint proc_index;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  //  proc_index = (curproc - ptable.proc)/sizeof(struct proc);
  //  pstats.inuse[proc_index] = 0;

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  // leaves queue
  processLeavesQueue(curproc);

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  uint proc_index;
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
	// MARKER_PSTATS_UPDATE
	proc_index = p - ptable.proc;
	pstats.inuse[proc_index] = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// helper function of recomputing process pass with remain
void recomputePassWithRemain(struct proc *p){
  // this process remain is assumed to be correct
  p->pass = p->remain + globalPass;
}

void computeRemainBeforeLeaving(struct proc *p){
  p->remain = p->pass - globalPass;
  globalTickets -= p->tickets;
  if(globalTickets != 0) globalStride = STRIDE1 / globalTickets;
}

// joining and leaving queue
void processJoinsQueue(struct proc *p){
  globalTickets += p->tickets;
  // recompute global variables
  globalStride = STRIDE1 / globalTickets;
  p->pass = globalPass + p->remain;
  updatePstatsForProcess(p);
}

void processLeavesQueue(struct proc *p){
  globalTickets -= p->tickets;
  if(globalTickets > 0) globalStride = STRIDE1 / globalTickets;
  else globalStride = 0;
  // compute remain
  p->remain = p->pass - globalPass;
  updatePstatsForProcess(p);
}

void updatePstatsForProcess(struct proc *p){
  if(p == 0) return;
  int index = p - ptable.proc;

  if(p->state == UNUSED){
	pstats.inuse[index] = 0;
    return;
  }
  
  pstats.inuse[index] = 1; 
  pstats.tickets[index] = p->tickets;
  pstats.pid[index] = p->pid;
  pstats.pass[index] = p->pass;
  pstats.remain[index] = p->remain;
  pstats.stride[index] = p->stride;
  pstats.rtime[index] = p->ticksTaken;
  return;
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
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  int lowestPass = 0x7FFFFF;
  struct proc *nextProcToRun = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

	// Stride scheduler
	if(useStrideScheduler == 1){
		lowestPass = 0x7FFFFF;
		// go through the processes and select the process to run
		acquire(&ptable.lock);
		for(int i = 0; i < NPROC; i++) {
			p = &ptable.proc[i];
			updatePstatsForProcess(p); // update pstats
			// update pstats for all processes
			if(p->state != RUNNABLE) continue;

			// here onwards are only processes that are RUNNABLE

			// initial
			if(nextProcToRun == 0){
				lowestPass = p->pass;
				nextProcToRun = p;
				continue;
			}

			if(p->pass > lowestPass) continue; // skip if bigger pass
			if(p->pass < lowestPass){
				lowestPass = p->pass;
				nextProcToRun = p;
				continue;
			}
			if(p->pass == lowestPass){
				if(p->ticksTaken > nextProcToRun->ticksTaken) continue; // skip if this p taken more ticks
				if(p->ticksTaken == nextProcToRun->ticksTaken){
					if(p->pid >= nextProcToRun->pid) continue; // skip if pid is bigger (equal is impossible)
					nextProcToRun = p;
					continue;
				}
				// lower ticksTaken so we select it
				nextProcToRun = p;
				continue;
			}
		}

		if(nextProcToRun != 0){
			c->proc = nextProcToRun;
			switchuvm(nextProcToRun);
			nextProcToRun->state = RUNNING;

			swtch(&(c->scheduler), nextProcToRun->context);
			switchkvm();
		
			// increment process pass after run
			nextProcToRun->pass += nextProcToRun->stride;
	
			c->proc = 0;
			nextProcToRun = 0;
		}
		release(&ptable.lock);
	}else{
		// Loop over process table looking for process to run.
		acquire(&ptable.lock);
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			updatePstatsForProcess(p);
			if(p->state != RUNNABLE)
				continue;

			// Switch to chosen process.  It is the process's job
			// to release ptable.lock and then reacquire it
			// before jumping back to us.
			c->proc = p;
			switchuvm(p);
			p->state = RUNNING;

			swtch(&(c->scheduler), p->context);
			switchkvm();

			// Process is done running for now.
			// It should have changed its p->state before coming back.
			c->proc = 0;
		}
		release(&ptable.lock);
    }
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
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
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

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
 
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // leaves queue
  processLeavesQueue(p);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
	  p->state = RUNNABLE;
	  processJoinsQueue(p);
	}
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
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
procdump(void)
{
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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// Process can set tickets for itself
int
settickets(int n)
{
  /*
   * Min = 1
   * Max = MAX_TICKETS
   * If < 1, set to 8
   */

  acquire(&ptable.lock); // acquiring lock early so as to set the change of tickets to the right process
    
  int new_tickets_count;
  int old_stride_count;
  
  if(n < 1)
    new_tickets_count = 8;
  else {
    if(n > MAX_TICKETS)
      new_tickets_count = MAX_TICKETS;
    else
      new_tickets_count = n;
  }

  struct proc *cur_proc = myproc();
  
  old_stride_count = cur_proc->stride;
  
  // update global tickets
  globalTickets -= cur_proc->tickets;
  cur_proc->tickets = new_tickets_count;
  globalTickets += cur_proc->tickets;
  globalStride = STRIDE1 / globalTickets;
  // continueing updating the process
  cur_proc->stride = STRIDE1/cur_proc->tickets;
  cur_proc->remain = cur_proc->remain * (cur_proc->stride/old_stride_count);
  cur_proc->pass = globalPass + cur_proc->remain;

  // update pstats
  updatePstatsForProcess(cur_proc);

  release(&ptable.lock);
  
  return new_tickets_count; // returns the number of tickets set for the process
}

int
getpinfo(struct pstat *ret_pstat)
{

  if(ret_pstat == 0)
    return -1;

  acquire(&ptable.lock);

  for(int i = 0; i < NPROC; i++) {
    ret_pstat->inuse[i] = pstats.inuse[i];
    ret_pstat->tickets[i] = pstats.tickets[i];
    ret_pstat->pid[i] = pstats.pid[i];
    ret_pstat->pass[i] = pstats.pass[i];
    ret_pstat->remain[i] = pstats.remain[i];
    ret_pstat->stride[i] = pstats.stride[i];
    ret_pstat->rtime[i] = pstats.rtime[i];
  }

  release(&ptable.lock);
  
  return 0;
}

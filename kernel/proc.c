#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct proc head_unused;
struct proc head_sleeping;
struct proc head_zombie;

struct proc *proc_heads[] = {&head_unused, &head_sleeping, &head_zombie};

enum list {UNUSED_LIST, SLEEPING_LIST, ZOMBIE_LIST, RUNNABLE_LIST};

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

extern uint64 cas(volatile void *addr, int expected, int newval);

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;


// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

void print_list(enum list list) {
  struct proc* p;
  if (list == RUNNABLE_LIST) {  // RUNNABLE
    struct cpu* cpu;
    int i = 0;
    for (cpu = cpus; cpu < &cpus[NCPU]; cpu++) {
      printf("CPU %d:\n", i);
      i++;
      p = &(cpu->head_runnable);
      acquire(&p->link);
      while (p->next != -1) {
        release(&p->link);
        p = &proc[p->next];
        acquire(&p->link);
        printf("%d\n", p->index);
      }
      release(&p->link);
    }
  }
  else {                        // UNUSED, SLEEPING, ZOMBIE
    p = proc_heads[list];
    acquire(&p->link);
    while (p->next != -1) {
      release(&p->link);
      p = &proc[p->next];
      acquire(&p->link);
      printf("%d\n", p->index);
    }
    release(&p->link);
  }
}

void print_lists(void) {
  printf("UNUSED\n");
  print_list(UNUSED_LIST);
  printf("SLEEPING\n");
  print_list(SLEEPING_LIST);
  printf("ZOMBIE\n");
  print_list(ZOMBIE_LIST);
  printf("RUNNABLE\n");
  print_list(RUNNABLE_LIST);
}

void insert(int new, enum list list) {
  struct proc* p;
  struct cpu* cpu;
  if (list == RUNNABLE_LIST) {  // RUNNABLE
    p = &proc[new];
    acquire(&p->link);
    cpu = &cpus[p->cpu];
    release(&p->link);
    p = &(cpu->head_runnable);
  }
  else p = proc_heads[list];    // UNUSED, SLEEPING, ZOMBIE 
  acquire(&p->link);
  while (p->next != -1) {
    release(&p->link);
    p = &proc[p->next];
    acquire(&p->link);
  }
  p->next = new;
  release(&p->link);
  p = &proc[new];
  acquire(&p->link);
  p->next = -1;
  release(&p->link);
}

void remove(int index, enum list list) {
  struct cpu* cpu;
  struct proc* p;
  struct proc* pred;
  struct proc* curr;
  if (list == RUNNABLE_LIST) {  // RUNNABLE
    p = &proc[index];
    acquire(&p->link);
    cpu = &cpus[p->cpu];
    release(&p->link);
    pred = &(cpu->head_runnable);
  }
  else pred = proc_heads[list]; // UNUSED, SLEEPING, ZOMBIE
  acquire(&pred->link);
  if (pred->next != -1) {
    curr = &proc[pred->next];
    acquire(&curr->link);
    while (curr->index != index && curr->next != -1) {
      release(&pred->link);
      pred = curr;
      curr = &proc[pred->next];
      acquire(&curr->link);
    }
    if (curr->index == index)   // found
      pred->next = curr->next;
    release(&curr->link);
  }
  release(&pred->link);
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  struct cpu *c;
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  int i = 0;

  for (i = 0; i < 3; i++) {
    initlock(&proc_heads[i]->lock, "head");
    initlock(&proc_heads[i]->link, "head_link");
    proc_heads[i]->index = -1;
    proc_heads[i]->next = -1;
    if (i == 1) {
      proc_heads[i]->name[0] = 's';
      proc_heads[i]->name[1] = 'k';
    }

  }
  for (c = cpus; c < &cpus[NCPU]; c++) {
    p = &(c->head_runnable);
    initlock(&p->lock, "head");
    initlock(&p->link, "head_link");
    p->index = -1;
    p->next = -1;
    c->counter = 0;
  }
  i = 0;
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
      initlock(&p->link, "proc_link");
      p->index = i;
      p->next = -1;
      i++;
      if (i == NPROC)
        break;
      p->cpu = -1;
      insert(i ,UNUSED_LIST);
  }

}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  do {
    pid = nextpid;
  } while (cas(&nextpid, pid, pid+1));
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  remove(p->index, UNUSED_LIST);
  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  remove(p->index, ZOMBIE_LIST);
  insert(p->index, UNUSED_LIST);
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  p->cpu = cpuid();
  int counter;
  do {
    counter = cpu_process_count(p->cpu);
  } while (cas(&(&cpus[p->cpu])->counter, counter, counter+1));
  printf("count: %d\n", (&cpus[p->cpu])->counter);
  insert(p->index, RUNNABLE_LIST);
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);
  acquire(&np->lock);
  np->state = RUNNABLE;
  np->cpu = p->cpu;
  insert(np->index, RUNNABLE_LIST);
  release(&np->lock);
  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();
  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){

    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;
  acquire(&wait_lock);
  // Give any children to init.
  reparent(p);
  // Parent might be sleeping in wait().
  wakeup(p->parent);
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  insert(p->index, ZOMBIE_LIST);
  release(&wait_lock);

  // Jump into the scheduler, never to return.

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();
  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.

  // #ifdef OFF
  //   round_robin();
  // #endif

  // #ifdef ON
  //   round_robin();
  // #endif

void
scheduler(void)
{
  struct proc *p;
  struct proc *next;
  struct cpu *c = mycpu();
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    p = &(c->head_runnable);
    acquire(&p->lock);
    if(p->next == -1){      // no runnable process
      release(&p->lock);
      continue;
    }
    next = &proc[p->next];
    acquire(&next->lock);
    release(&p->lock);
    next->state = RUNNING;
    remove(next->index, RUNNABLE_LIST);

    c->proc = next;
    swtch(&c->context, &next->context);
    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&next->lock);
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  insert(p->index, RUNNABLE_LIST);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  insert(p->index, SLEEPING_LIST);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.

  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *pred;
  struct proc *curr;
  pred = proc_heads[SLEEPING_LIST];

  acquire(&pred->link);
  if(pred->next != -1) {
    curr = &proc[pred->next];
    acquire(&curr->link);
    while (curr->next != -1) {
      if(curr->chan == chan) {
        curr->state = RUNNABLE;
        pred->next = curr->next;
        release(&curr->link);
        insert(curr->index, RUNNABLE_LIST);
        curr = &proc[pred->next];
        acquire(&curr->link);
      }
      else {
        release(&pred->link);
        pred = curr;
        curr = &proc[curr->next];
        acquire(&curr->link);
      }
    }
    if (curr->chan == chan) {
      curr->state = RUNNABLE;
      pred->next = -1;
      release(&curr->link);
      insert(curr->index, RUNNABLE_LIST);
    }
    else release(&curr->link);
  }
  release(&pred->link);
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
        remove(p->index, SLEEPING_LIST);
        insert(p->index, RUNNABLE_LIST);
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
set_cpu(int cpu_num)
{
  struct proc* p = myproc();
  p->cpu = cpu_num;
  yield();
  return cpu_num;
}

int
get_cpu(void)
{
  struct proc* p = myproc();
  return p->cpu;
}

int
cpu_process_count(int cpu_num)
{
  return (&cpus[cpu_num])->counter;
}
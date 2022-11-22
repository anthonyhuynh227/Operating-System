#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>
#include <fs.h>
#include <file.h>
#include <vspace.h>



// process table
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// to test crash safety in lab5,
// we trigger restarts in the middle of file operations
void reboot(void) {
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
loop:
  asm volatile("hlt");
  goto loop;
}

void pinit(void) { initlock(&ptable.lock, "ptable"); }

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *allocproc(void) {
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
  p->killed = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0) {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trap_frame *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 8;
  *(uint64_t *)sp = (uint64_t)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (uint64_t)forkret;
  return p;
}

// Set up first user process.
void userinit(void) {
  struct proc *p;
  extern char _binary_out_initcode_start[], _binary_out_initcode_size[];

  p = allocproc();

  initproc = p;
  assertm(vspaceinit(&p->vspace) == 0, "error initializing process's virtual address descriptor");
  vspaceinitcode(&p->vspace, _binary_out_initcode_start, (int64_t)_binary_out_initcode_size);
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ss = (SEG_UDATA << 3) | DPL_USER;
  p->tf->rflags = FLAGS_IF;
  p->tf->rip = VRBOT(&p->vspace.regions[VR_CODE]);  // beginning of initcode.S
  p->tf->rsp = VRTOP(&p->vspace.regions[VR_USTACK]);

  safestrcpy(p->name, "initcode", sizeof(p->name));

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void) {

  // Create new process
  struct proc* new_proc = allocproc();

  acquire(&ptable.lock);
  struct proc* curr_proc = myproc();

  // Initialize virtual space
  vspaceinit(&new_proc->vspace);
  vspacecopy(&new_proc->vspace, &curr_proc->vspace);

  // set the curr_proc as the parent proce of new process
  new_proc->parent = curr_proc;

  // set new proc state to RUNNABLE
  new_proc->state = RUNNABLE;
  
  // Copy trap frame
  memmove(new_proc->tf, curr_proc->tf, sizeof(struct trap_frame));

  // Copy 0 into rax for child
  new_proc->tf->rax = 0;

  // Copy over the file descriptors from parent process
  memmove(&new_proc->file_array, &curr_proc->file_array, sizeof(struct desc) * 16);
  
  // increment all global file reference counts to match the additional file descriptors. 
  for (int i = 0; i < NOFILE; i++) {
    if (curr_proc->file_array[i].available == DESC_NOT_AVAIL) {
      curr_proc->file_array[i].fileptr->ref_count++;
    }
  }
  release(&ptable.lock);
  vspaceinstall(myproc());
  return new_proc->pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void) {
  acquire(&ptable.lock);

  myproc()->state = ZOMBIE;
  
  // Decrement ref counts to global_files
  for (int i = 0; i < NOFILE; i++) {
    // If we are using a file descriptor, then decrement ref count of the file it points to
    struct desc curr_file = myproc()->file_array[i];
    if (curr_file.available == DESC_NOT_AVAIL) {
      curr_file.fileptr->ref_count--;
      if (curr_file.fileptr->ref_count <= 0) {
        // Deallocate resource depending on if its a file or a pipe
        if (curr_file.fileptr->file_type == FILE) {
          irelease(curr_file.fileptr->inodep);
        } else if (curr_file.fileptr->file_type == PIPE) {
          struct pipe* pipe = curr_file.fileptr->pipeptr;
          int ref_count = 0;
          for (int i = 0; i < NFILE; i++) {
            struct file i_file = global_files.files[i];
            if (i_file.available == FILE_NOT_AVAIL && i_file.file_type == PIPE && i_file.pipeptr == pipe) {
              ref_count += i_file.ref_count;
            }
          }
          // Signal all threads waiting on the pipe
          wakeup1(pipe);

          // Free pipe if no references to it anymore
          if (ref_count == 0) {
            kfree((char*) pipe);
          }
        }
        curr_file.fileptr->available = FILE_AVAIL;
      }
    }

    //vspacefree(&myproc()->vspace);
  }

  // Hand over children to init process
  for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      // All children are now owned by init
      if (p->parent == myproc()) {
        p->parent = initproc;
      }
  }
  
  // Wakeup parent in case it was waiting for this child
  wakeup1(myproc()->parent);
  
  // Schedule into the next process
  sched();
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void) {
  acquire(&ptable.lock);

  // Scan through table looking for exited children.
  while (1) {
    bool hasChildren = false;

    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      // If we find a valid zombie child, deallocate process and return id to user
      if (p->parent == myproc() && p->state == ZOMBIE) {
        int child_pid = p->pid;

        // Deallocate data structures
        p->state = UNUSED;
        kfree(p->kstack);
        vspacefree(&p->vspace);
        release(&ptable.lock);
        return child_pid;
      }
      else if (p->parent == myproc() && (p->state == RUNNABLE || p->state == SLEEPING || p->state == RUNNING)) {
        hasChildren = true;
      }
    }

    // If we do not have children, then exit loop
    if (!hasChildren) {
      break;
    }

    // Sleep and wait until woken up by child
    sleep(myproc(), &ptable.lock);
  }

  // There were no children for this process, so return error
  cprintf("Error wait(): There were no children for this process\n");
  release(&ptable.lock);
  return -1;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void) {
  struct proc *p;

  for (;;) {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      mycpu()->proc = p;
      vspaceinstall(p);
      p->state = RUNNING;
      swtch(&mycpu()->scheduler, p->context);
      vspaceinstallkern();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      mycpu()->proc = 0;
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
void sched(void) {
  int intena;

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1) {
    cprintf("pid : %d\n", myproc()->pid);
    cprintf("ncli : %d\n", mycpu()->ncli);
    cprintf("intena : %d\n", mycpu()->intena);

    panic("sched locks");
  }
  if (myproc()->state == RUNNING)
    panic("sched running");
  if (readeflags() & FLAGS_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&myproc()->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void) {
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void) {
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk) {
  if (myproc() == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock) { // DOC: sleeplock0
    acquire(&ptable.lock);  // DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  myproc()->chan = chan;
  myproc()->state = SLEEPING;
  sched();

  // Tidy up.
  myproc()->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock) { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan) {
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan) {
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid) {
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
  static char *states[] = {[UNUSED] = "unused",   [EMBRYO] = "embryo",
                           [SLEEPING] = "sleep ", [RUNNABLE] = "runble",
                           [RUNNING] = "run   ",  [ZOMBIE] = "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint64_t pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state != 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING) {
      getcallerpcs((uint64_t *)p->context->rbp, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

struct proc *findproc(int pid) {
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid)
      return p;
  }
  return 0;
}
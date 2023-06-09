#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

int num_page_faults = 0;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();
    
    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      // Case: Handle possible stack page fault from user
      // Check whether the address is within the stack base and 10 page frames
      if (addr < SZ_2G && addr >= SZ_2G - 10 * PGSIZE) {

        // Check whether the valid bit is set to 0, which it should be);
        if ((tf->err & 1) == 0) {

          struct vspace* vs = &myproc()->vspace;
          struct vregion* stackRegion = &vs->regions[VR_USTACK];

          // Next check that the stack region is not yet exceeding 10 frames.
          // If so, then panic.
          uint64_t stackSize = stackRegion->size;
          if (stackSize / PGSIZE >= 10) {
            panic("stack size exceeds 10 frames");
          }

          // Allocate new page of stack for the user
          if (vregionaddmap(stackRegion, stackRegion->va_base - stackSize - PGSIZE, PGSIZE, VPI_PRESENT, VPI_WRITABLE) < 0) {
            panic("cannot allocate page for stack");
           }

          //Increase the stack vregion's size
          stackRegion->size += PGSIZE;
          vspaceinvalidate(vs);
          vspaceinstall(myproc());
          break;
        }
      }

      // Case: Handle COW Fork page faults
      // Check that the last three error bits are all 1
      if ((tf->err & 0x3) == 0x3) {
       //cprintf("went to cow\n");
        // Get the vspace, vregion, and vpage info for the current address
        struct vspace* vs = &myproc()->vspace;
        struct vregion* region = va2vregion(vs, addr);
        struct vpage_info* info = va2vpage_info(region, addr);   
        
        // if (myproc()->pid == 3) {
        //   cprintf("pause here");
        // }
        // Check if the error is due to COW
        if (info->is_cow == VPI_COW && info->original_perm == VPI_WRITABLE) {
          struct core_map_entry* cm_entry = pa2page(info->ppn << PT_SHIFT);
          lock_memory();
          if (cm_entry->ref_count > 1) {
            // If ref_count is greater than 1, then we need to make a copy
            char* page_ptr = kalloc();
            memmove(page_ptr, P2V(info->ppn << PT_SHIFT), PGSIZE);
            info->ppn = PGNUM(V2P(page_ptr));
            cm_entry->ref_count--;
          }
          unlock_memory();
          // Reset the page table info to proper setting
          info->writable = VPI_WRITABLE;
          vspaceinvalidate(vs);
          vspaceinstall(myproc());
          break;
        }
      }
    }

    // Check for kernel misbehavior
    if (myproc() == 0 || (tf->cs & 3) == 0) {
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }

    // Assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}

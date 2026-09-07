#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  backtrace();
  return 0;
}

uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;
  struct proc *p = myproc();

  if(argint(0, &interval) < 0)
    return -1;
  if(argaddr(1, &handler) < 0)
    return -1;
  p->interval = interval;
  p->handler = handler;
  p->ticks = 0;
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  memmove(p->trapframe, p->alarm_trapframe, sizeof(struct trapframe));
  p->alarm_on = 0;
  return p->trapframe->a0;
}


#ifdef LAB_PGTBL
// Report which pages in [base, base+len*PGSIZE) have been accessed,
// as a bitmask written to the user buffer at abits_addr.
int
sys_pgaccess(void)
{
  uint64 base, abits_addr;
  int len;
  uint64 abits = 0;
  struct proc *p = myproc();

  if(argaddr(0, &base) < 0)
    return -1;
  if(argint(1, &len) < 0)
    return -1;
  if(argaddr(2, &abits_addr) < 0)
    return -1;
  if(len > 64)
    return -1;

  for(int i = 0; i < len; i++){
    pte_t *pte = walk(p->pagetable, base + i * PGSIZE, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      return -1;
    if(*pte & PTE_A){
      abits |= (1L << i);
      *pte &= ~PTE_A;
    }
  }

  if(copyout(p->pagetable, abits_addr, (char *)&abits, sizeof(abits)) < 0)
    return -1;
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int mask;
  if(argint(0, &mask) < 0)
    return -1;
  myproc()->mask = mask;
  return 0;
}

uint64
sys_sysinfo(void)
{
  uint64 addr;
  struct sysinfo info;
  struct proc *p = myproc();

  if(argaddr(0, &addr) < 0)
    return -1;
  info.freemem = freemem();
  info.nproc = nproc();
  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}

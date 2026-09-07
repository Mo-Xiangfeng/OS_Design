// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
// Each CPU has its own free list and lock, to reduce lock contention;
// an empty CPU steals a page from another CPU's list.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

// Reference count of each physical page, used by copy-on-write fork.
// Manipulated only with atomic operations, so no lock is needed.
int refcount[(PHYSTOP - KERNBASE) / PGSIZE];

#define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

void
kinit()
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");
  for(int i = 0; i < NELEM(refcount); i++)
    refcount[i] = 1;
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Drop the reference count atomically; the page is really freed only
  // when the last reference goes away (copy-on-write fork shares pages).
  if(__sync_add_and_fetch(&refcount[PA2IDX(pa)], -1) > 0)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if(r == 0){
    // this CPU's free list is empty: steal a page from another CPU
    for(int i = 1; i < NCPU; i++){
      int cid = (id + i) % NCPU;
      acquire(&kmem[cid].lock);
      r = kmem[cid].freelist;
      if(r){
        kmem[cid].freelist = r->next;
        release(&kmem[cid].lock);
        break;
      }
      release(&kmem[cid].lock);
    }
  }
  pop_off();

  if(r){
    refcount[PA2IDX(r)] = 1;
    memset((char*)r, 5, PGSIZE); // fill with junk
  }
  return (void*)r;
}

// Increment the reference count of the physical page pa.
void
krefinc(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefinc");
  __sync_add_and_fetch(&refcount[PA2IDX(pa)], 1);
}

// Decrement the reference count of the physical page pa.
void
krefdec(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefdec");
  __sync_add_and_fetch(&refcount[PA2IDX(pa)], -1);
}

// Return the reference count of the physical page pa.
int
krefget(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefget");
  return __atomic_load_n(&refcount[PA2IDX(pa)], __ATOMIC_SEQ_CST);
}

// Atomically: if pa has exactly one reference, keep it and return 0;
// otherwise drop one reference (this process is about to take a private
// copy) and return 1. Used by copy-on-write fault handling so that two
// processes faulting the same shared page can't both take the copy path.
int
krefcopydec(void *pa)
{
  int old, idx;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefcopydec");
  idx = PA2IDX(pa);
  for(;;){
    old = __atomic_load_n(&refcount[idx], __ATOMIC_SEQ_CST);
    if(old == 1)
      return 0;
    if(__sync_bool_compare_and_swap(&refcount[idx], old, old - 1))
      return 1;
  }
}

uint64
freemem(void)
{
  struct run *r;
  uint64 n = 0;

  for(int i = 0; i < NCPU; i++){
    acquire(&kmem[i].lock);
    for(r = kmem[i].freelist; r; r = r->next)
      n += PGSIZE;
    release(&kmem[i].lock);
  }
  return n;
}

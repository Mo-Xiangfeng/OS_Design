// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

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
} kmem;

// Reference count of each physical page, used by copy-on-write fork.
struct {
  struct spinlock lock;
  int refcount[(PHYSTOP - KERNBASE) / PGSIZE];
} kmemref;

#define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kmemref.lock, "kmemref");
  acquire(&kmemref.lock);
  for(int i = 0; i < NELEM(kmemref.refcount); i++)
    kmemref.refcount[i] = 1;
  release(&kmemref.lock);
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

  // Drop the reference count; the page is really freed only when the
  // last reference goes away (copy-on-write fork shares pages).
  acquire(&kmemref.lock);
  if(kmemref.refcount[PA2IDX(pa)] > 1){
    kmemref.refcount[PA2IDX(pa)]--;
    release(&kmemref.lock);
    return;
  }
  kmemref.refcount[PA2IDX(pa)] = 0;
  release(&kmemref.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    acquire(&kmemref.lock);
    kmemref.refcount[PA2IDX(r)] = 1;
    release(&kmemref.lock);
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
  acquire(&kmemref.lock);
  kmemref.refcount[PA2IDX(pa)]++;
  release(&kmemref.lock);
}

// Decrement the reference count of the physical page pa.
void
krefdec(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefdec");
  acquire(&kmemref.lock);
  kmemref.refcount[PA2IDX(pa)]--;
  release(&kmemref.lock);
}

// Return the reference count of the physical page pa.
int
krefget(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefget");
  acquire(&kmemref.lock);
  int n = kmemref.refcount[PA2IDX(pa)];
  release(&kmemref.lock);
  return n;
}

// Atomically: if pa has exactly one reference, keep it and return 0;
// otherwise drop one reference (this process is about to take a private
// copy) and return 1. Used by copy-on-write fault handling so that two
// processes faulting the same shared page can't both take the copy path.
int
krefcopydec(void *pa)
{
  int shared;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefcopydec");
  acquire(&kmemref.lock);
  if(kmemref.refcount[PA2IDX(pa)] == 1){
    shared = 0;
  } else {
    kmemref.refcount[PA2IDX(pa)]--;
    shared = 1;
  }
  release(&kmemref.lock);
  return shared;
}

uint64
freemem(void)
{
  struct run *r;
  uint64 n = 0;

  acquire(&kmem.lock);
  for(r = kmem.freelist; r; r = r->next)
    n += PGSIZE;
  release(&kmem.lock);
  return n;
}

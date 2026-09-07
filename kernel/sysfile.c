//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  // follow symbolic links, with a depth limit to avoid cycles
  {
    int depth = 0;
    char target[MAXPATH];
    while(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
      if(++depth > 10){
        iunlockput(ip);
        end_op();
        return -1;
      }
      if(readi(ip, 0, (uint64)target, 0, MAXPATH) <= 0){
        iunlockput(ip);
        end_op();
        return -1;
      }
      iunlockput(ip);
      if((ip = namei(target)) == 0){
        end_op();
        return -1;
      }
      ilock(ip);
      if(ip->type == T_DIR && omode != O_RDONLY){
        iunlockput(ip);
        end_op();
        return -1;
      }
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

// Create a symbolic link: symlink(target, path).
uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;

  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();

  if((ip = create(path, T_SYMLINK, 0, 0)) == 0){
    end_op();
    return -1;
  }

  // store the target string in the symlink inode's data blocks
  if(writei(ip, 0, (uint64)target, 0, strlen(target)) != strlen(target)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
  return 0;
}

// Map the file fd at a kernel-chosen address.  The mapping is created
// lazily: no physical memory is allocated here; page faults on the
// region are handled by mmapfault().
uint64
sys_mmap(void)
{
  uint64 addr;
  int len, prot, flags, fd;
  uint64 offset;
  struct file *f;
  struct proc *p = myproc();
  struct vma *v;

  if(argaddr(0, &addr) < 0 || argint(1, &len) < 0 ||
     argint(2, &prot) < 0 || argint(3, &flags) < 0 ||
     argint(4, &fd) < 0 || argaddr(5, &offset) < 0)
    return -1;

  if(len <= 0)
    return -1;
  if(offset % PGSIZE != 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;
  // a writable MAP_SHARED mapping requires a writable file
  if((prot & PROT_WRITE) && (flags & MAP_SHARED) && !f->writable)
    return -1;

  // find a free VMA slot
  v = 0;
  for(int i = 0; i < MAXVMA; i++){
    if(p->vmas[i].f == 0){
      v = &p->vmas[i];
      break;
    }
  }
  if(v == 0)
    return -1;

  v->addr = PGROUNDUP(p->sz);   // map just above the heap
  v->len = PGROUNDUP(len);
  v->prot = prot;
  v->flags = flags;
  v->offset = offset;
  v->f = f;
  filedup(f);

  p->sz = v->addr + v->len;
  return v->addr;
}

// Handle a page fault in a lazily-mapped mmap region: allocate a page,
// fill it with the file's content, and map it.  Returns 0 on success,
// -1 if va is not inside any mapped region.
int
mmapfault(struct proc *p, uint64 va)
{
  struct vma *v;
  char *mem;
  int n;
  int pteflags;

  va = PGROUNDDOWN(va);

  for(int i = 0; i < MAXVMA; i++){
    v = &p->vmas[i];
    if(v->f == 0 || va < v->addr || va >= v->addr + v->len)
      continue;

    if((mem = kalloc()) == 0)
      return -1;
    memset(mem, 0, PGSIZE);

    // read the corresponding part of the file (readi returns fewer
    // bytes at end-of-file; the rest of the page stays zero)
    begin_op();
    ilock(v->f->ip);
    n = readi(v->f->ip, 0, (uint64)mem, v->offset + (va - v->addr), PGSIZE);
    iunlock(v->f->ip);
    end_op();
    if(n < 0){
      kfree(mem);
      return -1;
    }

    pteflags = PTE_U;
    if(v->prot & PROT_READ)
      pteflags |= PTE_R;
    if(v->prot & PROT_WRITE)
      pteflags |= PTE_W;
    if(v->prot & PROT_EXEC)
      pteflags |= PTE_X;

    if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, pteflags) != 0){
      kfree(mem);
      return -1;
    }
    return 0;
  }
  return -1;
}

// Write back one dirty shared page to its file.
static void
mmapwriteback(struct proc *p, struct vma *v, uint64 va)
{
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_D) == 0)
    return;
  uint64 pa = PTE2PA(*pte);
  begin_op();
  ilock(v->f->ip);
  writei(v->f->ip, 0, pa, v->offset + (va - v->addr), PGSIZE);
  iunlock(v->f->ip);
  end_op();
}

// Unmap pages that are actually mapped (lazy mmap regions may have
// holes), freeing their physical memory.  Never panics on holes.
static void
mmapunmap(struct proc *p, uint64 va, int npages)
{
  for(int i = 0; i < npages; i++){
    pte_t *pte = walk(p->pagetable, va + i * PGSIZE, 0);
    if(pte && (*pte & PTE_V))
      uvmunmap(p->pagetable, va + i * PGSIZE, 1, 1);
  }
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int len;
  struct proc *p = myproc();
  struct vma *v;

  if(argaddr(0, &addr) < 0 || argint(1, &len) < 0)
    return -1;
  if(len <= 0)
    return -1;
  addr = PGROUNDDOWN(addr);
  len = PGROUNDUP(len);

  for(int i = 0; i < MAXVMA; i++){
    v = &p->vmas[i];
    if(v->f == 0 || addr < v->addr || addr >= v->addr + v->len)
      continue;

    // write back dirty pages of shared mappings before unmapping
    if(v->flags & MAP_SHARED){
      for(uint64 va = addr; va < addr + len && va < v->addr + v->len;
          va += PGSIZE)
        mmapwriteback(p, v, va);
    }

    mmapunmap(p, addr, len / PGSIZE);

    if(addr <= v->addr && addr + len >= v->addr + v->len){
      // the whole region is unmapped
      fileclose(v->f);
      v->f = 0;
    } else if(addr <= v->addr){
      // unmapped the beginning of the region
      v->offset += (addr + len) - v->addr;
      v->len = v->addr + v->len - (addr + len);
      v->addr = addr + len;
    } else {
      // unmapped the end of the region
      v->len = addr - v->addr;
    }
    return 0;
  }
  return -1;
}

// Unmap and write back all of the process's mmap regions; called from
// exit() and freeproc paths.
void
vmaclear(struct proc *p)
{
  for(int i = 0; i < MAXVMA; i++){
    struct vma *v = &p->vmas[i];
    if(v->f == 0)
      continue;
    if(v->flags & MAP_SHARED){
      for(uint64 va = v->addr; va < v->addr + v->len; va += PGSIZE)
        mmapwriteback(p, v, va);
    }
    mmapunmap(p, v->addr, v->len / PGSIZE);
    fileclose(v->f);
    v->f = 0;
  }
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

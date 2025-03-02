#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

static void syscall_handler (struct intr_frame *);
static bool fit_brk_pg(uint32_t *pd, void *src_brk, void *trg_brk);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void
syscall_exit (int status)
{
  printf ("%s: exit(%d)\n", thread_current ()->name, status);
  thread_exit ();
}

/*
 * This does not check that the buffer consists of only mapped pages; it merely
 * checks the buffer exists entirely below PHYS_BASE.
 */
static void
validate_buffer_in_user_region (const void* buffer, size_t length)
{
  uintptr_t delta = PHYS_BASE - buffer;
  if (!is_user_vaddr (buffer) || length > delta)
    syscall_exit (-1);
}

/*
 * This does not check that the string consists of only mapped pages; it merely
 * checks the string exists entirely below PHYS_BASE.
 */
static void
validate_string_in_user_region (const char* string)
{
  uintptr_t delta = PHYS_BASE - (const void*) string;
  if (!is_user_vaddr (string) || strnlen (string, delta) == delta)
    syscall_exit (-1);
}


static int
syscall_open (const char* filename)
{
  struct thread* t = thread_current ();
  if (t->open_file != NULL)
    return -1;

  t->open_file = filesys_open (filename);
  if (t->open_file == NULL)
    return -1;

  return 2;
}

static int
syscall_write (int fd, void* buffer, unsigned size)
{
  struct thread* t = thread_current ();
  if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, size);
      return size;
    }
  else if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int) file_write (t->open_file, buffer, size);
}

static int
syscall_read (int fd, void* buffer, unsigned size)
{
  struct thread* t = thread_current ();
  if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int) file_read (t->open_file, buffer, size);
}

static void
syscall_close (int fd)
{
  struct thread* t = thread_current ();
  if (fd == 2 && t->open_file != NULL)
    {
      file_close (t->open_file);
      t->open_file = NULL;
    }
}

static void *
syscall_sbrk(intptr_t increment) {
  struct thread *t = thread_current();
  uint8_t *src_brk = t->brk;
  if (increment == 0) {
    return src_brk;
  }
  
  uint8_t *trg_brk = src_brk + increment;
  if (!fit_brk_pg(t->pagedir, src_brk, trg_brk)) {
    return -1;
  }
  
  t->brk = trg_brk;
  return src_brk;
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t* args = (uint32_t*) f->esp;
  struct thread* t = thread_current ();
  t->in_syscall = true;
  t->user_stack = f->esp;

  validate_buffer_in_user_region (args, sizeof(uint32_t));
  switch (args[0])
    {
    case SYS_EXIT:
      validate_buffer_in_user_region (&args[1], sizeof(uint32_t));
      syscall_exit ((int) args[1]);
      break;

    case SYS_OPEN:
      validate_buffer_in_user_region (&args[1], sizeof(uint32_t));
      validate_string_in_user_region ((char*) args[1]);
      f->eax = (uint32_t) syscall_open ((char*) args[1]);
      break;

    case SYS_WRITE:
      validate_buffer_in_user_region (&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region ((void*) args[2], (unsigned) args[3]);
      f->eax = (uint32_t) syscall_write ((int) args[1], (void*) args[2], (unsigned) args[3]);
      break;

    case SYS_READ:
      validate_buffer_in_user_region (&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region ((void*) args[2], (unsigned) args[3]);
      f->eax = (uint32_t) syscall_read ((int) args[1], (void*) args[2], (unsigned) args[3]);
      break;

    case SYS_CLOSE:
      validate_buffer_in_user_region (&args[1], sizeof(uint32_t));
      syscall_close ((int) args[1]);
      break;

    case SYS_SBRK:
      validate_buffer_in_user_region(&args[1], sizeof(intptr_t));
      f->eax = syscall_sbrk((intptr_t) args[1]);
      break;

    default:
      printf ("Unimplemented system call: %d\n", (int) args[0]);
      break;
    }

  t->in_syscall = false;
}





static bool fit_brk_pg(uint32_t *pd, void *src_brk, void *trg_brk) {
  void *src_pg = pg_round_up(src_brk);
  void *trg_pg = pg_round_up(trg_brk);
  void *kaddr;
  size_t pg_cnt;
  
  if (src_pg == trg_pg) {
    return true;
  } else if (trg_pg > src_pg) {
    // allocate page
    pg_cnt = (trg_pg - src_pg) / PGSIZE;
    kaddr = palloc_get_multiple(PAL_USER|PAL_ZERO, pg_cnt);
    if (kaddr == NULL) {
      return false;
    }
    for (size_t i = 0; i != pg_cnt; ++i) {
      if (!pagedir_set_page(pd, src_pg+(i*PGSIZE), kaddr+(i*PGSIZE), true)) {
        for (size_t j = 0; j != i; ++j) {
          pagedir_clear_page(pd, src_pg+(j*PGSIZE));
        }
        palloc_free_multiple(kaddr, pg_cnt);
        return false;
      }
    }
  } else {
    // deallocate page
    pg_cnt = (src_pg - trg_pg) / PGSIZE;
    kaddr = pagedir_get_page(pd, trg_pg);
    for (size_t i = 0; i != pg_cnt; ++i) {
      pagedir_clear_page(pd, src_pg-(i+1)*PGSIZE);
    }
    // printf("%p\n", kaddr);
    if (kaddr == NULL) {return false;}
    palloc_free_multiple(kaddr, pg_cnt);
  }
  
  return true;
}
/* Pager for ext2fs

   Copyright (C) 1994, 1995 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <strings.h>
#include "ext2fs.h"

spin_lock_t pagerlistlock = SPIN_LOCK_INITIALIZER;
struct user_pager_info *filepagerlist;

spin_lock_t node2pagelock = SPIN_LOCK_INITIALIZER;

#ifdef DONT_CACHE_MEMORY_OBJECTS
#define MAY_CACHE 0
#else
#define MAY_CACHE 1
#endif

/* Find the location on disk of page OFFSET in pager UPI.  Return the
   disk address (in disk block) in *ADDR.  If *NPLOCK is set on
   return, then release that mutex after I/O on the data has
   completed.  Set DISKSIZE to be the amount of valid data on disk.
   (If this is an unallocated block, then set *ADDR to zero.)  */
static error_t
find_address (struct user_pager_info *upi,
	      vm_address_t offset,
	      daddr_t *addr,
	      int *disksize,
	      struct rwlock **nplock)
{
  assert (upi->type == DISK || upi->type == FILE_DATA);

  if (upi->type == DISK)
    {
      *disksize = vm_page_size;
      *addr = offset / DEV_BSIZE;
      *nplock = 0;
      return 0;
    }
  else 
    {
      struct node *np = upi->np;
      
      rwlock_reader_lock (&np->dn->allocptrlock);
      *nplock = &np->dn->allocptrlock;

      if (offset >= np->allocsize)
	{
	  rwlock_reader_unlock (&np->dn->allocptrlock);
	  return EIO;
	}
      
      if (offset + vm_page_size > np->allocsize)
	*disksize = np->allocsize - offset;
      else
	*disksize = vm_page_size;

      return ext2_getblk(np, offset / block_size, 0, addr);
    }
}


/* Implement the pager_read_page callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
error_t
pager_read_page (struct user_pager_info *pager,
		 vm_offset_t page,
		 vm_address_t *buf,
		 int *writelock)
{
  error_t err;
  struct rwlock *nplock;
  daddr_t addr;
  int disksize;
  
  err = find_address (pager, page, &addr, &disksize, &nplock);
  if (err)
    return err;
  
  if (addr)
    {
      err = dev_read_sync (addr, (void *)buf, disksize);
      if (!err && disksize != vm_page_size)
	bzero ((void *)(*buf + disksize), vm_page_size - disksize);
      *writelock = 0;
    }
  else
    {
      vm_allocate (mach_task_self (), buf, vm_page_size, 1);
      *writelock = 1;
    }
      
  if (nplock)
    rwlock_reader_unlock (nplock);
  
  return err;
}

/* Implement the pager_write_page callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
error_t
pager_write_page (struct user_pager_info *pager,
		  vm_offset_t page,
		  vm_address_t buf)
{
  daddr_t addr;
  int disksize;
  struct rwlock *nplock;
  error_t err;
  
  err = find_address (pager, page, &addr, &disksize, &nplock);
  if (err)
    return err;
  
  if (addr)
    err = dev_write_sync (addr, buf, disksize);
  else
    {
      ext2_error("pager_write_page",
		 "Attempt to write unallocated disk;"
		 " object = %p; offset = 0x%x", pager, page);
      /* unallocated disk; error would be pointless */
      err = 0;
    }
    
  if (nplock)
    rwlock_reader_unlock (nplock);
  
  return err;
}

/* Implement the pager_unlock_page callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
error_t
pager_unlock_page (struct user_pager_info *pager,
		   vm_offset_t address)
{
  if (pager->type == DISK)
    return 0;
  else
    {
      error_t err;
      struct node *np = pager->np;
      struct disknode *dn = np->dn;
      int block_size_shift = EXT2_BLOCK_SIZE_BITS(sblock);

      rwlock_writer_lock (&dn->allocptrlock);
  
      /* If this is the last block, we don't let it get unlocked. */
      if (address + vm_page_size
	  > ((np->allocsize >> block_size_shift) << block_size_shift))
	{
	  ext2_error ("pager_unlock_page",
		      "attempt to unlock at last block denied\n");
	  rwlock_writer_unlock (&dn->allocptrlock);
	  return EIO;
	}

      err = diskfs_catch_exception ();
      if (!err)
	err = ext2_getblk(np, address / block_size, 1, &buf);
      diskfs_end_catch_exception ();

      rwlock_writer_unlock (&dn->allocptrlock);

      return err;
    }
}

/* Implement the pager_report_extent callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
inline error_t
pager_report_extent (struct user_pager_info *pager,
		     vm_address_t *offset,
		     vm_size_t *size)
{
  assert (pager->type == DISK || pager->type == FILE_DATA);

  *offset = 0;

  if (pager->type == DISK)
    *size = diskpagersize;
  else
    *size = pager->np->allocsize;
  
  return 0;
}

/* Implement the pager_clear_user_data callback from the pager library.
   See <hurd/pager.h> for the interface description. */
void
pager_clear_user_data (struct user_pager_info *upi)
{
  assert (upi->type == FILE_DATA);
  spin_lock (&node2pagelock);
  upi->np->dn->fileinfo = 0;
  spin_unlock (&node2pagelock);
  diskfs_nrele_light (upi->np);
  *upi->prevp = upi->next;
  if (upi->next)
    upi->next->prevp = upi->prevp;
  free (upi);
}



/* Create a the DISK pager, initializing DISKPAGER, and DISKPAGERPORT */
void
create_disk_pager ()
{
  diskpager = malloc (sizeof (struct user_pager_info));
  diskpager->type = DISK;
  diskpager->np = 0;
  diskpager->p = pager_create (diskpager, MAY_CACHE, MEMORY_OBJECT_COPY_NONE);
  diskpagerport = pager_get_port (diskpager->p);
  mach_port_insert_right (mach_task_self (), diskpagerport, diskpagerport,
			  MACH_MSG_TYPE_MAKE_SEND);
}  

/* This syncs a single file (NP) to disk.  Wait for all I/O to complete
   if WAIT is set.  NP->lock must be held.  */
void
diskfs_file_update (struct node *np, int wait)
{
  struct dirty_indir *d, *tmp;
  struct user_pager_info *upi;

  spin_lock (&node2pagelock);
  upi = np->dn->fileinfo;
  if (upi)
    pager_reference (upi->p);
  spin_unlock (&node2pagelock);
  
  if (upi)
    {
      pager_sync (upi->p, wait);
      pager_unreference (upi->p);
    }
  
  for (d = np->dn->dirty; d; d = tmp)
    {
      sync_disk_image (d->bno, block_size, wait);
      tmp = d->next;
      free (d);
    }
  np->dn->dirty = 0;

  diskfs_node_update (np, wait);
}

/* Call this to create a FILE_DATA pager and return a send right.
   NP must be locked.  */
mach_port_t
diskfs_get_filemap (struct node *np)
{
  struct user_pager_info *upi;
  mach_port_t right;

  assert (S_ISDIR (np->dn_stat.st_mode)
	  || S_ISREG (np->dn_stat.st_mode)
	  || (S_ISLNK (np->dn_stat.st_mode)
	      && (!direct_symlink_extension 
		  || np->dn_stat.st_size >= sblock->fs_maxsymlinklen)));

  spin_lock (&node2pagelock);
  if (!np->dn->fileinfo)
    {
      upi = malloc (sizeof (struct user_pager_info));
      upi->type = FILE_DATA;
      upi->np = np;
      diskfs_nref_light (np);
      upi->p = pager_create (upi, MAY_CACHE, MEMORY_OBJECT_COPY_DELAY);
      np->dn->fileinfo = upi;

      spin_lock (&pagerlistlock);
      upi->next = filepagerlist;
      upi->prevp = &filepagerlist;
      if (upi->next)
	upi->next->prevp = &upi->next;
      filepagerlist = upi;
      spin_unlock (&pagerlistlock);
    }
  right = pager_get_port (np->dn->fileinfo->p);
  spin_unlock (&node2pagelock);
  
  mach_port_insert_right (mach_task_self (), right, right,
			  MACH_MSG_TYPE_MAKE_SEND);

  return right;
} 

/* Call this when we should turn off caching so that unused memory object
   ports get freed.  */
void
drop_pager_softrefs (struct node *np)
{
  struct user_pager_info *upi;
  
  spin_lock (&node2pagelock);
  upi = np->dn->fileinfo;
  if (upi)
    pager_reference (upi->p);
  spin_unlock (&node2pagelock);

  if (MAY_CACHE && upi)
    pager_change_attributes (upi->p, 0, MEMORY_OBJECT_COPY_DELAY, 0);
  if (upi)
    pager_unreference (upi->p);
}

/* Call this when we should turn on caching because it's no longer
   important for unused memory object ports to get freed.  */
void
allow_pager_softrefs (struct node *np)
{
  struct user_pager_info *upi;
  
  spin_lock (&node2pagelock);
  upi = np->dn->fileinfo;
  if (upi)
    pager_reference (upi->p);
  spin_unlock (&node2pagelock);
  
  if (MAY_CACHE && upi)
    pager_change_attributes (upi->p, 1, MEMORY_OBJECT_COPY_DELAY, 0);
  if (upi)
    pager_unreference (upi->p);
}

/* Call this to find out the struct pager * corresponding to the
   FILE_DATA pager of inode IP.  This should be used *only* as a subsequent
   argument to register_memory_fault_area, and will be deleted when 
   the kernel interface is fixed.  NP must be locked.  */
struct pager *
diskfs_get_filemap_pager_struct (struct node *np)
{
  /* This is safe because fileinfo can't be cleared; there must be
     an active mapping for this to be called. */
  return np->dn->fileinfo->p;
}

/* Call function FUNC (which takes one argument, a pager) on each pager, with
   all file pagers being processed before the disk pager.  Make the calls
   while holding no locks. */
static void
pager_traverse (void (*func)(struct user_pager_info *))
{
  struct user_pager_info *p;
  struct item {struct item *next; struct user_pager_info *p;} *list = 0;
  struct item *i;
  
  spin_lock (&pagerlistlock);
  for (p = filepagerlist; p; p = p->next)
    {
      i = alloca (sizeof (struct item));
      i->next = list;
      list = i;
      pager_reference (p->p);
      i->p = p;
    }
  spin_unlock (&pagerlistlock);
  
  for (i = list; i; i = i->next)
    {
      (*func)(i->p);
      pager_unreference (i->p->p);
    }
  
  (*func)(diskpager);
}

/* Shutdown all the pagers. */
void
diskfs_shutdown_pager ()
{
  void shutdown_one (struct user_pager_info *p)
    {
      pager_shutdown (p->p);
    }

  copy_sblock ();
  write_all_disknodes ();
  pager_traverse (shutdown_one);
}

/* Sync all the pagers. */
void
diskfs_sync_everything (int wait)
{
  void sync_one (struct user_pager_info *p)
    {
      if (p != diskpager)
	pager_sync (p->p, wait);
      else
	sync_disk (wait);
    }
  
  copy_sblock ();
  write_all_disknodes ();
  pager_traverse (sync_one);
}
  

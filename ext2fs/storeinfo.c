/* Access to file layout information

   Copyright (C) 1996 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

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

#include <string.h>
#include <netinet/in.h>			     /* htonl */
#include <hurd/store.h>

#include "ext2fs.h"

error_t
diskfs_S_file_get_storage_info (struct protid *cred,
				mach_port_t **ports,
				mach_msg_type_name_t *ports_type,
				mach_msg_type_number_t *num_ports,
				int **ints, mach_msg_type_number_t *num_ints,
				off_t **offsets,
				mach_msg_type_number_t *num_offsets,
				char **data, mach_msg_type_number_t *data_len)
{
  error_t err = 0;
  unsigned num_fs_blocks;
  struct store *file_store;
  struct store_run *runs, *run = 0;
  block_t index = 0;
  size_t num_runs = 0, runs_alloced = 10;
  struct node *node = cred->po->np;

  runs = malloc (runs_alloced * sizeof (struct store_run));
  if (! runs)
    return ENOMEM;

  mutex_lock (&node->lock);

  /* NUM_FS_BLOCKS counts down the blocks in the file that we've not
     enumerated yet; when it hits zero, we can stop.  */
  num_fs_blocks = node->dn_stat.st_blocks >> log2_stat_blocks_per_fs_block;
  while (num_fs_blocks-- > 0)
    {
      block_t block;

      err = ext2_getblk (node, index++, 0, &block);
      if (err == EINVAL)
	/* Either a hole, or past the end of the file.  */
	{
	  block = 0;
	  err = 0;
	}
      else if (err)
	break;

      block <<= log2_dev_blocks_per_fs_block;
      if (num_runs == 0
	  || ((block && run->start >= 0) /* Neither is a hole and... */
	      ? (block != run->start + run->length) /* BLOCK doesn't follow RUN */
	      : (block || run->start >= 0))) /* or one is, but not both */
	/* Add a new run.  */
	{
	  if (num_runs == runs_alloced)
	    /* Make some more space in RUNS.  */
	    {
	      struct store_run *new;
	      runs_alloced *= 2;
	      new = realloc (runs, runs_alloced * sizeof (struct store_run));
	      if (! new)
		{
		  err = ENOMEM;
		  break;
		}
	      runs = new;
	    }

	  run = runs + num_runs++;
	  run->start = block ?: -1;	     /* -1 means a hole in OFFSETS */
	  run->length = 0;		     /* will get extended just below */
	}

      /* Increase the size of the current run by one filesystem block.  */
      run->length += 1 << log2_dev_blocks_per_fs_block;
    }

  mutex_unlock (&node->lock);

  if (! err)
    err = store_clone (store, &file_store);
  if (! err)
    {
      err = store_remap (file_store, runs, num_runs, &file_store);
      if (!err
	  && !diskfs_isuid (0, cred)
	  && !store_is_securely_returnable (file_store, cred->po->openstat))
	{
	  err = store_set_flags (file_store, STORE_INACTIVE);
	  if (err == EINVAL)
	    err = EACCES;
	}
      if (! err)
	err = store_return (file_store, ports, num_ports, ints, num_ints,
			    offsets, num_offsets, data, data_len);
      *ports_type = MACH_MSG_TYPE_MAKE_SEND;
      store_free (file_store);
    }

  free (runs);

  return err;
}

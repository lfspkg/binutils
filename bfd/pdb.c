/* BFD back-end for PDB Multi-Stream Format archives.
   Copyright (C) 2022-2023 Free Software Foundation, Inc.

   This file is part of BFD, the Binary File Descriptor library.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */

/* This describes the MSF file archive format, which is used for the
   PDB debug info generated by MSVC. See https://llvm.org/docs/PDB/MsfFile.html
   for a full description of the format.  */

#include "sysdep.h"
#include "bfd.h"
#include "libbfd.h"

/* "Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\0\0\0" */
static const uint8_t pdb_magic[] =
{ 0x4d, 0x69, 0x63, 0x72, 0x6f, 0x73, 0x6f, 0x66,
  0x74, 0x20, 0x43, 0x2f, 0x43, 0x2b, 0x2b, 0x20,
  0x4d, 0x53, 0x46, 0x20, 0x37, 0x2e, 0x30, 0x30,
  0x0d, 0x0a, 0x1a, 0x44, 0x53, 0x00, 0x00, 0x00 };

#define arch_eltdata(bfd) ((struct areltdata *) ((bfd)->arelt_data))

static bfd_cleanup
pdb_archive_p (bfd *abfd)
{
  int ret;
  char magic[sizeof (pdb_magic)];

  ret = bfd_bread (magic, sizeof (magic), abfd);
  if (ret != sizeof (magic))
    {
      bfd_set_error (bfd_error_wrong_format);
      return NULL;
    }

  if (memcmp (magic, pdb_magic, sizeof (magic)))
    {
      bfd_set_error (bfd_error_wrong_format);
      return NULL;
    }

  void *tdata = bfd_zalloc (abfd, sizeof (struct artdata));
  if (tdata == NULL)
    return NULL;
  bfd_ardata (abfd) = tdata;

  return _bfd_no_cleanup;
}

static bfd *
pdb_get_elt_at_index (bfd *abfd, symindex sym_index)
{
  char int_buf[sizeof (uint32_t)];
  uint32_t block_size, block_map_addr, block, num_files;
  uint32_t first_dir_block, dir_offset, file_size, block_off, left;
  char name[10];
  bfd *file;
  char *buf;

  /* Get block_size.  */

  if (bfd_seek (abfd, sizeof (pdb_magic), SEEK_SET))
    return NULL;

  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    {
      bfd_set_error (bfd_error_malformed_archive);
      return NULL;
    }

  block_size = bfd_getl32 (int_buf);
  if ((block_size & -block_size) != block_size
      || block_size < 512
      || block_size > 4096)
    {
      bfd_set_error (bfd_error_malformed_archive);
      return NULL;
    }

  /* Get block_map_addr.  */

  if (bfd_seek (abfd, 4 * sizeof (uint32_t), SEEK_CUR))
    return NULL;

  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    {
      bfd_set_error (bfd_error_malformed_archive);
      return NULL;
    }

  block_map_addr = bfd_getl32 (int_buf);

  /* Get num_files.  */

  if (bfd_seek (abfd, block_map_addr * block_size, SEEK_SET))
    return NULL;

  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    {
      bfd_set_error (bfd_error_malformed_archive);
      return NULL;
    }

  first_dir_block = bfd_getl32 (int_buf);

  if (bfd_seek (abfd, first_dir_block * block_size, SEEK_SET))
    return NULL;

  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    {
      bfd_set_error (bfd_error_malformed_archive);
      return NULL;
    }

  num_files = bfd_getl32 (int_buf);

  if (sym_index >= num_files)
    {
      bfd_set_error (bfd_error_no_more_archived_files);
      return NULL;
    }

  /* Read file size.  */

  dir_offset = sizeof (uint32_t) * (sym_index + 1);

  if (dir_offset >= block_size)
    {
      uint32_t block_map_addr_off;

      block_map_addr_off = ((dir_offset / block_size) * sizeof (uint32_t));

      if (bfd_seek (abfd, (block_map_addr * block_size) + block_map_addr_off,
		    SEEK_SET))
	return NULL;

      if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
	{
	  bfd_set_error (bfd_error_malformed_archive);
	  return NULL;
	}

      block = bfd_getl32 (int_buf);
    }
  else
    {
      block = first_dir_block;
    }

  if (bfd_seek (abfd, (block * block_size) + (dir_offset % block_size),
		SEEK_SET))
    return NULL;

  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    {
      bfd_set_error (bfd_error_malformed_archive);
      return NULL;
    }

  file_size = bfd_getl32 (int_buf);

  /* Undocumented? Seen on PDBs created by MSVC 2022.  */
  if (file_size == 0xffffffff)
    file_size = 0;

  /* Create BFD. */

  /* Four hex digits is enough - even though MSF allows for 32 bits, the
     PDB format itself only uses 16 bits for stream numbers.  */
  sprintf (name, "%04lx", sym_index);

  file = bfd_create (name, abfd);

  if (!file)
    return NULL;

  if (!bfd_make_writable (file))
    goto fail;

  file->arelt_data =
    (struct areltdata *) bfd_zmalloc (sizeof (struct areltdata));

  if (!file->arelt_data)
    goto fail;

  arch_eltdata (file)->parsed_size = file_size;
  arch_eltdata (file)->key = sym_index;

  if (file_size == 0)
    return file;

  block_off = 0;

  /* Sum number of blocks in previous files.  */

  if (sym_index != 0)
    {
      dir_offset = sizeof (uint32_t);

      if (bfd_seek (abfd, (first_dir_block * block_size) + sizeof (uint32_t),
		    SEEK_SET))
	goto fail;

      for (symindex i = 0; i < sym_index; i++)
	{
	  uint32_t size, num_blocks;

	  if ((dir_offset % block_size) == 0)
	    {
	      uint32_t block_map_addr_off;

	      block_map_addr_off =
		((dir_offset / block_size) * sizeof (uint32_t));

	      if (bfd_seek
		  (abfd, (block_map_addr * block_size) + block_map_addr_off,
		   SEEK_SET))
		goto fail;

	      if (bfd_bread (int_buf, sizeof (uint32_t), abfd) !=
		  sizeof (uint32_t))
		{
		  bfd_set_error (bfd_error_malformed_archive);
		  goto fail;
		}

	      block = bfd_getl32 (int_buf);

	      if (bfd_seek (abfd, block * block_size, SEEK_SET))
		goto fail;
	    }

	  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) !=
	      sizeof (uint32_t))
	    {
	      bfd_set_error (bfd_error_malformed_archive);
	      goto fail;
	    }

	  size = bfd_getl32 (int_buf);

	  if (size == 0xffffffff)
	    size = 0;

	  num_blocks = (size + block_size - 1) / block_size;
	  block_off += num_blocks;

	  dir_offset += sizeof (uint32_t);
	}
    }

  /* Read blocks, and write into new BFD.  */

  dir_offset = sizeof (uint32_t) * (num_files + block_off + 1);

  if (dir_offset >= block_size)
    {
      uint32_t block_map_addr_off;

      block_map_addr_off = ((dir_offset / block_size) * sizeof (uint32_t));

      if (bfd_seek (abfd, (block_map_addr * block_size) + block_map_addr_off,
		    SEEK_SET))
	goto fail;

      if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
	{
	  bfd_set_error (bfd_error_malformed_archive);
	  goto fail;
	}

      block = bfd_getl32 (int_buf);
    }
  else
    {
      block = first_dir_block;
    }

  buf = bfd_malloc (block_size);
  if (!buf)
    goto fail;

  left = file_size;
  do
    {
      uint32_t file_block, to_read;

      if ((dir_offset % block_size) == 0 && left != file_size)
	{
	  uint32_t block_map_addr_off;

	  block_map_addr_off =
	    ((dir_offset / block_size) * sizeof (uint32_t));

	  if (bfd_seek
	      (abfd, (block_map_addr * block_size) + block_map_addr_off,
	       SEEK_SET))
	    goto fail2;

	  if (bfd_bread (int_buf, sizeof (uint32_t), abfd) !=
	      sizeof (uint32_t))
	    {
	      bfd_set_error (bfd_error_malformed_archive);
	      goto fail2;
	    }

	  block = bfd_getl32 (int_buf);
	}

      if (bfd_seek (abfd, (block * block_size) + (dir_offset % block_size),
		    SEEK_SET))
	goto fail2;

      if (bfd_bread (int_buf, sizeof (uint32_t), abfd) != sizeof (uint32_t))
	{
	  bfd_set_error (bfd_error_malformed_archive);
	  goto fail2;
	}

      file_block = bfd_getl32 (int_buf);

      if (bfd_seek (abfd, file_block * block_size, SEEK_SET))
	goto fail2;

      to_read = left > block_size ? block_size : left;

      if (bfd_bread (buf, to_read, abfd) != to_read)
	{
	  bfd_set_error (bfd_error_malformed_archive);
	  goto fail2;
	}

      if (bfd_bwrite (buf, to_read, file) != to_read)
	goto fail2;

      if (left > block_size)
	left -= block_size;
      else
	break;

      dir_offset += sizeof (uint32_t);
    }
  while (left > 0);

  free (buf);

  return file;

fail2:
  free (buf);

fail:
  bfd_close (file);
  return NULL;
}

static bfd *
pdb_openr_next_archived_file (bfd *archive, bfd *last_file)
{
  if (!last_file)
    return pdb_get_elt_at_index (archive, 0);
  else
    return pdb_get_elt_at_index (archive, arch_eltdata (last_file)->key + 1);
}

static int
pdb_generic_stat_arch_elt (bfd *abfd, struct stat *buf)
{
  buf->st_mtime = 0;
  buf->st_uid = 0;
  buf->st_gid = 0;
  buf->st_mode = 0644;
  buf->st_size = arch_eltdata (abfd)->parsed_size;

  return 0;
}

static uint32_t
pdb_allocate_block (uint32_t *num_blocks, uint32_t block_size)
{
  uint32_t block;

  block = *num_blocks;

  (*num_blocks)++;

  /* If new interval, skip two blocks for free space map.  */

  if ((block % block_size) == 1)
    {
      block += 2;
      (*num_blocks) += 2;
    }

  return block;
}

static bool
pdb_write_directory (bfd *abfd, uint32_t block_size, uint32_t num_files,
		     uint32_t block_map_addr, uint32_t * num_blocks)
{
  char tmp[sizeof (uint32_t)];
  uint32_t block, left, block_map_off;
  bfd *arelt;
  char *buf;

  /* Allocate first block for directory.  */

  block = pdb_allocate_block (num_blocks, block_size);
  left = block_size;

  /* Write allocated block no. at beginning of block map.  */

  if (bfd_seek (abfd, block_map_addr * block_size, SEEK_SET))
    return false;

  bfd_putl32 (block, tmp);

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  block_map_off = sizeof (uint32_t);

  /* Write num_files at beginning of directory.  */

  if (bfd_seek (abfd, block * block_size, SEEK_SET))
    return false;

  bfd_putl32 (num_files, tmp);

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  left -= sizeof (uint32_t);

  /* Write file sizes.  */

  arelt = abfd->archive_head;
  while (arelt)
    {
      if (left == 0)
	{
	  if (block_map_off == block_size) /* Too many blocks.  */
	    {
	      bfd_set_error (bfd_error_invalid_operation);
	      return false;
	    }

	  block = pdb_allocate_block (num_blocks, block_size);
	  left = block_size;

	  if (bfd_seek
	      (abfd, (block_map_addr * block_size) + block_map_off, SEEK_SET))
	    return false;

	  bfd_putl32 (block, tmp);

	  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
	    return false;

	  block_map_off += sizeof (uint32_t);

	  if (bfd_seek (abfd, block * block_size, SEEK_SET))
	    return false;
	}

      bfd_putl32 (bfd_get_size (arelt), tmp);

      if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
	return false;

      left -= sizeof (uint32_t);

      arelt = arelt->archive_next;
    }

  /* Write blocks.  */

  buf = bfd_malloc (block_size);
  if (!buf)
    return false;

  arelt = abfd->archive_head;
  while (arelt)
    {
      ufile_ptr size = bfd_get_size (arelt);
      uint32_t req_blocks = (size + block_size - 1) / block_size;

      if (bfd_seek (arelt, 0, SEEK_SET))
	{
	  free (buf);
	  return false;
	}

      for (uint32_t i = 0; i < req_blocks; i++)
	{
	  uint32_t file_block, to_read;

	  if (left == 0)
	    {
	      if (block_map_off == block_size) /* Too many blocks.  */
		{
		  bfd_set_error (bfd_error_invalid_operation);
		  free (buf);
		  return false;
		}

	      block = pdb_allocate_block (num_blocks, block_size);
	      left = block_size;

	      if (bfd_seek
		  (abfd, (block_map_addr * block_size) + block_map_off,
		   SEEK_SET))
		{
		  free (buf);
		  return false;
		}

	      bfd_putl32 (block, tmp);

	      if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) !=
		  sizeof (uint32_t))
		{
		  free (buf);
		  return false;
		}

	      block_map_off += sizeof (uint32_t);

	      if (bfd_seek (abfd, block * block_size, SEEK_SET))
		{
		  free (buf);
		  return false;
		}
	    }

	  /* Allocate block and write number into directory.  */

	  file_block = pdb_allocate_block (num_blocks, block_size);

	  bfd_putl32 (file_block, tmp);

	  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
	    {
	      free (buf);
	      return false;
	    }

	  left -= sizeof (uint32_t);

	  /* Read file contents into buffer.  */

	  to_read = size > block_size ? block_size : size;

	  if (bfd_bread (buf, to_read, arelt) != to_read)
	    {
	      free (buf);
	      return false;
	    }

	  size -= to_read;

	  if (to_read < block_size)
	    memset (buf + to_read, 0, block_size - to_read);

	  if (bfd_seek (abfd, file_block * block_size, SEEK_SET))
	    {
	      free (buf);
	      return false;
	    }

	  /* Write file contents into allocated block.  */

	  if (bfd_bwrite (buf, block_size, abfd) != block_size)
	    {
	      free (buf);
	      return false;
	    }

	  if (bfd_seek
	      (abfd, (block * block_size) + block_size - left, SEEK_SET))
	    {
	      free (buf);
	      return false;
	    }
	}

      arelt = arelt->archive_next;
    }

  memset (buf, 0, left);

  if (bfd_bwrite (buf, left, abfd) != left)
    {
      free (buf);
      return false;
    }

  free (buf);

  return true;
}

static bool
pdb_write_bitmap (bfd *abfd, uint32_t block_size, uint32_t num_blocks)
{
  char *buf;
  uint32_t num_intervals = (num_blocks + block_size - 1) / block_size;

  buf = bfd_malloc (block_size);
  if (!buf)
    return false;

  num_blocks--;			/* Superblock not included.  */

  for (uint32_t i = 0; i < num_intervals; i++)
    {
      if (bfd_seek (abfd, ((i * block_size) + 1) * block_size, SEEK_SET))
	{
	  free (buf);
	  return false;
	}

      /* All of our blocks are contiguous, making our free block map simple.
         0 = used, 1 = free.  */

      if (num_blocks >= 8)
	memset (buf, 0,
		(num_blocks / 8) >
		block_size ? block_size : (num_blocks / 8));

      if (num_blocks < block_size * 8)
	{
	  unsigned int off = num_blocks / 8;

	  if (num_blocks % 8)
	    {
	      buf[off] = (1 << (8 - (num_blocks % 8))) - 1;
	      off++;
	    }

	  if (off < block_size)
	    memset (buf + off, 0xff, block_size - off);
	}

      if (num_blocks < block_size * 8)
	num_blocks = 0;
      else
	num_blocks -= block_size * 8;

      if (bfd_bwrite (buf, block_size, abfd) != block_size)
	return false;
    }

  free (buf);

  return true;
}

static bool
pdb_write_contents (bfd *abfd)
{
  char tmp[sizeof (uint32_t)];
  const uint32_t block_size = 0x400;
  uint32_t block_map_addr;
  uint32_t num_blocks;
  uint32_t num_files = 0;
  uint32_t num_directory_bytes = sizeof (uint32_t);
  bfd *arelt;

  if (bfd_bwrite (pdb_magic, sizeof (pdb_magic), abfd) != sizeof (pdb_magic))
    return false;

  bfd_putl32 (block_size, tmp);

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  bfd_putl32 (1, tmp); /* Free block map block (always either 1 or 2).  */

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  arelt = abfd->archive_head;

  while (arelt)
    {
      uint32_t blocks_required =
	(bfd_get_size (arelt) + block_size - 1) / block_size;

      num_directory_bytes += sizeof (uint32_t); /* Size.  */
      num_directory_bytes += blocks_required * sizeof (uint32_t); /* Blocks.  */

      num_files++;

      arelt = arelt->archive_next;
    }

  /* Superblock plus two bitmap blocks.  */
  num_blocks = 3;

  /* Skip num_blocks for now.  */
  if (bfd_seek (abfd, sizeof (uint32_t), SEEK_CUR))
    return false;

  bfd_putl32 (num_directory_bytes, tmp);

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  /* Skip unknown uint32_t (always 0?).  */
  if (bfd_seek (abfd, sizeof (uint32_t), SEEK_CUR))
    return false;

  block_map_addr = pdb_allocate_block (&num_blocks, block_size);

  bfd_putl32 (block_map_addr, tmp);

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  if (!pdb_write_directory
      (abfd, block_size, num_files, block_map_addr, &num_blocks))
    return false;

  if (!pdb_write_bitmap (abfd, block_size, num_blocks))
    return false;

  /* Write num_blocks now we know it.  */

  if (bfd_seek
      (abfd, sizeof (pdb_magic) + sizeof (uint32_t) + sizeof (uint32_t),
       SEEK_SET))
    return false;

  bfd_putl32 (num_blocks, tmp);

  if (bfd_bwrite (tmp, sizeof (uint32_t), abfd) != sizeof (uint32_t))
    return false;

  return true;
}

#define pdb_bfd_free_cached_info _bfd_generic_bfd_free_cached_info
#define pdb_new_section_hook _bfd_generic_new_section_hook
#define pdb_get_section_contents _bfd_generic_get_section_contents
#define pdb_get_section_contents_in_window _bfd_generic_get_section_contents_in_window
#define pdb_close_and_cleanup _bfd_generic_close_and_cleanup

#define pdb_slurp_armap _bfd_noarchive_slurp_armap
#define pdb_slurp_extended_name_table _bfd_noarchive_slurp_extended_name_table
#define pdb_construct_extended_name_table _bfd_noarchive_construct_extended_name_table
#define pdb_truncate_arname _bfd_noarchive_truncate_arname
#define pdb_write_armap _bfd_noarchive_write_armap
#define pdb_read_ar_hdr _bfd_noarchive_read_ar_hdr
#define pdb_write_ar_hdr _bfd_noarchive_write_ar_hdr
#define pdb_update_armap_timestamp _bfd_noarchive_update_armap_timestamp

const bfd_target pdb_vec =
{
  "pdb",
  bfd_target_unknown_flavour,
  BFD_ENDIAN_LITTLE,		/* target byte order */
  BFD_ENDIAN_LITTLE,		/* target headers byte order */
  0,				/* object flags */
  0,				/* section flags */
  0,				/* leading underscore */
  ' ',				/* ar_pad_char */
  16,				/* ar_max_namelen */
  0,				/* match priority.  */
  TARGET_KEEP_UNUSED_SECTION_SYMBOLS, /* keep unused section symbols.  */
  bfd_getl64, bfd_getl_signed_64, bfd_putl64,
  bfd_getl32, bfd_getl_signed_32, bfd_putl32,
  bfd_getl16, bfd_getl_signed_16, bfd_putl16, /* Data.  */
  bfd_getl64, bfd_getl_signed_64, bfd_putl64,
  bfd_getl32, bfd_getl_signed_32, bfd_putl32,
  bfd_getl16, bfd_getl_signed_16, bfd_putl16, /* Hdrs.  */

  {				/* bfd_check_format */
    _bfd_dummy_target,
    _bfd_dummy_target,
    pdb_archive_p,
    _bfd_dummy_target
  },
  {				/* bfd_set_format */
    _bfd_bool_bfd_false_error,
    _bfd_bool_bfd_false_error,
    _bfd_bool_bfd_true,
    _bfd_bool_bfd_false_error
  },
  {				/* bfd_write_contents */
    _bfd_bool_bfd_true,
    _bfd_bool_bfd_false_error,
    pdb_write_contents,
    _bfd_bool_bfd_false_error
  },

  BFD_JUMP_TABLE_GENERIC (pdb),
  BFD_JUMP_TABLE_COPY (_bfd_generic),
  BFD_JUMP_TABLE_CORE (_bfd_nocore),
  BFD_JUMP_TABLE_ARCHIVE (pdb),
  BFD_JUMP_TABLE_SYMBOLS (_bfd_nosymbols),
  BFD_JUMP_TABLE_RELOCS (_bfd_norelocs),
  BFD_JUMP_TABLE_WRITE (_bfd_generic),
  BFD_JUMP_TABLE_LINK (_bfd_nolink),
  BFD_JUMP_TABLE_DYNAMIC (_bfd_nodynamic),

  NULL,

  NULL
};

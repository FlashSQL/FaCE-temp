/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0dblwr.cc
Doublwrite buffer module

Created 2011/12/19
*******************************************************/

#include "buf0dblwr.h"

#ifdef UNIV_NONINL
#include "buf0buf.ic"
#endif

#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "trx0sys.h"

#ifndef UNIV_HOTBACKUP

#ifdef UNIV_PFS_MUTEX
/* Key to register the mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	buf_dblwr_mutex_key;
#endif /* UNIV_PFS_RWLOCK */

/** The doublewrite buffer */
UNIV_INTERN buf_dblwr_t*	buf_dblwr = NULL;

/** Set to TRUE when the doublewrite buffer is being created */
UNIV_INTERN ibool	buf_dblwr_being_created = FALSE;

/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
UNIV_INTERN
ibool
buf_dblwr_page_inside(
/*==================*/
	ulint	page_no)	/*!< in: page number */
{
	if (buf_dblwr == NULL) {

		return(FALSE);
	}

	if (page_no >= buf_dblwr->block1
	    && page_no < buf_dblwr->block1
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	if (page_no >= buf_dblwr->block2
	    && page_no < buf_dblwr->block2
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	return(FALSE);
}

/****************************************************************//**
Calls buf_page_get() on the TRX_SYS_PAGE and returns a pointer to the
doublewrite buffer within it.
@return	pointer to the doublewrite buffer within the filespace header
page. */
UNIV_INLINE
byte*
buf_dblwr_get(
/*==========*/
	mtr_t*	mtr)	/*!< in/out: MTR to hold the page latch */
{
	buf_block_t*	block;

	block = buf_page_get(TRX_SYS_SPACE, 0, TRX_SYS_PAGE_NO,
			     RW_X_LATCH, mtr);
	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	return(buf_block_get_frame(block) + TRX_SYS_DOUBLEWRITE);
}

/********************************************************************//**
Flush a batch of writes to the datafiles that have already been
written to the dblwr buffer on disk. */
UNIV_INLINE
void
buf_dblwr_sync_datafiles()
/*======================*/
{
	/* Wake possible simulated aio thread to actually post the
	writes to the operating system */
	os_aio_simulated_wake_handler_threads();

	/* Wait that all async writes to tablespaces have been posted to
	the OS */
	os_aio_wait_until_no_pending_writes();

	/* Now we flush the data to disk (for example, with fsync) */
	fil_flush_file_spaces(FIL_TABLESPACE);
}

/****************************************************************//**
Creates or initialializes the doublewrite buffer at a database start. */
static
void
buf_dblwr_init(
/*===========*/
	byte*	doublewrite)	/*!< in: pointer to the doublewrite buf
				header on trx sys page */
{
	ulint	buf_size;

	buf_dblwr = static_cast<buf_dblwr_t*>(
		mem_zalloc(sizeof(buf_dblwr_t)));

	/* There are two blocks of same size in the doublewrite
	buffer. */
	buf_size = 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;

	/* There must be atleast one buffer for single page writes
	and one buffer for batch writes. */
	ut_a(srv_doublewrite_batch_size > 0
	     && srv_doublewrite_batch_size < buf_size);

	mutex_create(buf_dblwr_mutex_key,
		     &buf_dblwr->mutex, SYNC_DOUBLEWRITE);

	buf_dblwr->b_event = os_event_create();
	buf_dblwr->s_event = os_event_create();

	buf_dblwr->first_free = 0;
	buf_dblwr->s_reserved = 0;
	buf_dblwr->b_reserved = 0;

	buf_dblwr->block1 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK1);
	buf_dblwr->block2 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK2);

	buf_dblwr->in_use = static_cast<bool*>(
		mem_zalloc(buf_size * sizeof(bool)));

	buf_dblwr->write_buf_unaligned = static_cast<byte*>(
		ut_malloc((1 + buf_size) * UNIV_PAGE_SIZE));

	buf_dblwr->write_buf = static_cast<byte*>(
		ut_align(buf_dblwr->write_buf_unaligned,
			 UNIV_PAGE_SIZE));

	buf_dblwr->buf_block_arr = static_cast<buf_page_t**>(
		mem_zalloc(buf_size * sizeof(void*)));
}

/****************************************************************//**
Creates the doublewrite buffer to a new InnoDB installation. The header of the
doublewrite buffer is placed on the trx system header page. */
UNIV_INTERN
void
buf_dblwr_create(void)
/*==================*/
{
	buf_block_t*	block2;
	buf_block_t*	new_block;
	byte*	doublewrite;
	byte*	fseg_header;
	ulint	page_no;
	ulint	prev_page_no;
	ulint	i;
	mtr_t	mtr;

	if (buf_dblwr) {
		/* Already inited */

		return;
	}

start_again:
	mtr_start(&mtr);
	buf_dblwr_being_created = TRUE;

	doublewrite = buf_dblwr_get(&mtr);

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has already been created:
		just read in some numbers */

		buf_dblwr_init(doublewrite);

		mtr_commit(&mtr);
		buf_dblwr_being_created = FALSE;
		return;
	}

	ib_logf(IB_LOG_LEVEL_INFO,
		"Doublewrite buffer not found: creating new");

	if (buf_pool_get_curr_size()
	    < ((2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		+ FSP_EXTENT_SIZE / 2 + 100)
	       * UNIV_PAGE_SIZE)) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create doublewrite buffer: you must "
			"increase your buffer pool size. Cannot continue "
			"operation.");

		exit(EXIT_FAILURE);
	}

	block2 = fseg_create(TRX_SYS_SPACE, TRX_SYS_PAGE_NO,
			     TRX_SYS_DOUBLEWRITE
			     + TRX_SYS_DOUBLEWRITE_FSEG, &mtr);

	/* fseg_create acquires a second latch on the page,
	therefore we must declare it: */

	buf_block_dbg_add_level(block2, SYNC_NO_ORDER_CHECK);

	if (block2 == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create doublewrite buffer: you must "
			"increase your tablespace size. "
			"Cannot continue operation.");

		/* We exit without committing the mtr to prevent
		its modifications to the database getting to disk */

		exit(EXIT_FAILURE);
	}

	fseg_header = doublewrite + TRX_SYS_DOUBLEWRITE_FSEG;
	prev_page_no = 0;

	for (i = 0; i < 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		     + FSP_EXTENT_SIZE / 2; i++) {
		new_block = fseg_alloc_free_page(
			fseg_header, prev_page_no + 1, FSP_UP, &mtr);
		if (new_block == NULL) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Cannot create doublewrite buffer: you must "
				"increase your tablespace size. "
				"Cannot continue operation.");

			exit(EXIT_FAILURE);
		}

		/* We read the allocated pages to the buffer pool;
		when they are written to disk in a flush, the space
		id and page number fields are also written to the
		pages. When we at database startup read pages
		from the doublewrite buffer, we know that if the
		space id and page number in them are the same as
		the page position in the tablespace, then the page
		has not been written to in doublewrite. */

		ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
		page_no = buf_block_get_page_no(new_block);

		if (i == FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == FSP_EXTENT_SIZE);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_BLOCK1,
					 page_no, MLOG_4BYTES, &mtr);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_REPEAT
					 + TRX_SYS_DOUBLEWRITE_BLOCK1,
					 page_no, MLOG_4BYTES, &mtr);

		} else if (i == FSP_EXTENT_SIZE / 2
			   + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
			ut_a(page_no == 2 * FSP_EXTENT_SIZE);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_BLOCK2,
					 page_no, MLOG_4BYTES, &mtr);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_REPEAT
					 + TRX_SYS_DOUBLEWRITE_BLOCK2,
					 page_no, MLOG_4BYTES, &mtr);

		} else if (i > FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == prev_page_no + 1);
		}

		if (((i + 1) & 15) == 0) {
			/* rw_locks can only be recursively x-locked
			2048 times. (on 32 bit platforms,
			(lint) 0 - (X_LOCK_DECR * 2049)
			is no longer a negative number, and thus
			lock_word becomes like a shared lock).
			For 4k page size this loop will
			lock the fseg header too many times. Since
			this code is not done while any other threads
			are active, restart the MTR occasionally. */
			mtr_commit(&mtr);
			mtr_start(&mtr);
			doublewrite = buf_dblwr_get(&mtr);
			fseg_header = doublewrite
				      + TRX_SYS_DOUBLEWRITE_FSEG;
		}

		prev_page_no = page_no;
	}

	mlog_write_ulint(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC,
			 TRX_SYS_DOUBLEWRITE_MAGIC_N,
			 MLOG_4BYTES, &mtr);
	mlog_write_ulint(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC
			 + TRX_SYS_DOUBLEWRITE_REPEAT,
			 TRX_SYS_DOUBLEWRITE_MAGIC_N,
			 MLOG_4BYTES, &mtr);

	mlog_write_ulint(doublewrite
			 + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED,
			 TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N,
			 MLOG_4BYTES, &mtr);
	mtr_commit(&mtr);

	/* Flush the modified pages to disk and make a checkpoint */
	log_make_checkpoint_at(LSN_MAX, TRUE);

	/* Remove doublewrite pages from LRU */
	buf_pool_invalidate();

	ib_logf(IB_LOG_LEVEL_INFO, "Doublewrite buffer created");

	goto start_again;
}

/****************************************************************//**
At a database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function loads the pages from double write buffer into memory. */
void
buf_dblwr_init_or_load_pages(
/*=========================*/
	os_file_t	file,
	char*		path,
	bool		load_corrupt_pages)
{
	byte*	buf;
	byte*	read_buf;
	byte*	unaligned_read_buf;
	ulint	block1;
	ulint	block2;
	byte*	page;
	ibool	reset_space_ids = FALSE;
	byte*	doublewrite;
	ulint	space_id;
	ulint	i;
        ulint	block_bytes = 0;
	recv_dblwr_t& recv_dblwr = recv_sys->dblwr;

	/* We do the file i/o past the buffer pool */

	unaligned_read_buf = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));

	/* Read the trx sys header to check if we are using the doublewrite
	buffer */
	off_t  trx_sys_page = TRX_SYS_PAGE_NO * UNIV_PAGE_SIZE;
	os_file_read(file, read_buf, trx_sys_page, UNIV_PAGE_SIZE);

	doublewrite = read_buf + TRX_SYS_DOUBLEWRITE;

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has been created */

		buf_dblwr_init(doublewrite);

		block1 = buf_dblwr->block1;
		block2 = buf_dblwr->block2;

		buf = buf_dblwr->write_buf;
	} else {
		goto leave_func;
	}

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED)
	    != TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N) {

		/* We are upgrading from a version < 4.1.x to a version where
		multiple tablespaces are supported. We must reset the space id
		field in the pages in the doublewrite buffer because starting
		from this version the space id is stored to
		FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID. */

		reset_space_ids = TRUE;

		ib_logf(IB_LOG_LEVEL_INFO,
			"Resetting space id's in the doublewrite buffer");
	}

	/* Read the pages from the doublewrite buffer to memory */

        block_bytes = TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;

	os_file_read(file, buf, block1 * UNIV_PAGE_SIZE, block_bytes);
	os_file_read(file, buf + block_bytes, block2 * UNIV_PAGE_SIZE,
		     block_bytes);

	/* Check if any of these pages is half-written in data files, in the
	intended position */

	page = buf;

	for (i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * 2; i++) {

		ulint source_page_no;

		if (reset_space_ids) {

			space_id = 0;
			mach_write_to_4(page
					+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);
			/* We do not need to calculate new checksums for the
			pages because the field .._SPACE_ID does not affect
			them. Write the page back to where we read it from. */

			if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
				source_page_no = block1 + i;
			} else {
				source_page_no = block2
					+ i - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			}

			os_file_write(path, file, page,
				      source_page_no * UNIV_PAGE_SIZE,
				      UNIV_PAGE_SIZE);

		} else if (load_corrupt_pages) {

			recv_dblwr.add(page);
		}

		page += UNIV_PAGE_SIZE;
	}

	if (reset_space_ids) {
		os_file_flush(file);
	}

leave_func:
	ut_free(unaligned_read_buf);
}

/****************************************************************//**
Process the double write buffer pages. */
void
buf_dblwr_process()
/*===============*/
{
	ulint	space_id;
	ulint	page_no;
	ulint	page_no_dblwr = 0;
	byte*	page;
	byte*	read_buf;
	byte*	unaligned_read_buf;
	recv_dblwr_t& recv_dblwr = recv_sys->dblwr;

	unaligned_read_buf = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));

	for (std::list<byte*>::iterator i = recv_dblwr.pages.begin();
	     i != recv_dblwr.pages.end(); ++i, ++page_no_dblwr ) {

		page = *i;
		page_no  = mach_read_from_4(page + FIL_PAGE_OFFSET);
		space_id = mach_read_from_4(page + FIL_PAGE_SPACE_ID);

		if (!fil_tablespace_exists_in_mem(space_id)) {
			/* Maybe we have dropped the single-table tablespace
			and this page once belonged to it: do nothing */

		} else if (!fil_check_adress_in_tablespace(space_id,
							   page_no)) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"A page in the doublewrite buffer is not "
				"within space bounds; space id %lu "
				"page number %lu, page %lu in "
				"doublewrite buf.",
				(ulong) space_id, (ulong) page_no,
				page_no_dblwr);
		} else {
			ulint	zip_size = fil_space_get_zip_size(space_id);

			/* Read in the actual page from the file */
			fil_io(OS_FILE_READ, true, space_id, zip_size,
			       page_no, 0,
			       zip_size ? zip_size : UNIV_PAGE_SIZE,
			       read_buf, NULL);

			/* Check if the page is corrupt */

			if (buf_page_is_corrupted(true, read_buf, zip_size)) {

				fprintf(stderr,
					"InnoDB: Warning: database page"
					" corruption or a failed\n"
					"InnoDB: file read of"
					" space %lu page %lu.\n"
					"InnoDB: Trying to recover it from"
					" the doublewrite buffer.\n",
					(ulong) space_id, (ulong) page_no);

				if (buf_page_is_corrupted(true,
							  page, zip_size)) {
					fprintf(stderr,
						"InnoDB: Dump of the page:\n");
					buf_page_print(
						read_buf, zip_size,
						BUF_PAGE_PRINT_NO_CRASH);
					fprintf(stderr,
						"InnoDB: Dump of"
						" corresponding page"
						" in doublewrite buffer:\n");
					buf_page_print(
						page, zip_size,
						BUF_PAGE_PRINT_NO_CRASH);

					fprintf(stderr,
						"InnoDB: Also the page in the"
						" doublewrite buffer"
						" is corrupt.\n"
						"InnoDB: Cannot continue"
						" operation.\n"
						"InnoDB: You can try to"
						" recover the database"
						" with the my.cnf\n"
						"InnoDB: option:\n"
						"InnoDB:"
						" innodb_force_recovery=6\n");
					ut_error;
				}

				/* Write the good page from the
				doublewrite buffer to the intended
				position */

				fil_io(OS_FILE_WRITE, true, space_id,
				       zip_size, page_no, 0,
				       zip_size ? zip_size : UNIV_PAGE_SIZE,
				       page, NULL);

				ib_logf(IB_LOG_LEVEL_INFO,
					"Recovered the page from"
					" the doublewrite buffer.");

			} else if (buf_page_is_zeroes(read_buf, zip_size)) {

				if (!buf_page_is_zeroes(page, zip_size)
				    && !buf_page_is_corrupted(true, page,
							      zip_size)) {

					/* Database page contained only
					zeroes, while a valid copy is
					available in dblwr buffer. */

					fil_io(OS_FILE_WRITE, true, space_id,
					       zip_size, page_no, 0,
					       zip_size ? zip_size
							: UNIV_PAGE_SIZE,
					       page, NULL);
				}
			}
		}
	}

	fil_flush_file_spaces(FIL_TABLESPACE);
	ut_free(unaligned_read_buf);
}

/****************************************************************//**
Frees doublewrite buffer. */
UNIV_INTERN
void
buf_dblwr_free(void)
/*================*/
{
	/* Free the double write data structures. */
	ut_a(buf_dblwr != NULL);
	ut_ad(buf_dblwr->s_reserved == 0);
	ut_ad(buf_dblwr->b_reserved == 0);

	os_event_free(buf_dblwr->b_event);
	os_event_free(buf_dblwr->s_event);
	ut_free(buf_dblwr->write_buf_unaligned);
	buf_dblwr->write_buf_unaligned = NULL;

	mem_free(buf_dblwr->buf_block_arr);
	buf_dblwr->buf_block_arr = NULL;

	mem_free(buf_dblwr->in_use);
	buf_dblwr->in_use = NULL;

	mutex_free(&buf_dblwr->mutex);
	mem_free(buf_dblwr);
	buf_dblwr = NULL;
}

/********************************************************************//**
Updates the doublewrite buffer when an IO request is completed. */
UNIV_INTERN
void
buf_dblwr_update(
/*=============*/
	const buf_page_t*	bpage,	/*!< in: buffer block descriptor */
	buf_flush_t		flush_type)/*!< in: flush type */
{
	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		return;
	}

	switch (flush_type) {
	case BUF_FLUSH_LIST:
	case BUF_FLUSH_LRU:
		mutex_enter(&buf_dblwr->mutex);

		ut_ad(buf_dblwr->batch_running);
		ut_ad(buf_dblwr->b_reserved > 0);
		ut_ad(buf_dblwr->b_reserved <= buf_dblwr->first_free);

		buf_dblwr->b_reserved--;

		if (buf_dblwr->b_reserved == 0) {
			mutex_exit(&buf_dblwr->mutex);
			/* This will finish the batch. Sync data files
			to the disk. */
			fil_flush_file_spaces(FIL_TABLESPACE);
			mutex_enter(&buf_dblwr->mutex);

			/* We can now reuse the doublewrite memory buffer: */
			buf_dblwr->first_free = 0;
			buf_dblwr->batch_running = false;
			os_event_set(buf_dblwr->b_event);
		}

		mutex_exit(&buf_dblwr->mutex);
		break;
	case BUF_FLUSH_SINGLE_PAGE:
		{
			const ulint size = 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			ulint i;
			mutex_enter(&buf_dblwr->mutex);
			for (i = srv_doublewrite_batch_size; i < size; ++i) {
				if (buf_dblwr->buf_block_arr[i] == bpage) {
					buf_dblwr->s_reserved--;
					buf_dblwr->buf_block_arr[i] = NULL;
					buf_dblwr->in_use[i] = false;
					break;
				}
			}

			/* The block we are looking for must exist as a
			reserved block. */
			ut_a(i < size);
		}
		os_event_set(buf_dblwr->s_event);
		mutex_exit(&buf_dblwr->mutex);
		break;
	case BUF_FLUSH_N_TYPES:
		ut_error;
	}
}

/********************************************************************//**
Check the LSN values on the page. */
static
void
buf_dblwr_check_page_lsn(
/*=====================*/
	const page_t*	page)		/*!< in: page to check */
{
	if (memcmp(page + (FIL_PAGE_LSN + 4),
		   page + (UNIV_PAGE_SIZE
			   - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
		   4)) {

		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: ERROR: The page to be written"
			" seems corrupt!\n"
			"InnoDB: The low 4 bytes of LSN fields do not match "
			"(" ULINTPF " != " ULINTPF ")!"
			" Noticed in the buffer pool.\n",
			mach_read_from_4(
				page + FIL_PAGE_LSN + 4),
			mach_read_from_4(
				page + UNIV_PAGE_SIZE
				- FIL_PAGE_END_LSN_OLD_CHKSUM + 4));
	}
}

/********************************************************************//**
Asserts when a corrupt block is find during writing out data to the
disk. */
static
void
buf_dblwr_assert_on_corrupt_block(
/*==============================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	buf_page_print(block->frame, 0, BUF_PAGE_PRINT_NO_CRASH);

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Apparent corruption of an"
		" index page n:o %lu in space %lu\n"
		"InnoDB: to be written to data file."
		" We intentionally crash server\n"
		"InnoDB: to prevent corrupt data"
		" from ending up in data\n"
		"InnoDB: files.\n",
		(ulong) buf_block_get_page_no(block),
		(ulong) buf_block_get_space(block));

	ut_error;
}

/********************************************************************//**
Check the LSN values on the page with which this block is associated.
Also validate the page if the option is set. */
static
void
buf_dblwr_check_block(
/*==================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
	    || block->page.zip.data) {
		/* No simple validate for compressed pages exists. */
		return;
	}

	buf_dblwr_check_page_lsn(block->frame);

	if (!block->check_index_page_at_flush) {
		return;
	}

	if (page_is_comp(block->frame)) {
		if (!page_simple_validate_new(block->frame)) {
			buf_dblwr_assert_on_corrupt_block(block);
		}
	} else if (!page_simple_validate_old(block->frame)) {

		buf_dblwr_assert_on_corrupt_block(block);
	}
}

/********************************************************************//**
Writes a page that has already been written to the doublewrite buffer
to the datafile. It is the job of the caller to sync the datafile. */
static
void
buf_dblwr_write_block_to_datafile(
/*==============================*/
	const buf_page_t*	bpage,	/*!< in: page to write */
	bool			sync)	/*!< in: true if sync IO
					is requested */
{
	ut_a(bpage);
	ut_a(buf_page_in_file(bpage));

	const ulint flags = sync
		? OS_FILE_WRITE
		: OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER;

	if (bpage->zip.data) {
		fil_io(flags, sync, buf_page_get_space(bpage),
		       buf_page_get_zip_size(bpage),
		       buf_page_get_page_no(bpage), 0,
		       buf_page_get_zip_size(bpage),
		       (void*) bpage->zip.data,
		       (void*) bpage);

		return;
	}


	const buf_block_t* block = (buf_block_t*) bpage;
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	buf_dblwr_check_page_lsn(block->frame);

	fil_io(flags, sync, buf_block_get_space(block), 0,
	       buf_block_get_page_no(block), 0, UNIV_PAGE_SIZE,
	       (void*) block->frame, (void*) block);

}

#ifdef SSD_CACHE_FACE
/********************************************************//**
Write SSD cache block to the data file in the storage. */
static
void
ssd_cache_block_to_datafile(
/*========================*/
    const ssd_meta_dir_t*   entry,  /*!< in: metadata entry */
    byte*                   gc_buf, /*!< in: buffer to write-back */
    bool                    sync)   /*!< in: true if sync IO is requested */
{
   // byte*       ssd_cache_gc_buf;
   // ulint       ssd_offset = 0;

    const ulint flags = sync
        ? OS_FILE_WRITE
        : OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER;

/*    assert(!posix_memalign((void **) &ssd_cache_gc_buf, 4096, UNIV_PAGE_SIZE));
    
    ssd_offset = entry->ssd_offset * UNIV_PAGE_SIZE;
    if ((ulint) pread(ssd_cache_fd, ssd_cache_gc_buf, UNIV_PAGE_SIZE, ssd_offset) == UNIV_PAGE_SIZE) {
        fprintf(stderr, "Reading SSD cache file for overwriting succeeded! (metadata index) = (%lu)\n",
                        entry->ssd_offset);
    } else {
        fprintf(stderr, "Reading SSD cache file for overwriting failed.\n");
    }

    memcpy(ssd_cache_gc_buf, gc_buf, UNIV_PAGE_SIZE);
*/
    fil_io(flags, sync, entry->space, 0,
            entry->offset, 0, UNIV_PAGE_SIZE,
            (void*) gc_buf, NULL);

    free(gc_buf);
}
#endif

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk,
and also wakes up the aio thread if simulated aio is used. It is very
important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
UNIV_INTERN
void
buf_dblwr_flush_buffered_writes(void)
/*=================================*/
{
	byte*		write_buf;
	ulint		first_free;
	ulint		len;
#ifdef SSD_CACHE_FACE
    ulint       first_idx;
    ulint       page_num = 0;
    ulint       meta_idx;
    ulint       gsc_page_num = 0;
    ulint       total_page_num = 0;
    byte*       ssd_cache_buf;
//    ulint       fold = 0;
    bool        ssd_cache_size_over_first = false;
#endif

	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		/* Sync the writes to the disk. */
		buf_dblwr_sync_datafiles();
		return;
	}

try_again:
	mutex_enter(&buf_dblwr->mutex);

	/* Write first to doublewrite buffer blocks. We use synchronous
	aio and thus know that file write has been completed when the
	control returns. */

	if (buf_dblwr->first_free == 0) {

		mutex_exit(&buf_dblwr->mutex);

		return;
	}

	if (buf_dblwr->batch_running) {
		/* Another thread is running the batch right now. Wait
		for it to finish. */
		ib_int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);

		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	ut_a(!buf_dblwr->batch_running);
	ut_ad(buf_dblwr->first_free == buf_dblwr->b_reserved);

	/* Disallow anyone else to post to doublewrite buffer or to
	start another batch of flushing. */
	buf_dblwr->batch_running = true;
	first_free = buf_dblwr->first_free;

	/* Now safe to release the mutex. Note that though no other
	thread is allowed to post to the doublewrite batch flushing
	but any threads working on single page flushes are allowed
	to proceed. */
	mutex_exit(&buf_dblwr->mutex);

	write_buf = buf_dblwr->write_buf;

	for (ulint len2 = 0, i = 0;
	     i < buf_dblwr->first_free;
	     len2 += UNIV_PAGE_SIZE, i++) {

		const buf_block_t*	block;

		block = (buf_block_t*) buf_dblwr->buf_block_arr[i];

		if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
		    || block->page.zip.data) {
			/* No simple validate for compressed
			pages exists. */
			continue;
		}

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block(block);

		/* Check that the page as written to the doublewrite
		buffer has sane LSN values. */
		buf_dblwr_check_page_lsn(write_buf + len2);

#ifdef SSD_CACHE_FACE
        if (srv_use_ssd_cache) {
            page_num++;
        }
#endif
	}

    if (!srv_use_ssd_cache) {
    	/* Write out the first block of the doublewrite buffer */
    	len = ut_min(TRX_SYS_DOUBLEWRITE_BLOCK_SIZE,
    		     buf_dblwr->first_free) * UNIV_PAGE_SIZE;

    	fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
    	       buf_dblwr->block1, 0, len,
    	       (void*) write_buf, NULL);

    	if (buf_dblwr->first_free <= TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
    		/* No unwritten pages in the second block. */
    		goto flush;
    	}

    	/* Write out the second block of the doublewrite buffer. */
    	len = (buf_dblwr->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
    	       * UNIV_PAGE_SIZE;

    	write_buf = buf_dblwr->write_buf
    		    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;

    	fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
    	       buf_dblwr->block2, 0, len,
    	       (void*) write_buf, NULL);

flush:
    	/* increment the doublewrite flushed pages counter */
    	srv_stats.dblwr_pages_written.add(buf_dblwr->first_free);
    	srv_stats.dblwr_writes.inc();

    	/* Now flush the doublewrite buffer data to disk */
    	fil_flush(TRX_SYS_SPACE);

    	/* We know that the writes have been flushed to disk now
    	and in recovery we will find them in the doublewrite buffer
    	blocks. Next do the writes to the intended positions. */


    	/* Up to this point first_free and buf_dblwr->first_free are
	    same because we have set the buf_dblwr->batch_running flag
    	disallowing any other thread to post any request but we
    	can't safely access buf_dblwr->first_free in the loop below.
    	This is so because it is possible that after we are done with
    	the last iteration and before we terminate the loop, the batch
    	gets finished in the IO helper thread and another thread posts
    	a new batch setting buf_dblwr->first_free to a higher value.
    	If this happens and we are using buf_dblwr->first_free in the
    	loop termination condition then we'll end up dispatching
    	the same block twice from two different threads. */
    	ut_ad(first_free == buf_dblwr->first_free);
    	for (ulint i = 0; i < first_free; i++) {
    		buf_dblwr_write_block_to_datafile(
    			buf_dblwr->buf_block_arr[i], false);
    	}

    	/* Wake possible simulated aio thread to actually post the
    	writes to the operating system. We don't flush the files
    	at this point. We leave it to the IO helper thread to flush
    	datafiles when the whole batch has been processed. */
    	os_aio_simulated_wake_handler_threads();
    }

    ut_ad(first_free == buf_dblwr->first_free);

#if SSD_CACHE_FACE
    if (srv_use_ssd_cache) {
        if (page_num != 0) {
            ut_a(page_num == buf_dblwr->first_free);

            /* Reserve metadata index. */
            rw_lock_x_lock(ssd_cache_meta_idx_lock);

            first_idx = ssd_cache_meta_free_idx;
           
            meta_idx = first_idx;
            for (;;) {
                if (meta_idx == ssd_cache_size) {
                    meta_idx = 0;
                }

                if ((ssd_meta_dir[meta_idx].flags & BM_REF) &&
                    (ssd_meta_dir[meta_idx].flags & BM_VALID)) {
                    gsc_page_num++;
                    ssd_meta_dir[meta_idx].flags |= BM_GSC;
                }

                total_page_num++;

                if ((total_page_num - gsc_page_num) == page_num) {
                    fprintf(stderr, "buffered flush, total = %lu, gsc = %lu, idx = %lu\n",
                                    total_page_num, gsc_page_num, first_idx);
                    break;
                }

                meta_idx++;
            }

            ssd_cache_meta_free_idx += total_page_num;
            if (ssd_cache_meta_free_idx >= ssd_cache_size) {
                ssd_cache_meta_free_idx = ssd_cache_meta_free_idx - ssd_cache_size;
                if (!ssd_cache_size_over) {
                    ssd_cache_size_over_first = true;
                    ssd_cache_size_over = true;
                }
            }

            rw_lock_x_unlock(ssd_cache_meta_idx_lock);

            /* Rebuild write buffer. */
            ssd_cache_buf = rebuild_write_buf_for_ssd_cache(first_idx,
                            total_page_num, gsc_page_num, write_buf, ssd_cache_size_over_first);

            if (first_idx + gsc_page_num >= ssd_cache_size) {
                meta_idx = (first_idx + gsc_page_num) - ssd_cache_size;
            } else {
                meta_idx = first_idx + gsc_page_num;
            }

            /* Update metadata directory. */
            for (ulint i = 0; i < page_num; i++) {
                const buf_block_t*  block;

                block = (buf_block_t*) buf_dblwr->buf_block_arr[i];

                if (meta_idx == ssd_cache_size) {
                    meta_idx = 0;
                }

                update_ssd_cache_info((const_cast<buf_page_t*>(&block->page))->space,
                                        (const_cast<buf_page_t*>(&block->page))->offset,
                                        (const_cast<buf_page_t*>(&block->page))->newest_modification,
                                        meta_idx);
                meta_idx++;
            }

            /* Update SSD cache hash table and SSD metadata directory. */
            /*rw_lock_x_lock(ssd_cache_hash_lock);

            meta_idx = first_idx;
            for (ulint i = 0; i < total_page_num; i++) {
                if (meta_idx == ssd_cache_size) {
                    meta_idx = 0;
                }
           
                ut_a(ssd_meta_dir[meta_idx].space == mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));
                ut_a(ssd_meta_dir[meta_idx].offset == mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i + FIL_PAGE_OFFSET));
                fprintf(stderr, "%lu: %lu, %lu, %lu, %lu\n",
                            meta_idx,
                            mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                                FIL_PAGE_PREV),
                            mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                                FIL_PAGE_NEXT),
                            mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                                FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID),
                            mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                                FIL_PAGE_OFFSET));

                mutex_enter(&ssd_meta_dir[meta_idx].mutex);
                ssd_meta_dir[meta_idx].io_fix = BUF_IO_WRITE;
                mutex_exit(&ssd_meta_dir[meta_idx].mutex);
            
                fold = buf_page_address_fold(ssd_meta_dir[meta_idx].space,
                                            ssd_meta_dir[meta_idx].offset);
                insert_ssd_metadata(fold, meta_idx);

                meta_idx++;
            }

            rw_lock_x_unlock(ssd_cache_hash_lock);*/

            /* Update SSD cache file. Write victim page to SSD cache. */
            insert_page_in_ssd_cache(first_idx, total_page_num, ssd_cache_buf);

            /* Reset the io_fix to BUF_IO_NONE. */
            meta_idx = first_idx;
            for (ulint i = 0; i < total_page_num; i++) {
                if (meta_idx == ssd_cache_size) {
                    meta_idx = 0; 
                }

                mutex_enter(&ssd_meta_dir[meta_idx].mutex);
                ssd_meta_dir[meta_idx].io_fix = BUF_IO_NONE;
                mutex_exit(&ssd_meta_dir[meta_idx].mutex);

                meta_idx++;
            }
            
            free(ssd_cache_buf);
        }

        /* Call buf_page_io_complete() to unfix io_fix. */
    	for (ulint i = 0; i < first_free; i++) {
    		const buf_block_t*	block;
    		block = (buf_block_t*) buf_dblwr->buf_block_arr[i];

            ut_a(buf_page_io_complete(const_cast<buf_page_t*>(&block->page)));
	    }

        os_aio_simulated_wake_handler_threads();
    }
#endif
}

/********************************************************************//**
Posts a buffer page for writing. If the doublewrite memory buffer is
full, calls buf_dblwr_flush_buffered_writes and waits for for free
space to appear. */
UNIV_INTERN
void
buf_dblwr_add_to_batch(
/*====================*/
	buf_page_t*	bpage)	/*!< in: buffer block to write */
{
	ulint	zip_size;

	ut_a(buf_page_in_file(bpage));

try_again:
	mutex_enter(&buf_dblwr->mutex);

	ut_a(buf_dblwr->first_free <= srv_doublewrite_batch_size);

	if (buf_dblwr->batch_running) {

		/* This not nearly as bad as it looks. There is only
		page_cleaner thread which does background flushing
		in batches therefore it is unlikely to be a contention
		point. The only exception is when a user thread is
		forced to do a flush batch because of a sync
		checkpoint. */
		ib_int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);

		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	if (buf_dblwr->first_free == srv_doublewrite_batch_size) {
		mutex_exit(&(buf_dblwr->mutex));

		buf_dblwr_flush_buffered_writes();

		goto try_again;
	}

	zip_size = buf_page_get_zip_size(bpage);

	if (zip_size) {
		UNIV_MEM_ASSERT_RW(bpage->zip.data, zip_size);
		/* Copy the compressed page and clear the rest. */
		memcpy(buf_dblwr->write_buf
		       + UNIV_PAGE_SIZE * buf_dblwr->first_free,
		       bpage->zip.data, zip_size);
		memset(buf_dblwr->write_buf
		       + UNIV_PAGE_SIZE * buf_dblwr->first_free
		       + zip_size, 0, UNIV_PAGE_SIZE - zip_size);
	} else {
		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
		UNIV_MEM_ASSERT_RW(((buf_block_t*) bpage)->frame,
				   UNIV_PAGE_SIZE);

		memcpy(buf_dblwr->write_buf
		       + UNIV_PAGE_SIZE * buf_dblwr->first_free,
		       ((buf_block_t*) bpage)->frame, UNIV_PAGE_SIZE);
	}

	buf_dblwr->buf_block_arr[buf_dblwr->first_free] = bpage;

	buf_dblwr->first_free++;
	buf_dblwr->b_reserved++;

	ut_ad(!buf_dblwr->batch_running);
	ut_ad(buf_dblwr->first_free == buf_dblwr->b_reserved);
	ut_ad(buf_dblwr->b_reserved <= srv_doublewrite_batch_size);

	if (buf_dblwr->first_free == srv_doublewrite_batch_size) {
		mutex_exit(&(buf_dblwr->mutex));

		buf_dblwr_flush_buffered_writes();

		return;
	}

	mutex_exit(&(buf_dblwr->mutex));
}

#ifdef SSD_CACHE_FACE
/**************************************************************//**
Create a new metadata entry for SSD cache hash table. */
UNIV_INTERN
void
create_new_ssd_metadata(
/*===================*/
    ulint space,        /*!< in: space id */
    ulint offset,       /*!< in: page number */
    lsn_t lsn,          /*!< in: lsn */
    ulint meta_idx)     /*!< in: metadata index */
{
	ssd_meta_dir[meta_idx].space = space;
    ssd_meta_dir[meta_idx].offset = offset;
    ssd_meta_dir[meta_idx].lsn = lsn;

    ssd_meta_dir[meta_idx].flags = 0;

    ssd_meta_dir[meta_idx].flags |= BM_DIRTY;
    ssd_meta_dir[meta_idx].flags |= BM_VALID;

    ssd_meta_dir[meta_idx].hash = NULL;
    ssd_meta_dir[meta_idx].ssd_offset = meta_idx;
    ssd_meta_dir[meta_idx].ref_count = 0;

    ssd_meta_dir[meta_idx].io_fix = BUF_IO_NONE;
}

/**************************************************************//**
Insert metadata entry into the metadata directory with no lock
for recovery. */
UNIV_INTERN
void
insert_ssd_metadata_for_recovery(
/*=============================*/
    ssd_meta_dir_t* metadata_entry, /*!< in: metadata entry */
    ulint meta_idx)                 /*!< in: metadata index */
{
    rw_lock_x_lock(ssd_cache_meta_idx_lock);
    ssd_cache_meta_free_idx = meta_idx + 1;
    rw_lock_x_unlock(ssd_cache_meta_idx_lock);

    metadata_entry->ssd_offset = meta_idx;

    memcpy(&ssd_meta_dir[meta_idx], metadata_entry, sizeof(ssd_meta_dir_t));
    fprintf(stderr, "metadata index: %lu, (space id, offset) = (%u, %u)\n", meta_idx, ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset);
}

/**************************************************************//**
If metadata directory is full, evict the old entry according to
FIFO replacement and update metadata index. Otherwise, set
metadata index using lock. */
static
void 
ssd_cache_writeback(
/*================*/
    ulint meta_idx, /*!< in: metadata index */
    byte* gc_buf)   /*!< in: buffer to write-back */
{
    ulint           fold;
    ssd_meta_dir_t* old_entry = NULL;

    /* If the page to be overwritten is valid, delete it
    from the SSD cache hash table and flush it to the storage. */
    if (ssd_meta_dir[meta_idx].flags & BM_VALID) {
        ssd_meta_dir[meta_idx].flags |= BM_WB;

        fold = buf_page_address_fold(ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset);

        rw_lock_s_lock(ssd_cache_hash_lock);
        HASH_SEARCH(hash, ssd_cache, fold, ssd_meta_dir_t*, old_entry, ut_ad(1),
                    old_entry->space == ssd_meta_dir[meta_idx].space
                    && old_entry->offset == ssd_meta_dir[meta_idx].offset);
        rw_lock_s_unlock(ssd_cache_hash_lock);

        if (old_entry && (old_entry->ssd_offset == meta_idx) && (old_entry->flags & BM_VALID)) {
            /* Wait until the IO in progress is finished. */
            for (;;) {
                enum buf_io_fix io_fix;

                mutex_enter(&ssd_meta_dir[meta_idx].mutex);
                io_fix = (enum buf_io_fix) ssd_meta_dir[meta_idx].io_fix;
                mutex_exit(&ssd_meta_dir[meta_idx].mutex);

                if (io_fix == BUF_IO_NONE) {
                    /* Flush the page to be overwritten to the storage. */
                    if (old_entry->flags & BM_VALID) {
                        ssd_cache_block_to_datafile(old_entry, gc_buf, true);
                    }

                    /* Remove the metadata entry of the page to be overwritten
                    from the hash table. */
                    rw_lock_x_lock(ssd_cache_hash_lock);

                    /* If the old entry is already invalid, do nothing. */
                    if (old_entry->flags & BM_VALID) {
                        old_entry->flags &= ~BM_VALID;

                        fprintf(stderr, "HASH DELETE2 metadata index: %lu (%lu), (space id, offset) = (%u, %u)\n",
                                        old_entry->ssd_offset, fold, old_entry->space,
                                        old_entry->offset);
                        HASH_DELETE(ssd_meta_dir_t, hash, ssd_cache, fold, old_entry);
                    }

                    rw_lock_x_unlock(ssd_cache_hash_lock);

                    break;
                } else {
                    fprintf(stderr, "writeback sleep..(metadata index) = (%lu), (space, offset) = (%u, %u)\n",
                                    old_entry->ssd_offset, old_entry->space, old_entry->offset);
                    os_thread_sleep(WAIT_FOR_READ);
                }
            }
        }
    }
}

/**************************************************************//**
Insert metadata entry into the metadata directory. */
UNIV_INTERN
void
insert_ssd_metadata(
/*================*/
    ulint fold,                     /*!< in: fold value */
    ulint meta_idx)                 /*!< in: metadata index */
{
  //  ssd_meta_dir_t* old_entry = NULL;

    /* Search SSD cache hash table to check whether the old page is in the SSD cache or not. */
    /*HASH_SEARCH(hash, ssd_cache, fold, ssd_meta_dir_t*, old_entry, ut_ad(1),
                old_entry->space == ssd_meta_dir[meta_idx].space
                && old_entry->offset == ssd_meta_dir[meta_idx].offset);
*/
    /* If the old entry exists, remove it from the hash table. */
    /*if (old_entry) {
                old_entry->flags &= ~BM_VALID;
                fprintf(stderr, "HASH DELETE3 metadata index: %lu, (space id, offset) = (%u, %u)\n",
                        meta_idx, ssd_meta_dir[meta_idx].space,
                        ssd_meta_dir[meta_idx].offset);
                HASH_DELETE(ssd_meta_dir_t, hash, ssd_cache, fold, old_entry);
    }*/

    ssd_meta_dir[meta_idx].flags |= BM_VALID;

    HASH_INSERT(ssd_meta_dir_t, hash, ssd_cache, fold, &ssd_meta_dir[meta_idx]);

    fprintf(stderr, "metadata index: %lu (%lu), (space id, offset) = (%u, %u)\n",
            ssd_meta_dir[meta_idx].ssd_offset, fold,
            ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset);

    time_t  current_time;
    time(&current_time);
    fprintf(stderr, "insert time: %ld, %lu, (space, offset) = (%u, %u)\n",
            current_time, fold, ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset);
}

/**************************************************************//**
Update SSD cache hash table and metadata directory. */
UNIV_INTERN
void
update_ssd_cache_info(
/*==================*/
    ulint   space,      /*!< in: space id */
    ulint   offset,     /*!< in: page number */
    lsn_t   lsn,        /*!< in: lsn */
    ulint meta_idx)     /*!< in: metadata index */    
{
	ulint   		fold;
	ssd_meta_dir_t*	old_entry = NULL;

	fold = buf_page_address_fold(space, offset);
  
	/* Create a SSD cache metadata entry. */
    create_new_ssd_metadata(space, offset, lsn, meta_idx);

	/* Search SSD cache hash table to check whether the old page is in the SSD cache or not. */
	rw_lock_s_lock(ssd_cache_hash_lock);
	HASH_SEARCH(hash, ssd_cache, fold, ssd_meta_dir_t*, old_entry, ut_ad(1),
                old_entry->space == space && old_entry->offset == offset);
    rw_lock_s_unlock(ssd_cache_hash_lock);

	/* If the old entry exists, remove it from the hash table. */
	if (old_entry) {
        ut_a((old_entry->space == space) && (old_entry->offset == offset));

        /* Wait until the IO in progress is finished. */
		for (;;) {
            enum buf_io_fix io_fix;

            mutex_enter(&old_entry->mutex);
            io_fix = (enum buf_io_fix) old_entry->io_fix;
            mutex_exit(&old_entry->mutex);

            if ((io_fix == BUF_IO_NONE) || (io_fix == BUF_IO_WRITE)) {
			    rw_lock_x_lock(ssd_cache_hash_lock);

                old_entry->flags &= ~BM_VALID;
                fprintf(stderr, "HASH DELETE1 metadata index: %lu (%lu), (space id, offset) = (%u, %u)\n",
                                old_entry->ssd_offset, fold, old_entry->space, old_entry->offset);

				HASH_DELETE(ssd_meta_dir_t, hash, ssd_cache, fold, old_entry);
                
				rw_lock_x_unlock(ssd_cache_hash_lock);
                
				break;
			} else {
                fprintf(stderr, "update sleep..(metadata index) = (%lu), (space, offset) = (%u, %u)\n",
                                old_entry->ssd_offset, old_entry->space, old_entry->offset);
				os_thread_sleep(WAIT_FOR_READ);
			}
		}
	}

    rw_lock_x_lock(ssd_cache_hash_lock);

    mutex_enter(&ssd_meta_dir[meta_idx].mutex);
    ssd_meta_dir[meta_idx].io_fix = BUF_IO_WRITE;
    mutex_exit(&ssd_meta_dir[meta_idx].mutex);

    insert_ssd_metadata(fold, meta_idx);

    rw_lock_x_unlock(ssd_cache_hash_lock);
}

/**************************************************************//**
Insert a page into the SSD cache. */
UNIV_INTERN
void
insert_page_in_ssd_cache(
/*=====================*/
    ulint first_idx,    /*!< in: metadata index of the first page to write */
    ulint page_num,     /*!< in: total number of pages to write */
    byte* buf)          /*!< in: buffer used in writing to the SSD cache */
{
	ssize_t     r;
    ulint       ssd_offset = 0;
    ulint       len1 = 0;
    ulint       len2 = 0;
    ulint       write_page_num1 = 0;
    ulint       write_page_num2 = 0;
    byte*       write_buf;

    /* Calculate the size of pages to write. */
    if (first_idx + page_num >= ssd_cache_size) {
        /* The index of the page to write is over the index of SSD cache. 
        So, write the pages by dividing into two groups. */
        write_page_num1 = ssd_cache_size - first_idx;
        write_page_num2 = page_num - write_page_num1;

        len1 = write_page_num1 * UNIV_PAGE_SIZE;
        len2 = write_page_num2 * UNIV_PAGE_SIZE;
    } else {
        len1 = page_num * UNIV_PAGE_SIZE;
    }

    ssd_offset = first_idx * UNIV_PAGE_SIZE;
    write_buf = buf;

    /* Write out the pages of the first group. */
    r = pwrite(ssd_cache_fd, (void*) write_buf, len1, ssd_offset);

    if((ulint) r == len1) {
        fprintf(stderr, "Insertion in SSD cache succeeded! (metadata index) = (%lu, %lu)\n",
                        first_idx, page_num);
	} else {
        fprintf(stderr, "Insertion in SSD cache failed.\n");
	}

    /* No unwritten pages in the second group. */
    if (!len2)  return;

    ssd_offset = 0;
    write_buf = buf + len1;

    /* Write out the pages of the second group. */
    r = pwrite(ssd_cache_fd, (void*) write_buf, len2, ssd_offset);

    if((ulint) r == len2) {
        fprintf(stderr, "Insertion in SSD cache succeeded! (metadata index) = (0, %lu)\n", 
                        write_page_num2);
    } else {
        fprintf(stderr, "Insertion in SSD cache failed.\n");
    }

    /* Close the file descriptor when MySQL is shut down. */
	//close(fd);
}

/**************************************************************//**
Rebuild write buffer to distinguish three types of pages such
as invalid pages, pages which receive second chance and pages
which should be written back to the storage (write-back). */
UNIV_INTERN
byte*
rebuild_write_buf_for_ssd_cache(
/*============================*/
    ulint first_idx,            /*!< in: metadata index of the first page to write */
    ulint total_page_num,       /*!< in: the number of total pages to write */
    ulint gsc_page_num,         /*!< in: the number of pages to give second chance */
    byte* buf,                  /*!< in: buffer used in writing to the SSD cache */
    bool  ssd_cache_size_over_first)
{
    byte*   read_buf;
    byte*   gsc_buf;
    byte*   write_buf;
    byte*   gc_buf;

    ulint   ssd_offset = 0;
    ulint   gsc_idx = 0;
    ulint   read_page_num1 = 0;
    ulint   read_page_num2 = 0;
    ulint   len1 = 0;
    ulint   len2 = 0;
    ulint   meta_idx = 0;

    assert(!posix_memalign((void **) &write_buf, 4096,
            UNIV_PAGE_SIZE * total_page_num));

    if (!ssd_cache_size_over) {
        memcpy(write_buf, buf, UNIV_PAGE_SIZE * total_page_num);       
        return(write_buf);
    }

    if (gsc_page_num) {
        assert(!posix_memalign((void **) &gsc_buf, 4096,
                UNIV_PAGE_SIZE * gsc_page_num));
    }

    if (ssd_cache_size_over_first) {
        assert(!posix_memalign((void **) &read_buf, 4096,
                UNIV_PAGE_SIZE * ((first_idx + total_page_num) - ssd_cache_size)));

        ssd_offset = 0;
        len1 = UNIV_PAGE_SIZE * ((first_idx + total_page_num) - ssd_cache_size);
    } else {
        assert(!posix_memalign((void **) &read_buf, 4096,
                UNIV_PAGE_SIZE * total_page_num));

        ssd_offset = first_idx * UNIV_PAGE_SIZE;

        /* Calculate the size of pages to read. */
        if (first_idx + total_page_num >= ssd_cache_size) {
            /* The index of the page to read is over the index of SSD cache.
            So, read SSD cache twice. */
            read_page_num1 = ssd_cache_size - first_idx;
            read_page_num2 = total_page_num - read_page_num1;

            len1 = read_page_num1 * UNIV_PAGE_SIZE;
            len2 = read_page_num2 * UNIV_PAGE_SIZE;
        } else {
            len1 = total_page_num * UNIV_PAGE_SIZE;
        }
    }

    /* Read in the pages of the first group. */
    if ((ulint) pread(ssd_cache_fd, read_buf, len1, ssd_offset) == len1) {
        fprintf(stderr, "Reading for rebuilding write buffer1 succeeded! (metadata index) = (%lu)\n",
                        first_idx);
    } else {
        fprintf(stderr, "Reading for rebuilding write buffer1 failed\n");
    }

    /* Read in the pages of the second group. */
    if (len2 != 0) {
        ssd_offset = 0;

        if ((ulint) pread(ssd_cache_fd, (read_buf + len1), len2, ssd_offset) == len2) {
            fprintf(stderr, "Reading for rebuilding write buffer2 succeeded! (metadata index) = (0)\n");
        } else {
            fprintf(stderr, "Reading for rebuilding write buffer2 failed\n");
        }
    }

    /* Rebuild write buffer according to the type of the target page. */
    meta_idx = first_idx;
    for (ulint i = 0;i < total_page_num; i++) {
        if (meta_idx == ssd_cache_size) {
            meta_idx = 0;
        }

        if ((ssd_meta_dir[meta_idx].flags & BM_REF) &&
            (ssd_meta_dir[meta_idx].flags & BM_VALID) &&
            (ssd_meta_dir[meta_idx].flags & BM_GSC)) {
            /* Give second chance to the pages which reference bit is set. */
            fprintf(stderr, "second chance = %lu, %lu\n", meta_idx, gsc_idx);
            if (ssd_cache_size_over_first) {
                memcpy(gsc_buf + UNIV_PAGE_SIZE * gsc_idx,
                        read_buf + UNIV_PAGE_SIZE * meta_idx,
                        UNIV_PAGE_SIZE);
            } else {
                memcpy(gsc_buf + UNIV_PAGE_SIZE * gsc_idx,
                        read_buf + UNIV_PAGE_SIZE * i,
                        UNIV_PAGE_SIZE);
            }

            if (first_idx + gsc_idx >= ssd_cache_size) {
                update_ssd_cache_info(ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset,
                                        ssd_meta_dir[meta_idx].lsn,
                                        (first_idx + gsc_idx) - ssd_cache_size);
            } else {
                update_ssd_cache_info(ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset,
                                        ssd_meta_dir[meta_idx].lsn, first_idx + gsc_idx);
            }

            gsc_idx++;
        } else if (ssd_meta_dir[meta_idx].flags & BM_VALID) {
            /* Write back SSD cache data, if necessary. */
            fprintf(stderr, "writeback = %lu\n", meta_idx);
            assert(!posix_memalign((void **) &gc_buf, 4096, UNIV_PAGE_SIZE));

            if (ssd_cache_size_over_first) {
                memcpy(gc_buf, read_buf + UNIV_PAGE_SIZE * meta_idx,
                        UNIV_PAGE_SIZE);
            } else {
                memcpy(gc_buf, read_buf + UNIV_PAGE_SIZE * i,
                        UNIV_PAGE_SIZE);
            }

            ssd_cache_writeback(meta_idx, gc_buf);            
        }

        meta_idx++;
    }

    ut_a(gsc_idx == gsc_page_num);

    if (gsc_page_num) {
        memcpy(write_buf, gsc_buf, UNIV_PAGE_SIZE * gsc_page_num);
    }
    memcpy(write_buf + (UNIV_PAGE_SIZE * gsc_page_num), buf,
            UNIV_PAGE_SIZE * (total_page_num - gsc_page_num));

    if (gsc_page_num) {
        free(gsc_buf);
    }
    free(read_buf);

    return(write_buf);
}
#endif

/********************************************************************//**
Writes a page to the doublewrite buffer on disk, sync it, then write
the page to the datafile and sync the datafile. This function is used
for single page flushes. If all the buffers allocated for single page
flushes in the doublewrite buffer are in use we wait here for one to
become free. We are guaranteed that a slot will become free because any
thread that is using a slot must also release the slot before leaving
this function. */
UNIV_INTERN
void
buf_dblwr_write_single_page(
/*========================*/
	buf_page_t*	bpage,	/*!< in: buffer block to write */
	bool		sync)	/*!< in: true if sync IO requested */
{
	ulint		n_slots;
	ulint		size;
	ulint		zip_size;
	ulint		offset;
	ulint		i;

	ut_a(buf_page_in_file(bpage));
	ut_a(srv_use_doublewrite_buf);
	ut_a(buf_dblwr != NULL);

	/* total number of slots available for single page flushes
	starts from srv_doublewrite_batch_size to the end of the
	buffer. */
	size = 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
	ut_a(size > srv_doublewrite_batch_size);
	n_slots = size - srv_doublewrite_batch_size;

	if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block((buf_block_t*) bpage);

		/* Check that the page as written to the doublewrite
		buffer has sane LSN values. */
		if (!bpage->zip.data) {
			buf_dblwr_check_page_lsn(
				((buf_block_t*) bpage)->frame);
		}
	}

retry:
	mutex_enter(&buf_dblwr->mutex);
	if (buf_dblwr->s_reserved == n_slots) {

		/* All slots are reserved. */
		ib_int64_t	sig_count =
			os_event_reset(buf_dblwr->s_event);
		mutex_exit(&buf_dblwr->mutex);
		os_event_wait_low(buf_dblwr->s_event, sig_count);

		goto retry;
	}

	for (i = srv_doublewrite_batch_size; i < size; ++i) {

		if (!buf_dblwr->in_use[i]) {
			break;
		}
	}

	/* We are guaranteed to find a slot. */
	ut_a(i < size);
	buf_dblwr->in_use[i] = true;
	buf_dblwr->s_reserved++;
	buf_dblwr->buf_block_arr[i] = bpage;

	/* increment the doublewrite flushed pages counter */
	srv_stats.dblwr_pages_written.inc();
	srv_stats.dblwr_writes.inc();

	mutex_exit(&buf_dblwr->mutex);

	/* Lets see if we are going to write in the first or second
	block of the doublewrite buffer. */
	if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		offset = buf_dblwr->block1 + i;
	} else {
		offset = buf_dblwr->block2 + i
			 - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
	}

	/* We deal with compressed and uncompressed pages a little
	differently here. In case of uncompressed pages we can
	directly write the block to the allocated slot in the
	doublewrite buffer in the system tablespace and then after
	syncing the system table space we can proceed to write the page
	in the datafile.
	In case of compressed page we first do a memcpy of the block
	to the in-memory buffer of doublewrite before proceeding to
	write it. This is so because we want to pad the remaining
	bytes in the doublewrite page with zeros. */

	zip_size = buf_page_get_zip_size(bpage);

#ifdef SSD_CACHE_FACE
    ulint   first_idx;
    ulint   meta_idx;
    ulint   gsc_page_num = 0;
    ulint   total_page_num = 0;
    byte*   ssd_cache_buf;
//    ulint   fold;
    bool    ssd_cache_size_over_first = false;

	if (srv_use_ssd_cache && !zip_size) {
        /* Reserve metadata index. */
        rw_lock_x_lock(ssd_cache_meta_idx_lock);

        first_idx = ssd_cache_meta_free_idx;

        meta_idx = first_idx;
        for (;;) {
            if (meta_idx == ssd_cache_size) {
                meta_idx = 0;
            }

            if ((ssd_meta_dir[meta_idx].flags & BM_REF) &&
                (ssd_meta_dir[meta_idx].flags & BM_VALID)) {
                gsc_page_num++;
                ssd_meta_dir[meta_idx].flags |= BM_GSC;
            }

            total_page_num++;

            if ((total_page_num - gsc_page_num) == 1) {
                fprintf(stderr, "single page flush, total = %lu, gsc = %lu, idx = %lu\n",
                                total_page_num, gsc_page_num, first_idx);
                break;
            }

            meta_idx++;
        }

        ssd_cache_meta_free_idx += total_page_num;
        if (ssd_cache_meta_free_idx >= ssd_cache_size) {
            ssd_cache_meta_free_idx = ssd_cache_meta_free_idx - ssd_cache_size;
            if (!ssd_cache_size_over)
            {
                ssd_cache_size_over_first = true;
                ssd_cache_size_over = true;
            }
        }

        rw_lock_x_unlock(ssd_cache_meta_idx_lock);

        /* Rebuild write buffer. */
        ssd_cache_buf = rebuild_write_buf_for_ssd_cache(first_idx,
                        total_page_num, gsc_page_num, ((buf_block_t*) bpage)->frame,
                        ssd_cache_size_over_first);

        if (first_idx + gsc_page_num >= ssd_cache_size) {
            meta_idx = (first_idx + gsc_page_num) - ssd_cache_size;
        } else {
            meta_idx = first_idx + gsc_page_num;
        }

        /* Update metadata directory. */
	    update_ssd_cache_info(bpage->space, bpage->offset,
                                bpage->newest_modification, meta_idx);

        /* Update SSD cache hash table and SSD metadata directory. */
        /*rw_lock_x_lock(ssd_cache_hash_lock);

        meta_idx = first_idx;
        for (ulint i = 0; i < total_page_num; i++) {
            if (meta_idx == ssd_cache_size) {
                meta_idx = 0;
            }

            ut_a(ssd_meta_dir[meta_idx].space == mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));
            ut_a(ssd_meta_dir[meta_idx].offset == mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i + FIL_PAGE_OFFSET));

            fprintf(stderr, "%lu: %lu, %lu, %lu, %lu\n",
                        meta_idx,
                        mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                            FIL_PAGE_PREV),
                        mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                            FIL_PAGE_NEXT),
                        mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                            FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID),
                        mach_read_from_4(ssd_cache_buf + UNIV_PAGE_SIZE * i +
                                            FIL_PAGE_OFFSET));

            mutex_enter(&ssd_meta_dir[meta_idx].mutex);
            ssd_meta_dir[meta_idx].io_fix = BUF_IO_WRITE;
            mutex_exit(&ssd_meta_dir[meta_idx].mutex);

            fold = buf_page_address_fold(ssd_meta_dir[meta_idx].space, ssd_meta_dir[meta_idx].offset);
            insert_ssd_metadata(fold, meta_idx);

            meta_idx++;
        }

        rw_lock_x_unlock(ssd_cache_hash_lock);*/

        /* Update SSD cache file. Write victim page to SSD cache. */
        insert_page_in_ssd_cache(first_idx, total_page_num, ssd_cache_buf);

        /* Reset the io_fix to BUF_IO_NONE. */
        meta_idx = first_idx;
        for (ulint i = 0; i < total_page_num; i++) {
            if (meta_idx == ssd_cache_size) {
                meta_idx = 0;
            }

            mutex_enter(&ssd_meta_dir[meta_idx].mutex);
            ssd_meta_dir[meta_idx].io_fix = BUF_IO_NONE;
            mutex_exit(&ssd_meta_dir[meta_idx].mutex);

            meta_idx++;
        }

        free(ssd_cache_buf);
	}
#endif
    else {
        if (zip_size) {
            memcpy(buf_dblwr->write_buf + UNIV_PAGE_SIZE * i,
                    bpage->zip.data, zip_size);
            memset(buf_dblwr->write_buf + UNIV_PAGE_SIZE * i
                    + zip_size, 0, UNIV_PAGE_SIZE - zip_size);

            fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
                    offset, 0, UNIV_PAGE_SIZE,
                    (void*) (buf_dblwr->write_buf
                        + UNIV_PAGE_SIZE * i), NULL);
        } else {
            /* It is a regular page. Write it directly to the
               doublewrite buffer */
            fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
                    offset, 0, UNIV_PAGE_SIZE,
                    (void*) ((buf_block_t*) bpage)->frame,
                    NULL);
        }

        /* Now flush the doublewrite buffer data to disk */
        fil_flush(TRX_SYS_SPACE);

        /* We know that the write has been flushed to disk now
           and during recovery we will find it in the doublewrite buffer
           blocks. Next do the write to the intended position. */
        buf_dblwr_write_block_to_datafile(bpage, sync);
    }
}
#endif /* !UNIV_HOTBACKUP */

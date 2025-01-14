/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/ioSubmitter.h#14 $
 */

#ifndef IOSUBMITTER_H
#define IOSUBMITTER_H

#include "kernelLayer.h"
#include "kvio.h"

/**
 * Create an io_submitter structure.
 *
 * @param [in]  thread_name_prefix   The per-device prefix to use in process
 *                                   names
 * @param [in]  thread_count         Number of bio-submission threads to set up
 * @param [in]  rotation_interval    Interval to use when rotating between
 *                                   bio-submission threads when enqueuing work
 *                                   items
 * @param [in]  max_requests_active  Number of bios for merge tracking
 * @param [in]  vdo                  The vdo which will use this submitter
 * @param [out] io_submitter         Pointer to the new data structure
 *
 * @return VDO_SUCCESS or an error
 **/
int make_vdo_io_submitter(const char *thread_name_prefix,
			  unsigned int thread_count,
			  unsigned int rotation_interval,
			  unsigned int max_requests_active,
			  struct vdo *vdo,
			  struct io_submitter **io_submitter);

/**
 * Tear down the io_submitter fields as needed for a physical layer.
 *
 * @param [in]  io_submitter  The I/O submitter data to tear down (may be NULL)
 **/
void cleanup_vdo_io_submitter(struct io_submitter *io_submitter);

/**
 * Free the io_submitter fields and structure as needed for a
 * physical layer. This must be called after
 * cleanup_vdo_io_submitter(). It is used to release resources late in
 * the shutdown process to avoid or reduce the chance of race
 * conditions.
 *
 * @param [in]  io_submitter  The I/O submitter data to destroy
 **/
void free_vdo_io_submitter(struct io_submitter *io_submitter);

/**
 * Dump info to the kernel log about the work queue used by the
 * physical layer. For debugging only.
 *
 * @param [in]  io_submitter  The I/O submitter data
 **/
void vdo_dump_bio_work_queue(struct io_submitter *io_submitter);


/**
 * Enqueue a work item to run in the work queue(s) used for bio
 * submissions from the physical layer.
 *
 * Outside of io_submitter, used only for finishing processing of empty
 * flush bios by sending them to the storage device.
 *
 * @param io_submitter  The I/O submitter data to update
 * @param work_item     The new work item to run
 **/
void vdo_enqueue_bio_work_item(struct io_submitter *io_submitter,
			       struct vdo_work_item *work_item);

/**
 * Submit bio but don't block.
 *
 * Submits the bio to a helper work queue which sits in a loop
 * submitting bios. The worker thread may block if the target device
 * is busy, which is why we don't want to do the submission in the
 * original calling thread.
 *
 * The bi_private field of the bio must point to a vio associated
 * with the operation. The bi_end_io callback is invoked when the I/O
 * operation completes.
 *
 * @param bio     the block I/O operation descriptor to submit
 * @param action  the action code specifying the priority for the operation
 **/
void vdo_submit_bio(struct bio *bio, enum bio_q_action action);

#endif // IOSUBMITTER_H

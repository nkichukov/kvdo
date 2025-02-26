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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/blockAllocator.h#12 $
 */

#ifndef BLOCK_ALLOCATOR_H
#define BLOCK_ALLOCATOR_H

#include "statistics.h"
#include "types.h"
#include "vioPool.h"
#include "waitQueue.h"

/**
 * Create a block allocator.
 *
 * @param [in]  depot               The slab depot for this allocator
 * @param [in]  zone_number         The physical zone number for this allocator
 * @param [in]  thread_id           The thread ID for this allocator's zone
 * @param [in]  nonce               The nonce of the VDO
 * @param [in]  vio_pool_size       The size of the VIO pool
 * @param [in]  vdo                 The VDO
 * @param [in]  read_only_notifier  The context for entering read-only mode
 * @param [out] allocator_ptr       A pointer to hold the allocator
 *
 * @return A success or error code
 **/
int __must_check
make_vdo_block_allocator(struct slab_depot *depot,
			 zone_count_t zone_number,
			 thread_id_t thread_id,
			 nonce_t nonce,
			 block_count_t vio_pool_size,
			 struct vdo *vdo,
			 struct read_only_notifier *read_only_notifier,
			 struct block_allocator **allocator_ptr);

/**
 * Destroy a block allocator.
 *
 * @param allocator  The allocator to destroy
 **/
void free_vdo_block_allocator(struct block_allocator *allocator);

/**
 * Queue a slab for allocation or scrubbing.
 *
 * @param slab  The slab to queue
 **/
void queue_vdo_slab(struct vdo_slab *slab);

/**
 * Update the block allocator to reflect an increment or decrement of the free
 * block count in a slab. This adjusts the allocated block count and
 * reprioritizes the slab when appropriate.
 *
 * @param slab       The slab whose free block count changed
 * @param increment  True if the free block count went up by one,
 *                   false if it went down by one
 **/
void adjust_vdo_free_block_count(struct vdo_slab *slab, bool increment);

/**
 * Allocate a physical block.
 *
 * The block allocated will have a provisional reference and the reference
 * must be either confirmed with a subsequent increment or vacated with a
 * subsequent decrement of the reference count.
 *
 * @param [in]  allocator         The block allocator
 * @param [out] block_number_ptr  A pointer to receive the allocated block
 *                                number
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check allocate_vdo_block(struct block_allocator *allocator,
				    physical_block_number_t *block_number_ptr);

/**
 * Release an unused provisional reference.
 *
 * @param allocator  The block allocator
 * @param pbn        The block to dereference
 * @param why        Why the block was referenced (for logging)
 **/
void release_vdo_block_reference(struct block_allocator *allocator,
				 physical_block_number_t pbn,
				 const char *why);

/**
 * Get the number of allocated blocks, which is the total number of
 * blocks in all slabs that have a non-zero reference count.
 *
 * @param allocator  The block allocator
 *
 * @return The number of blocks with a non-zero reference count
 **/
block_count_t __must_check
get_vdo_allocated_blocks(const struct block_allocator *allocator);

/**
 * Get the number of unrecovered slabs.
 *
 * @param allocator  The block allocator
 *
 * @return The number of slabs that are unrecovered
 **/
block_count_t __must_check
get_vdo_unrecovered_slab_count(const struct block_allocator *allocator);

/**
 * Load the state of an allocator from disk.
 *
 * <p>Implements vdo_zone_action.
 **/
void load_vdo_block_allocator(void *context,
			      zone_count_t zone_number,
			      struct vdo_completion *parent);

/**
 * Inform a block allocator that its slab journals have been recovered from the
 * recovery journal.
 *
 * @param allocator  The allocator to inform
 * @param result     The result of the recovery operation
 **/
void notify_vdo_slab_journals_are_recovered(struct block_allocator *allocator,
					    int result);

/**
 * Prepare the block allocator to come online and start allocating blocks.
 *
 * <p>Implements vdo_zone_action.
 **/
void prepare_vdo_block_allocator_to_allocate(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent);

/**
 * Register a slab with the allocator, ready for use.
 *
 * @param allocator  The allocator to use
 * @param slab       The slab in question
 **/
void register_vdo_slab_with_allocator(struct block_allocator *allocator,
				      struct vdo_slab *slab);

/**
 * Register the new slabs belonging to this allocator.
 *
 * <p>Implements vdo_zone_action.
 **/
void register_new_vdo_slabs_for_allocator(void *context,
					  zone_count_t zone_number,
					  struct vdo_completion *parent);

/**
 * Drain all allocator I/O. Depending upon the type of drain, some or all
 * dirty metadata may be written to disk. The type of drain will be determined
 * from the state of the allocator's depot.
 *
 * <p>Implements vdo_zone_action.
 **/
void drain_vdo_block_allocator(void *context,
			       zone_count_t zone_number,
			       struct vdo_completion *parent);

/**
 * Resume a quiescent allocator.
 *
 * <p>Implements vdo_zone_action.
 **/
void resume_vdo_block_allocator(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent);

/**
 * Request a commit of all dirty tail blocks which are locking a given recovery
 * journal block.
 *
 * <p>Implements vdo_zone_action.
 **/
void release_vdo_tail_block_locks(void *context,
				  zone_count_t zone_number,
				  struct vdo_completion *parent);

/**
 * Get the slab summary zone for an allocator.
 *
 * @param allocator  The allocator
 *
 * @return The slab_summary_zone for that allocator
 **/
struct slab_summary_zone * __must_check
get_vdo_slab_summary_zone(const struct block_allocator *allocator);

/**
 * Acquire a VIO from a block allocator's VIO pool (asynchronous).
 *
 * @param allocator  The allocator from which to get a VIO
 * @param waiter     The object requesting the VIO
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
acquire_vdo_block_allocator_vio(struct block_allocator *allocator,
				struct waiter *waiter);

/**
 * Return a VIO to a block allocator's VIO pool
 *
 * @param allocator  The block allocator which owns the VIO
 * @param entry      The VIO being returned
 **/
void return_vdo_block_allocator_vio(struct block_allocator *allocator,
				    struct vio_pool_entry *entry);

/**
 * Initiate scrubbing all unrecovered slabs.
 *
 * <p>Implements vdo_zone_action.
 **/
void scrub_all_unrecovered_vdo_slabs_in_zone(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent);

/**
 * Queue a waiter for a clean slab.
 *
 * @param allocator  The allocator to wait on
 * @param waiter     The waiter
 *
 * @return VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no
 *         slabs to scrub, and some other error otherwise
 **/
int __must_check enqueue_for_clean_vdo_slab(struct block_allocator *allocator,
					    struct waiter *waiter);

/**
 * Increase the scrubbing priority of a slab.
 *
 * @param slab  The slab
 **/
void increase_vdo_slab_scrubbing_priority(struct vdo_slab *slab);

/**
 * Get the statistics for this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
struct block_allocator_statistics __must_check
get_vdo_block_allocator_statistics(const struct block_allocator *allocator);

/**
 * Get the aggregated slab journal statistics for the slabs in this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
struct slab_journal_statistics __must_check
get_vdo_slab_journal_statistics(const struct block_allocator *allocator);

/**
 * Get the cumulative ref_counts statistics for the slabs in this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
struct ref_counts_statistics __must_check
get_vdo_ref_counts_statistics(const struct block_allocator *allocator);

/**
 * Dump information about a block allocator to the log for debugging.
 *
 * @param allocator  The allocator to dump
 **/
void dump_vdo_block_allocator(const struct block_allocator *allocator);

#endif // BLOCK_ALLOCATOR_H

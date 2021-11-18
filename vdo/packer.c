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
 */

#include "packer.h"

#include <linux/atomic.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"

#include "admin-state.h"
#include "allocating-vio.h"
#include "allocation-selector.h"
#include "completion.h"
#include "constants.h"
#include "compressed-block.h"
#include "compression-state.h"
#include "data-vio.h"
#include "hash-lock.h"
#include "io-submitter.h"
#include "pbn-lock.h"
#include "read-only-notifier.h"
#include "status-codes.h"
#include "thread-config.h"
#include "vdo.h"
#include "vio.h"
#include "vio-write.h"

/**
 * Check that we are on the packer thread.
 *
 * @param packer  The packer
 * @param caller  The function which is asserting
 **/
static inline void assert_on_packer_thread(struct packer *packer,
					   const char *caller)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == packer->thread_id),
			"%s() called from packer thread", caller);
}

/**
 * This returns the next bin in the free_space-sorted list.
 **/
static struct packer_bin * __must_check
vdo_next_packer_bin(const struct packer *packer, struct packer_bin *bin)
{
	if (bin->list.next == &packer->bins) {
		return NULL;
	} else {
		return list_entry(bin->list.next, struct packer_bin, list);
	}
}

/**
 * This returns the first bin in the free_space-sorted list.
 **/
static struct packer_bin * __must_check
vdo_get_packer_fullest_bin(const struct packer *packer)
{
	return (list_empty(&packer->bins) ?
		NULL :
		list_entry(packer->bins.next, struct packer_bin, list));
}

/**
 * Insert a bin to the list, which is in ascending order of free space.  Since
 * all bins are already in the list, this actually moves the bin to the correct
 * position in the list.
 *
 * @param packer  The packer
 * @param bin     The bin to move to its sorted position
 **/
static void insert_in_sorted_list(struct packer *packer,
				  struct packer_bin *bin)
{
	struct packer_bin *active_bin;

	for (active_bin = vdo_get_packer_fullest_bin(packer);
	     active_bin != NULL;
	     active_bin = vdo_next_packer_bin(packer, active_bin)) {
		if (active_bin->free_space > bin->free_space) {
			list_move_tail(&bin->list, &active_bin->list);
			return;
		}
	}

	list_move_tail(&bin->list, &packer->bins);
}

/**
 * Allocate a bin and put it into the packer's list.
 *
 * @param packer  The packer
 **/
static int __must_check make_bin(struct packer *packer)
{
	struct packer_bin *bin;
	int result = UDS_ALLOCATE_EXTENDED(struct packer_bin,
					   VDO_MAX_COMPRESSION_SLOTS,
					   struct vio *, __func__, &bin);
	if (result != VDO_SUCCESS) {
		return result;
	}

	bin->free_space = packer->bin_data_size;
	INIT_LIST_HEAD(&bin->list);
	list_add_tail(&bin->list, &packer->bins);
	return VDO_SUCCESS;
}

/**
 * Make a new block packer.
 *
 * @param [in]  vdo         The vdo to which this packer belongs
 * @param [in]  bin_count   The number of partial bins to keep in memory
 * @param [out] packer_ptr  A pointer to hold the new packer
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_make_packer(struct vdo *vdo,
		    block_count_t bin_count,
		    struct packer **packer_ptr)
{
	struct packer *packer;
	block_count_t i;

	int result = UDS_ALLOCATE(1, struct packer, __func__, &packer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	packer->thread_id = vdo->thread_config->packer_thread;
	packer->bin_data_size = (VDO_BLOCK_SIZE
				 - sizeof(struct compressed_block_header));
	packer->size = bin_count;
	packer->max_slots = VDO_MAX_COMPRESSION_SLOTS;
	INIT_LIST_HEAD(&packer->bins);
	vdo_set_admin_state_code(&packer->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	for (i = 0; i < bin_count; i++) {
		int result = make_bin(packer);

		if (result != VDO_SUCCESS) {
			vdo_free_packer(packer);
			return result;
		}
	}

	/*
	 * The canceled bin can hold up to half the number of user vios. Every
	 * canceled vio in the bin must have a canceler for which it is waiting,
	 * and any canceler will only have canceled one lock holder at a time.
	 */
	result = UDS_ALLOCATE_EXTENDED(struct packer_bin,
				       MAXIMUM_VDO_USER_VIOS / 2,
				       struct vio *, __func__,
				       &packer->canceled_bin);
	if (result != VDO_SUCCESS) {
		vdo_free_packer(packer);
		return result;
	}

	*packer_ptr = packer;
	return VDO_SUCCESS;
}

/**
 * Free a block packer.
 *
 * @param packer  The packer to free
 **/
void vdo_free_packer(struct packer *packer)
{
	struct packer_bin *bin;

	if (packer == NULL) {
		return;
	}

	while ((bin = vdo_get_packer_fullest_bin(packer)) != NULL) {
		list_del_init(&bin->list);
		UDS_FREE(bin);
	}

	UDS_FREE(UDS_FORGET(packer->canceled_bin));
	UDS_FREE(packer);
}

/**
 * Get the packer from a data_vio.
 *
 * @param data_vio  The data_vio
 *
 * @return The packer from the VDO to which the data_vio belongs
 **/
static inline struct packer *
get_packer_from_data_vio(struct data_vio *data_vio)
{
	return vdo_get_from_data_vio(data_vio)->packer;
}

/**
 * Check whether the compressed data in a data_vio will fit in a packer bin.
 *
 * @param data_vio  The data_vio
 *
 * @return <code>true</code> if the data_vio will fit in a bin
 **/
bool vdo_data_is_sufficiently_compressible(struct data_vio *data_vio)
{
	struct packer *packer = get_packer_from_data_vio(data_vio);

	return (data_vio->compression.size < packer->bin_data_size);
}

/**
 * Get the current statistics from the packer.
 *
 * @param packer  The packer to query
 *
 * @return a copy of the current statistics for the packer
 **/
struct packer_statistics vdo_get_packer_statistics(const struct packer *packer)
{
	const struct packer_statistics *stats = &packer->statistics;

	return (struct packer_statistics) {
		.compressed_fragments_written =
			READ_ONCE(stats->compressed_fragments_written),
		.compressed_blocks_written =
			READ_ONCE(stats->compressed_blocks_written),
		.compressed_fragments_in_packer =
			READ_ONCE(stats->compressed_fragments_in_packer),
	};
}

/**
 * Abort packing a data_vio.
 *
 * @param data_vio     The data_vio to abort
 **/
static void abort_packing(struct data_vio *data_vio)
{
	struct packer *packer = get_packer_from_data_vio(data_vio);

	set_vio_compression_done(data_vio);

	WRITE_ONCE(packer->statistics.compressed_fragments_in_packer,
		   packer->statistics.compressed_fragments_in_packer - 1);

	continue_write_after_compression(data_vio);
}

/**
 * Update a data_vio for which a successful compressed write has completed and
 * send it on its way.
 *
 * @param data_vio    The data_vio to release
 * @param allocation  The allocation to which the compressed block was written
 **/
static void release_compressed_write_waiter(struct data_vio *data_vio,
					    struct allocating_vio *allocation)
{
	data_vio->new_mapped = (struct zoned_pbn) {
		.pbn = allocation->allocation,
		.zone = allocation->zone,
		.state = vdo_get_state_for_slot(data_vio->compression.slot),
	};

	vdo_share_compressed_write_lock(data_vio,
					allocation->allocation_lock);
	continue_write_after_compression(data_vio);
}

/**
 * Finish a compressed block write. This callback is registered in
 * continue_after_allocation().
 *
 * @param completion  The compressed write completion
 **/
static void finish_compressed_write(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct allocating_vio *allocating_vio = as_allocating_vio(completion);
	struct data_vio *client, *next;

	assert_data_vio_in_allocated_zone(agent);

	/*
	 * Process all the non-agent waiters first to ensure that the pbn
	 * lock can not be released until all of them have had a chance to
	 * journal their increfs.
         */
	for (client = agent->compression.next_in_batch;
	     client != NULL;
	     client = next) {
		next = client->compression.next_in_batch;
		release_compressed_write_waiter(client, allocating_vio);
	}

	completion->error_handler = NULL;
	release_compressed_write_waiter(agent, allocating_vio);
}

/**********************************************************************/
static void handle_compressed_write_error(struct vdo_completion *completion)
{
	struct data_vio *agent = as_data_vio(completion);
	struct allocating_vio *allocating_vio = as_allocating_vio(completion);
	struct data_vio *client, *next;

	if (vdo_get_callback_thread_id() != allocating_vio->zone->thread_id) {
		completion->callback_thread_id =
			allocating_vio->zone->thread_id;
		vdo_continue_completion(completion, VDO_SUCCESS);
		return;
	}

	update_vio_error_stats(as_vio(completion),
			       "Completing compressed write vio for physical block %llu with error",
			       (unsigned long long) allocating_vio->allocation);

	for (client = agent->compression.next_in_batch;
	     client != NULL;
	     client = next) {
		next = client->compression.next_in_batch;
		continue_write_after_compression(client);
	}

	/*
	 * Now that we've released the batch from the packer, forget the error
	 * and continue on.
	 */
	vdo_reset_completion(completion);
	completion->error_handler = NULL;
	continue_write_after_compression(agent);
}

/**
 * Put a data_vio in a specific packer_bin in which it will definitely fit.
 *
 * @param bin       The bin in which to put the data_vio
 * @param data_vio  The data_vio to add
 **/
static void add_to_bin(struct packer_bin *bin, struct data_vio *data_vio)
{
	data_vio->compression.bin = bin;
	data_vio->compression.slot = bin->slots_used;
	bin->incoming[bin->slots_used++] = data_vio;
}

/**
 * Get the next data_vio whose compression has not been canceled from a bin.
 * Any canceled data_vios will be moved to the canceled bin.
 *
 * @param packer  The packer
 * @param bin     The bin from which to get a data_vio
 *
 * @return An uncanceled data_vio from the bin or NULL if there are none
 **/
static struct data_vio *remove_from_bin(struct packer *packer,
					struct packer_bin *bin)
{
	while (bin->slots_used > 0) {
		struct data_vio *data_vio = bin->incoming[--bin->slots_used];

		if (may_write_compressed_data_vio(data_vio)) {
			data_vio->compression.bin = NULL;
			return data_vio;
		}

		add_to_bin(packer->canceled_bin, data_vio);
	}

	/* The bin is now empty. */
	bin->free_space = packer->bin_data_size;
	return NULL;
}

/**
 * Pack a data_vio's fragment into the compressed block in which it is already
 * known to fit.
 *
 * @param compression       The agent's compression_state to pack in to
 * @param data_vio          The data_vio to pack
 * @param offset            The offset into the compressed block at which to
 *                          pack the frament
 * @param compressed_block  The compressed block which will be written out when
 *                          the batch is fully packed
 *
 * @return The new amount of space used
 **/
static block_size_t pack_fragment(struct compression_state *compression,
				  struct data_vio *data_vio,
				  block_size_t offset,
				  slot_number_t slot,
				  struct compressed_block *block)
{
	struct compression_state *to_pack = &data_vio->compression;
	char *fragment = ((struct compressed_block *) to_pack->data)->data;

	to_pack->next_in_batch = compression->next_in_batch;
	compression->next_in_batch = data_vio;
	to_pack->slot = slot;
	vdo_put_compressed_block_fragment(block,
					  slot,
					  offset,
					  fragment,
					  to_pack->size);
	return (offset + to_pack->size);
}

/**
 * The bio_end_io for a compressed block write.
 *
 * @param bio  The bio for the compressed write
 **/
static void compressed_write_end_io(struct bio *bio)
{
	struct data_vio *data_vio = vio_as_data_vio(bio->bi_private);

	vdo_count_completed_bios(bio);
	set_data_vio_allocated_zone_callback(data_vio,
					     finish_compressed_write);
	continue_data_vio(data_vio, vdo_get_bio_result(bio));
}

/**
 * Write out a bin.
 *
 * @param packer  The packer
 * @param bin     The bin to write
 **/
static void write_bin(struct packer *packer, struct packer_bin *bin)
{
	int result;
	block_size_t offset;
	slot_number_t slot = 1;
	struct compression_state *compression;
	struct compressed_block *block;
	struct data_vio *agent = remove_from_bin(packer, bin);
	struct data_vio *client;
	struct packer_statistics *stats;
	struct vdo *vdo;

	if (agent == NULL) {
		return;
	}

	compression = &agent->compression;
	compression->slot = 0;
	block = (struct compressed_block *) agent->scratch_block;
	vdo_initialize_compressed_block(block, compression->size);
	offset = compression->size;

	while ((client = remove_from_bin(packer, bin)) != NULL) {
		offset = pack_fragment(compression,
				       client,
				       offset,
				       slot++,
				       block);
	}

	/*
	 * If the batch contains only a single vio, then we save nothing by
	 * saving the compressed form. Continue processing the single vio in
	 * the batch.
	 */
	if (slot == 1) {
		abort_packing(agent);
		return;
	}

	data_vio_as_completion(agent)->error_handler =
		handle_compressed_write_error;
	vdo = vdo_get_from_data_vio(agent);
	if (vdo_is_read_only(vdo->read_only_notifier)) {
		continue_data_vio(agent, VDO_READ_ONLY);
		return;
	}

	result = prepare_data_vio_for_io(agent,
					 agent->scratch_block,
					 compressed_write_end_io,
					 REQ_OP_WRITE,
					 data_vio_as_allocating_vio(agent)->allocation);
	if (result != VDO_SUCCESS) {
		continue_data_vio(agent, result);
		return;
	}

	/*
	 * Once the compressed write is submitted, the fragments are no longer
         * in the packer, so update stats now.
	 */
	stats = &packer->statistics;
	WRITE_ONCE(stats->compressed_fragments_in_packer,
		   (stats->compressed_fragments_in_packer - slot));
	WRITE_ONCE(stats->compressed_fragments_written,
		   (stats->compressed_fragments_written + slot));
	WRITE_ONCE(stats->compressed_blocks_written,
		   stats->compressed_blocks_written + 1);

	submit_data_vio_io(agent);
}

/**
 * Add a data_vio to a bin's incoming queue, handle logical space change, and
 * call physical space processor.
 *
 * @param packer    The packer
 * @param bin       The bin to which to add the data_vio
 * @param data_vio  The data_vio to add to the bin's queue
 **/
static void add_data_vio_to_packer_bin(struct packer *packer,
				       struct packer_bin *bin,
				       struct data_vio *data_vio)
{
	/*
	 * If the selected bin doesn't have room, start a new batch to make
	 * room.
	 */
	if (bin->free_space < data_vio->compression.size) {
		write_bin(packer, bin);
	}

	add_to_bin(bin, data_vio);
	bin->free_space -= data_vio->compression.size;

	/* If we happen to exactly fill the bin, start a new batch. */
	if ((bin->slots_used == packer->max_slots) || (bin->free_space == 0)) {
		write_bin(packer, bin);
	}

	/*
	 * Now that we've finished changing the free space, restore the sort
	 * order.
	 */
	insert_in_sorted_list(packer, bin);
}

/**
 * Select the bin that should be used to pack the compressed data in a data_vio
 * with other data_vios.
 *
 * @param packer    The packer
 * @param data_vio  The data_vio
 **/
static struct packer_bin * __must_check
select_bin(struct packer *packer, struct data_vio *data_vio)
{
	/*
	 * First best fit: select the bin with the least free space that has
	 * enough room for the compressed data in the data_vio.
	 */
	struct packer_bin *fullest_bin = vdo_get_packer_fullest_bin(packer);
	struct packer_bin *bin;

	for (bin = fullest_bin; bin != NULL;
	     bin = vdo_next_packer_bin(packer, bin)) {
		if (bin->free_space >= data_vio->compression.size) {
			return bin;
		}
	}

	/*
	 * None of the bins have enough space for the data_vio. We're not
	 * allowed to create new bins, so we have to overflow one of the
	 * existing bins. It's pretty intuitive to select the fullest bin,
	 * since that "wastes" the least amount of free space in the
	 * compressed block. But if the space currently used in the fullest
	 * bin is smaller than the compressed size of the incoming block, it
	 * seems wrong to force that bin to write when giving up on
	 * compressing the incoming data_vio would likewise "waste" the
	 * least amount of free space.
	 */
	if (data_vio->compression.size
	    >= (packer->bin_data_size - fullest_bin->free_space)) {
		return NULL;
	}

	/*
	 * The fullest bin doesn't have room, but writing it out and starting a
	 * new batch with the incoming data_vio will increase the packer's free
	 * space.
	 */
	return fullest_bin;
}

/**
 * Attempt to rewrite the data in this data_vio as part of a compressed block.
 *
 * @param data_vio  The data_vio to pack
 **/
void vdo_attempt_packing(struct data_vio *data_vio)
{
	int result;
	struct packer_bin *bin;
	struct vio_compression_state state =
		get_vio_compression_state(data_vio);
	struct packer *packer = get_packer_from_data_vio(data_vio);

	assert_on_packer_thread(packer, __func__);

	result = ASSERT((state.status == VIO_COMPRESSING),
			"attempt to pack data_vio not ready for packing, state: %u",
			state.status);
	if (result != VDO_SUCCESS) {
		return;
	}

	/*
	 * Increment whether or not this data_vio will be packed or not since
	 * abort_packing() always decrements the counter.
	 */
	WRITE_ONCE(packer->statistics.compressed_fragments_in_packer,
		   packer->statistics.compressed_fragments_in_packer + 1);

	/*
	 * If packing of this data_vio is disallowed for administrative
	 * reasons, give up before making any state changes.
	 */
	if (!vdo_is_state_normal(&packer->state) ||
	    (data_vio->flush_generation < packer->flush_generation)) {
		abort_packing(data_vio);
		return;
	}

	/*
	 * The check of may_vio_block_in_packer() here will set the data_vio's
	 * compression state to VIO_PACKING if the data_vio is allowed to be
	 * compressed (if it has already been canceled, we'll fall out here).
	 * Once the data_vio is in the VIO_PACKING state, it must be guaranteed
	 * to be put in a bin before any more requests can be processed by the
	 * packer thread. Otherwise, a canceling data_vio could attempt to
	 * remove the canceled data_vio from the packer and fail to rendezvous
	 * with it (VDO-2809). We must also make sure that we will actually bin
	 * the data_vio and not give up on it as being larger than the space
	 * used in the fullest bin. Hence we must call select_bin() before
	 * calling may_vio_block_in_packer() (VDO-2826).
	 */
	bin = select_bin(packer, data_vio);
	if ((bin == NULL) || !may_vio_block_in_packer(data_vio)) {
		abort_packing(data_vio);
		return;
	}

	add_data_vio_to_packer_bin(packer, bin, data_vio);
}

/**
 * Check whether the packer has drained.
 *
 * @param packer  The packer
 **/
static void check_for_drain_complete(struct packer *packer)
{
	if (vdo_is_state_draining(&packer->state) &&
	    (packer->canceled_bin->slots_used == 0)) {
		vdo_finish_draining(&packer->state);
	}
}

/**
 * Write out all non-empty bins on behalf of a flush or suspend.
 *
 * @param packer  The packer being flushed
 **/
static void write_all_non_empty_bins(struct packer *packer)
{
	struct packer_bin *bin;

	for (bin = vdo_get_packer_fullest_bin(packer);
	     bin != NULL;
	     bin = vdo_next_packer_bin(packer, bin)) {
		write_bin(packer, bin);
		/*
		 * We don't need to re-sort the bin here since this loop will
		 * make every bin have the same amount of free space, so every
		 * ordering is sorted.
		 */
	}

	check_for_drain_complete(packer);
}

/**
 * Request that the packer flush asynchronously. All bins with at least two
 * compressed data blocks will be written out, and any solitary pending VIOs
 * will be released from the packer. While flushing is in progress, any VIOs
 * submitted to vdo_attempt_packing() will be continued immediately without
 * attempting to pack them.
 *
 * @param packer  The packer to flush
 **/
void vdo_flush_packer(struct packer *packer)
{
	assert_on_packer_thread(packer, __func__);
	if (vdo_is_state_normal(&packer->state)) {
		write_all_non_empty_bins(packer);
	}
}

/**
 * Remove a lock holder from the packer.
 *
 * @param completion  The data_vio which needs a lock held by a data_vio in the
 *                    packer. The data_vio's compression.lock_holder field will
 *                    point to the data_vio to remove.
 **/
void vdo_remove_lock_holder_from_packer(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct packer *packer = get_packer_from_data_vio(data_vio);
	struct data_vio *lock_holder;
	struct packer_bin *bin;
	slot_number_t slot;

	assert_data_vio_in_packer_zone(data_vio);

	lock_holder = UDS_FORGET(data_vio->compression.lock_holder);
	bin = lock_holder->compression.bin;
	ASSERT_LOG_ONLY((bin != NULL), "data_vio in packer has a bin");

	slot = lock_holder->compression.slot;
	bin->slots_used--;
	if (slot < bin->slots_used) {
		bin->incoming[slot] = bin->incoming[bin->slots_used];
		bin->incoming[slot]->compression.slot = slot;
	}

	lock_holder->compression.bin = NULL;
	lock_holder->compression.slot = 0;

	if (bin != packer->canceled_bin) {
		bin->free_space += lock_holder->compression.size;
		insert_in_sorted_list(packer, bin);
	}

	abort_packing(lock_holder);
	check_for_drain_complete(packer);
}

/**
 * Increment the flush generation in the packer. This will also cause the
 * packer to flush so that any VIOs from previous generations will exit the
 * packer.
 *
 * @param packer  The packer
 **/
void vdo_increment_packer_flush_generation(struct packer *packer)
{
	assert_on_packer_thread(packer, __func__);
	packer->flush_generation++;
	vdo_flush_packer(packer);
}

/**
 * Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	struct packer *packer = container_of(state, struct packer, state);

	write_all_non_empty_bins(packer);
}

/**
 * Drain the packer by preventing any more VIOs from entering the packer and
 * then flushing.
 *
 * @param packer      The packer to drain
 * @param completion  The completion to finish when the packer has drained
 **/
void vdo_drain_packer(struct packer *packer, struct vdo_completion *completion)
{
	assert_on_packer_thread(packer, __func__);
	vdo_start_draining(&packer->state,
			   VDO_ADMIN_STATE_SUSPENDING,
			   completion,
			   initiate_drain);
}

/**
 * Resume a packer which has been suspended.
 *
 * @param packer  The packer to resume
 * @param parent  The completion to finish when the packer has resumed
 **/
void vdo_resume_packer(struct packer *packer, struct vdo_completion *parent)
{
	assert_on_packer_thread(packer, __func__);
	vdo_finish_completion(parent, vdo_resume_if_quiescent(&packer->state));
}


static void dump_packer_bin(const struct packer_bin *bin, bool canceled)
{
	if (bin->slots_used == 0) {
		/* Don't dump empty bins. */
		return;
	}

	uds_log_info("    %sBin slots_used=%u free_space=%zu",
		     (canceled ? "Canceled" : ""), bin->slots_used,
		     bin->free_space);

	/*
	 * FIXME: dump vios in bin->incoming? The vios should have been dumped
	 * from the vio pool. Maybe just dump their addresses so it's clear
	 * they're here?
	 */
}

/**
 * Dump the packer, in a thread-unsafe fashion.
 *
 * @param packer  The packer
 **/
void vdo_dump_packer(const struct packer *packer)
{
	struct packer_bin *bin;

	uds_log_info("packer");
	uds_log_info("  flushGeneration=%llu state %s  packer_bin_count=%llu",
		     (unsigned long long) packer->flush_generation,
		     vdo_get_admin_state_name(&packer->state),
		     (unsigned long long) packer->size);
	for (bin = vdo_get_packer_fullest_bin(packer);
	     bin != NULL;
	     bin = vdo_next_packer_bin(packer, bin)) {
		dump_packer_bin(bin, false);
	}

	dump_packer_bin(packer->canceled_bin, true);
}

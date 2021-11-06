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

#ifndef VDO_SLAB_H
#define VDO_SLAB_H

#include <linux/list.h>

#include "permassert.h"

#include "admin-state.h"
#include "fixed-layout.h"
#include "journal-point.h"
#include "reference-operation.h"
#include "types.h"

enum slab_rebuild_status {
	VDO_SLAB_REBUILT = 0,
	VDO_SLAB_REPLAYING,
	VDO_SLAB_REQUIRES_SCRUBBING,
	VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING,
	VDO_SLAB_REBUILDING,
};

/**
 * This is the type declaration for the vdo_slab type. A vdo_slab currently
 * consists of a run of 2^23 data blocks, but that will soon change to
 * dedicate a small number of those blocks for metadata storage for the
 * reference counts and slab journal for the slab.
 **/
struct vdo_slab {
	/** A list entry to queue this slab in a block_allocator list */
	struct list_head allocq_entry;

	/** The struct block_allocator that owns this slab */
	struct block_allocator *allocator;

	/** The reference counts for the data blocks in this slab */
	struct ref_counts *reference_counts;
	/** The journal for this slab */
	struct slab_journal *journal;

	/** The slab number of this slab */
	slab_count_t slab_number;
	/**
	 * The offset in the allocator partition of the first block in this
	 * slab
	 */
	physical_block_number_t start;
	/** The offset of the first block past the end of this slab */
	physical_block_number_t end;
	/** The starting translated PBN of the slab journal */
	physical_block_number_t journal_origin;
	/** The starting translated PBN of the reference counts */
	physical_block_number_t ref_counts_origin;

	/** The administrative state of the slab */
	struct admin_state state;
	/** The status of the slab */
	enum slab_rebuild_status status;
	/** Whether the slab was ever queued for scrubbing */
	bool was_queued_for_scrubbing;

	/** The priority at which this slab has been queued for allocation */
	uint8_t priority;
};

/**
 * Convert a vdo_slab's list entry back to the vdo_slab.
 *
 * @param entry  The list entry to convert
 *
 * @return  The list entry as a vdo_slab
 **/
static inline struct vdo_slab *vdo_slab_from_list_entry(struct list_head *entry)
{
	return list_entry(entry, struct vdo_slab, allocq_entry);
}

int __must_check make_vdo_slab(physical_block_number_t slab_origin,
			       struct block_allocator *allocator,
			       physical_block_number_t translation,
			       struct recovery_journal *recovery_journal,
			       slab_count_t slab_number,
			       bool is_new,
			       struct vdo_slab **slab_ptr);

int __must_check allocate_ref_counts_for_vdo_slab(struct vdo_slab *slab);

void free_vdo_slab(struct vdo_slab *slab);

zone_count_t __must_check get_vdo_slab_zone_number(struct vdo_slab *slab);

/**
 * Check whether a slab is unrecovered.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is unrecovered
 **/
static inline bool is_unrecovered_vdo_slab(const struct vdo_slab *slab)
{
	return (slab->status != VDO_SLAB_REBUILT);
}

/**
 * Check whether a slab is being replayed into.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is replaying
 **/
static inline bool is_replaying_vdo_slab(const struct vdo_slab *slab)
{
	return (slab->status == VDO_SLAB_REPLAYING);
}

/**
 * Check whether a slab is being rebuilt.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is being rebuilt
 **/
static inline bool is_vdo_slab_rebuilding(const struct vdo_slab *slab)
{
	return (slab->status == VDO_SLAB_REBUILDING);
}

void mark_vdo_slab_replaying(struct vdo_slab *slab);

void mark_vdo_slab_unrecovered(struct vdo_slab *slab);

void open_vdo_slab(struct vdo_slab *slab);

block_count_t __must_check
get_slab_free_block_count(const struct vdo_slab *slab);

int __must_check
modify_vdo_slab_reference_count(struct vdo_slab *slab,
				const struct journal_point *journal_point,
				struct reference_operation operation);

int __must_check
vdo_acquire_provisional_reference(struct vdo_slab *slab,
				  physical_block_number_t pbn,
				  struct pbn_lock *lock);

int __must_check
vdo_slab_block_number_from_pbn(struct vdo_slab *slab,
			       physical_block_number_t physical_block_number,
			       slab_block_number *slab_block_number_ptr);

bool __must_check should_save_fully_built_vdo_slab(const struct vdo_slab *slab);

void start_vdo_slab_action(struct vdo_slab *slab,
			   const struct admin_state_code *operation,
			   struct vdo_completion *parent);

void notify_vdo_slab_journal_is_loaded(struct vdo_slab *slab, int result);

bool __must_check is_vdo_slab_open(struct vdo_slab *slab);

bool __must_check is_vdo_slab_draining(struct vdo_slab *slab);

void check_if_vdo_slab_drained(struct vdo_slab *slab);

void notify_vdo_slab_ref_counts_are_drained(struct vdo_slab *slab, int result);

bool __must_check is_vdo_slab_resuming(struct vdo_slab *slab);

void finish_scrubbing_vdo_slab(struct vdo_slab *slab);

void dump_vdo_slab(const struct vdo_slab *slab);

#endif /* VDO_SLAB_H */

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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/slabJournalEraser.c#17 $
 */

#include "slabJournalEraser.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "extent.h"
#include "slab.h"
#include "slabDepot.h"
#include "vdo.h"

struct slab_journal_eraser {
	struct vdo_completion *parent;
	struct vdo_extent *extent;
	char *zero_buffer;
	struct slab_iterator slabs;
};

/**
 * Free the eraser and finish the parent.
 *
 * @param eraser    The eraser that is done
 * @param result    The result to return to the parent
 **/
static void finish_erasing(struct slab_journal_eraser *eraser, int result)
{
	struct vdo_completion *parent = eraser->parent;
	free_vdo_extent(UDS_FORGET(eraser->extent));
	UDS_FREE(eraser->zero_buffer);
	UDS_FREE(eraser);
	finish_vdo_completion(parent, result);
}

/**
 * Finish erasing slab journals with an error.
 *
 * @param completion   A completion whose parent is the eraser
 **/
static void handle_erasing_error(struct vdo_completion *completion)
{
	struct slab_journal_eraser *eraser = completion->parent;
	finish_erasing(eraser, eraser->extent->completion.result);
}

/**
 * Erase the next slab journal.
 *
 * @param extent_completion  A completion whose parent is the eraser
 **/
static void erase_next_slab_journal(struct vdo_completion *extent_completion)
{
	struct vdo_slab *slab;
	struct slab_journal_eraser *eraser = extent_completion->parent;

	if (!vdo_has_next_slab(&eraser->slabs)) {
		finish_erasing(eraser, VDO_SUCCESS);
		return;
	}

	slab = vdo_next_slab(&eraser->slabs);
	write_vdo_metadata_extent(eraser->extent, slab->journal_origin);
}

/**********************************************************************/
void erase_vdo_slab_journals(struct slab_depot *depot,
			     struct slab_iterator slabs,
			     struct vdo_completion *parent)
{
	struct slab_journal_eraser *eraser;
	block_count_t journal_size;
	struct vdo_completion *extent_completion;

	int result = UDS_ALLOCATE(1, struct slab_journal_eraser, __func__, &eraser);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	eraser->parent = parent;
	eraser->slabs = slabs;

	journal_size = get_vdo_slab_config(depot)->slab_journal_blocks;
	result = UDS_ALLOCATE(journal_size * VDO_BLOCK_SIZE,
			      char,
			      __func__,
			      &eraser->zero_buffer);
	if (result != VDO_SUCCESS) {
		finish_erasing(eraser, result);
		return;
	}

	result = create_vdo_extent(parent->vdo,
				   VIO_TYPE_SLAB_JOURNAL,
				   VIO_PRIORITY_METADATA,
				   journal_size,
				   eraser->zero_buffer,
				   &eraser->extent);
	if (result != VDO_SUCCESS) {
		finish_erasing(eraser, result);
		return;
	}

	extent_completion = &eraser->extent->completion;
	prepare_vdo_completion(extent_completion,
			       erase_next_slab_journal,
			       handle_erasing_error,
			       vdo_get_callback_thread_id(),
			       eraser);
	erase_next_slab_journal(extent_completion);
}

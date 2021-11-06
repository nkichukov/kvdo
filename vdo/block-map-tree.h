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

#ifndef BLOCK_MAP_TREE_H
#define BLOCK_MAP_TREE_H

#include <linux/list.h>

#include "block-map-format.h"
#include "block-map-page.h"
#include "constants.h"
#include "kernel-types.h"
#include "types.h"
#include "wait-queue.h"

/** A single page of a block map tree */
struct tree_page {
	/** struct waiter for a VIO to write out this page */
	struct waiter waiter;

	/** Dirty list entry */
	struct list_head entry;

	/**
	 * If this is a dirty tree page, the tree zone flush generation in which
	 * it was last dirtied.
	 */
	uint8_t generation;

	/** Whether this page is an interior tree page being written out. */
	bool writing;

	/**
	 * If this page is being written, the tree zone flush generation of the
	 * copy of the page being written.
	 **/
	uint8_t writing_generation;

	/**
	 * The earliest journal block containing uncommitted updates to this
	 * page
	 */
	sequence_number_t recovery_lock;

	/**
	 * The value of recovery_lock when the this page last started writing
	 */
	sequence_number_t writing_recovery_lock;

	/** The buffer to hold the on-disk representation of this page */
	char page_buffer[VDO_BLOCK_SIZE];
};

/**
 * An invalid PBN used to indicate that the page holding the location of a
 * tree root has been "loaded".
 **/
extern const physical_block_number_t VDO_INVALID_PBN;

/**
 * Extract the block_map_page from a tree_page.
 *
 * @param tree_page  The tree_page
 *
 * @return The block_map_page of the tree_page
 **/
static inline struct block_map_page * __must_check
as_vdo_block_map_page(struct tree_page *tree_page)
{
	return (struct block_map_page *) tree_page->page_buffer;
}

bool vdo_copy_valid_page(char *buffer, nonce_t nonce,
			 physical_block_number_t pbn,
			 struct block_map_page *page);

int __must_check vdo_initialize_tree_zone(struct block_map_zone *zone,
					  struct vdo *vdo,
					  block_count_t maximum_age);

void vdo_uninitialize_block_map_tree_zone(struct block_map_tree_zone *tree_zone);

void vdo_set_tree_zone_initial_period(struct block_map_tree_zone *tree_zone,
				      sequence_number_t period);

bool __must_check vdo_is_tree_zone_active(struct block_map_tree_zone *zone);

void vdo_advance_zone_tree_period(struct block_map_tree_zone *zone,
				  sequence_number_t period);

void vdo_drain_zone_trees(struct block_map_tree_zone *zone);

void vdo_lookup_block_map_pbn(struct data_vio *data_vio);

physical_block_number_t vdo_find_block_map_page_pbn(struct block_map *map,
						    page_number_t page_number);

void vdo_write_tree_page(struct tree_page *page, struct block_map_tree_zone *zone);

#endif /* BLOCK_MAP_TREE_H */

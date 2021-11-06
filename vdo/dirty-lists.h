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

#ifndef DIRTY_LISTS_H
#define DIRTY_LISTS_H

#include <linux/list.h>

#include "types.h"

/**
 * A collection of lists of dirty elements ordered by age. An element is always
 * placed on the oldest list in which it was dirtied (moving between lists or
 * removing altogether is cheap). Whenever the current period is advanced, any
 * elements older than the maxium age are expired. If an element is to be added
 * with a dirty age older than the maximum age, it is expired immediately.
 **/
struct dirty_lists;

/**
 * A function which will be called with a ring of dirty elements which have
 * been expired. All of the expired elements must be removed from the ring
 * before this function returns.
 *
 * @param expired  The list of expired elements
 * @param context  The context for the callback
 **/
typedef void vdo_dirty_callback(struct list_head *expired, void *context);

int __must_check make_vdo_dirty_lists(block_count_t maximum_age,
				      vdo_dirty_callback *callback,
				      void *context,
				      struct dirty_lists **dirty_lists_ptr);

void set_vdo_dirty_lists_current_period(struct dirty_lists *dirty_lists,
					sequence_number_t period);

void add_to_vdo_dirty_lists(struct dirty_lists *dirty_lists,
			    struct list_head *entry,
			    sequence_number_t old_period,
			    sequence_number_t new_period);

void advance_vdo_dirty_lists_period(struct dirty_lists *dirty_lists,
				    sequence_number_t period);

void flush_vdo_dirty_lists(struct dirty_lists *dirty_lists);

#endif /* DIRTY_LISTS_H */

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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/deadlockQueue.c#3 $
 */

#include "deadlockQueue.h"

/**********************************************************************/
void initialize_vdo_deadlock_queue(struct deadlock_queue *queue)
{
	spin_lock_init(&queue->lock);
	bio_list_init(&queue->list);
}

/**********************************************************************/
void add_to_vdo_deadlock_queue(struct deadlock_queue *queue,
			       struct bio *bio,
			       uint64_t arrival_jiffies)
{
	spin_lock(&queue->lock);
	if (bio_list_empty(&queue->list)) {
		/*
		 * If we get more than one pending at once, this will be
		 * inaccurate for some of them. Oh well. If we've gotten here,
		 * we're trying to avoid a deadlock; stats are a secondary
		 * concern.
		 */
		queue->arrival_jiffies = arrival_jiffies;
	}
	bio_list_add(&queue->list, bio);
	spin_unlock(&queue->lock);
}

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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/lockCounter.h#7 $
 */

#ifndef LOCK_COUNTER_H
#define LOCK_COUNTER_H

#include "completion.h"
#include "types.h"

/**
 * A lock_counter provides a set of shared reference count locks which is safe
 * across multiple zones with a minimum of cross-thread synchronization
 * operations. For each lock in the set, it maintains a set of per-zone lock
 * counts, and a single, atomic count of the number of zones holding locks.
 * Whenever a zone's individual counter for a lock goes from 0 to 1, the
 * zone count for that lock is incremented. Whenever a zone's individual
 * counter for a lock goes from 1 to 0, the zone count for that lock is
 * decremented. If the zone count goes to 0, and the lock counter's
 * completion is not in use, the completion is launched to inform the counter's
 * owner that some lock has been released. It is the owner's responsibility to
 * check for which locks have been released, and to inform the lock counter
 * that it has received the notification by calling
 * acknowledge_vdo_lock_unlock().
 **/

/**
 * Create a lock counter.
 *
 * @param [in]  vdo               The VDO
 * @param [in]  parent            The parent to notify when the lock count goes
 *                                to zero
 * @param [in]  callback          The function to call when the lock count goes
 *                                to zero
 * @param [in]  thread_id         The id of thread on which to run the callback
 * @param [in]  logical_zones     The total number of logical zones
 * @param [in]  physical_zones    The total number of physical zones
 * @param [in]  locks             The number of locks
 * @param [out] lock_counter_ptr  A pointer to hold the new counter
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_vdo_lock_counter(struct vdo *vdo,
				       void *parent,
				       vdo_action callback,
				       thread_id_t thread_id,
				       zone_count_t logical_zones,
				       zone_count_t physical_zones,
				       block_count_t locks,
				       struct lock_counter **lock_counter_ptr);

/**
 * Free a lock counter.
 *
 * @param counter  The lock counter to free
 **/
void free_vdo_lock_counter(struct lock_counter *counter);

/**
 * Check whether a lock is locked for a zone type. If the recovery journal has
 * a lock on the lock number, both logical and physical zones are considered
 * locked.
 *
 * @param lock_counter  The set of locks to check
 * @param lock_number   The lock to check
 * @param zone_type     The type of the zone
 *
 * @return <code>true</code> if the specified lock has references (is locked)
 **/
bool __must_check is_vdo_lock_locked(struct lock_counter *lock_counter,
				     block_count_t lock_number,
				     enum vdo_zone_type zone_type);

/**
 * Initialize the value of the journal zone's counter for a given lock. This
 * must be called from the journal zone.
 *
 * @param counter      The counter to initialize
 * @param lock_number  Which lock to initialize
 * @param value        The value to set
 **/
void initialize_vdo_lock_count(struct lock_counter *counter,
			       block_count_t lock_number,
			       uint16_t value);

/**
 * Acquire a reference to a given lock in the specified zone. This method must
 * not be used from the journal zone.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to increment
 * @param zone_type    The type of the zone acquiring the reference
 * @param zone_id      The ID of the zone acquiring the reference
 **/
void acquire_vdo_lock_count_reference(struct lock_counter *counter,
				      block_count_t lock_number,
				      enum vdo_zone_type zone_type,
				      zone_count_t zone_id);

/**
 * Release a reference to a given lock in the specified zone. This method
 * must not be used from the journal zone.
 *
 * @param counter      The lock_counter
 * @param lock_number  Which lock to increment
 * @param zone_type    The type of the zone releasing the reference
 * @param zone_id      The ID of the zone releasing the reference
 **/
void release_vdo_lock_count_reference(struct lock_counter *counter,
				      block_count_t lock_number,
				      enum vdo_zone_type zone_type,
				      zone_count_t zone_id);

/**
 * Release a single journal zone reference from the journal zone. This method
 * must be called from the journal zone.
 *
 * @param counter      The counter from which to release a reference
 * @param lock_number  The lock from which to release a reference
 **/
void release_vdo_journal_zone_reference(struct lock_counter *counter,
					block_count_t lock_number);

/**
 * Release a single journal zone reference from any zone. This method shouldn't
 * be called from the journal zone as it would be inefficient; use
 * release_vdo_journal_zone_reference() instead.
 *
 * @param counter      The counter from which to release a reference
 * @param lock_number  The lock from which to release a reference
 **/
void
release_vdo_journal_zone_reference_from_other_zone(struct lock_counter *counter,
						   block_count_t lock_number);

/**
 * Inform a lock counter that an unlock notification was received by the
 * caller.
 *
 * @param counter  The counter to inform
 **/
void acknowledge_vdo_lock_unlock(struct lock_counter *counter);

/**
 * Prevent the lock counter from issuing notifications.
 *
 * @param counter  The counter
 *
 * @return <code>true</code> if the lock counter was not notifying and hence
 *         the suspend was efficacious
 **/
bool __must_check suspend_vdo_lock_counter(struct lock_counter *counter);

/**
 * Re-allow notifications from a suspended lock counter.
 *
 * @param counter  The counter
 *
 * @return <code>true</code> if the lock counter was suspended
 **/
bool __must_check resume_vdo_lock_counter(struct lock_counter *counter);

#endif // LOCK_COUNTER_H

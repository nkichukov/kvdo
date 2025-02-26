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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/waitQueue.h#8 $
 */

#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include "common.h"

/**
 * A wait queue is a circular list of entries waiting to be notified of a
 * change in a condition. Keeping a circular list allows the queue structure
 * to simply be a pointer to the tail (newest) entry in the queue, supporting
 * constant-time enqueue and dequeue operations. A null pointer is an empty
 * queue.
 *
 *   An empty queue:
 *     queue0.last_waiter -> NULL
 *
 *   A singleton queue:
 *     queue1.last_waiter -> entry1 -> entry1 -> [...]
 *
 *   A three-element queue:
 *     queue2.last_waiter -> entry3 -> entry1 -> entry2 -> entry3 -> [...]
 **/

struct waiter;

struct wait_queue {
	/** The tail of the queue, the last (most recently added) entry */
	struct waiter *last_waiter;
	/** The number of waiters currently in the queue */
	size_t queue_length;
};

/**
 * Callback type for functions which will be called to resume processing of a
 * waiter after it has been removed from its wait queue.
 **/
typedef void waiter_callback(struct waiter *waiter, void *context);

/**
 * Method type for waiter matching methods.
 *
 * A waiter_match method returns false if the waiter does not match.
 **/
typedef bool waiter_match(struct waiter *waiter, void *context);

/**
 * The queue entry structure for entries in a wait_queue.
 **/
struct waiter {
	/**
	 * The next waiter in the queue. If this entry is the last waiter, then
	 * this is actually a pointer back to the head of the queue.
	 **/
	struct waiter *next_waiter;

	/**
	 * Optional waiter-specific callback to invoke when waking this waiter.
	 */
	waiter_callback *callback;
};

/**
 * Check whether a waiter is waiting.
 *
 * @param waiter  The waiter to check
 *
 * @return <code>true</code> if the waiter is on some wait_queue
 **/
static inline bool is_waiting(struct waiter *waiter)
{
	return (waiter->next_waiter != NULL);
}

/**
 * Initialize a wait queue.
 *
 * @param queue  The queue to initialize
 **/
static inline void initialize_wait_queue(struct wait_queue *queue)
{
	*queue = (struct wait_queue) {
		.last_waiter = NULL,
		.queue_length = 0,
	};
}

/**
 * Check whether a wait queue has any entries waiting in it.
 *
 * @param queue  The queue to query
 *
 * @return <code>true</code> if there are any waiters in the queue
 **/
static inline bool __must_check has_waiters(const struct wait_queue *queue)
{
	return (queue->last_waiter != NULL);
}

/**
 * Add a waiter to the tail end of a wait queue. The waiter must not already
 * be waiting in a queue.
 *
 * @param queue     The queue to which to add the waiter
 * @param waiter    The waiter to add to the queue
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
enqueue_waiter(struct wait_queue *queue, struct waiter *waiter);

/**
 * Notify all the entries waiting in a queue to continue execution by invoking
 * a callback function on each of them in turn. The queue is copied and
 * emptied before invoking any callbacks, and only the waiters that were in
 * the queue at the start of the call will be notified.
 *
 * @param queue     The wait queue containing the waiters to notify
 * @param callback  The function to call to notify each waiter, or NULL
 *                  to invoke the callback field registered in each waiter
 * @param context   The context to pass to the callback function
 **/
void notify_all_waiters(struct wait_queue *queue, waiter_callback *callback,
			void *context);

/**
 * Notify the next entry waiting in a queue to continue execution by invoking
 * a callback function on it after removing it from the queue.
 *
 * @param queue     The wait queue containing the waiter to notify
 * @param callback  The function to call to notify the waiter, or NULL
 *                  to invoke the callback field registered in the waiter
 * @param context   The context to pass to the callback function
 *
 * @return <code>true</code> if there was a waiter in the queue
 **/
bool notify_next_waiter(struct wait_queue *queue, waiter_callback *callback,
			void *context);

/**
 * Transfer all waiters from one wait queue to a second queue, emptying the
 * first queue.
 *
 * @param from_queue  The queue containing the waiters to move
 * @param to_queue    The queue that will receive the waiters from the
 *                    the first queue
 **/
void transfer_all_waiters(struct wait_queue *from_queue,
			  struct wait_queue *to_queue);

/**
 * Return the waiter that is at the head end of a wait queue.
 *
 * @param queue  The queue from which to get the first waiter
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
struct waiter *get_first_waiter(const struct wait_queue *queue);

/**
 * Remove all waiters that match based on the specified matching method and
 * append them to a wait_queue.
 *
 * @param queue          The wait queue to process
 * @param match_method   The method to determine matching
 * @param match_context  Contextual info for the match method
 * @param matched_queue  A wait_queue to store matches
 *
 * @return VDO_SUCCESS or an error code
 **/
int dequeue_matching_waiters(struct wait_queue *queue,
			     waiter_match *match_method,
			     void *match_context,
			     struct wait_queue *matched_queue);

/**
 * Remove the first waiter from the head end of a wait queue. The caller will
 * be responsible for waking the waiter by invoking the correct callback
 * function to resume its execution.
 *
 * @param queue  The wait queue from which to remove the first entry
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
struct waiter *dequeue_next_waiter(struct wait_queue *queue);

/**
 * Count the number of waiters in a wait queue.
 *
 * @param queue  The wait queue to query
 *
 * @return the number of waiters in the queue
 **/
static inline size_t __must_check count_waiters(const struct wait_queue *queue)
{
	return queue->queue_length;
}

/**
 * Get the waiter after this one, for debug iteration.
 *
 * @param queue   The wait queue
 * @param waiter  A waiter
 *
 * @return the next waiter, or NULL
 **/
const struct waiter * __must_check
get_next_waiter(const struct wait_queue *queue, const struct waiter *waiter);

#endif // WAIT_QUEUE_H

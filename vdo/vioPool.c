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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vioPool.c#14 $
 */

#include "vioPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "constants.h"
#include "vdo.h"
#include "vio.h"
#include "types.h"

/**
 * An vio_pool is a collection of preallocated vios.
 **/
struct vio_pool {
	/** The number of objects managed by the pool */
	size_t size;
	/** The list of objects which are available */
	struct list_head available;
	/** The queue of requestors waiting for objects from the pool */
	struct wait_queue waiting;
	/** The number of objects currently in use */
	size_t busy_count;
	/** The list of objects which are in use */
	struct list_head busy;
	/** The ID of the thread on which this pool may be used */
	thread_id_t thread_id;
	/** The buffer backing the pool's vios */
	char *buffer;
	/** The pool entries */
	struct vio_pool_entry entries[];
};

/**********************************************************************/
int make_vio_pool(struct vdo *vdo,
		  size_t pool_size,
		  thread_id_t thread_id,
		  vio_constructor *constructor,
		  void *context,
		  struct vio_pool **pool_ptr)
{
	struct vio_pool *pool;
	char *ptr;
	size_t i;

	int result = UDS_ALLOCATE_EXTENDED(struct vio_pool, pool_size,
					   struct vio_pool_entry, __func__,
					   &pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	pool->thread_id = thread_id;
	INIT_LIST_HEAD(&pool->available);
	INIT_LIST_HEAD(&pool->busy);

	result = UDS_ALLOCATE(pool_size * VDO_BLOCK_SIZE, char,
			      "VIO pool buffer", &pool->buffer);
	if (result != VDO_SUCCESS) {
		free_vio_pool(pool);
		return result;
	}

	ptr = pool->buffer;
	for (i = 0; i < pool_size; i++) {
		struct vio_pool_entry *entry = &pool->entries[i];
		entry->buffer = ptr;
		entry->context = context;
		result = constructor(vdo, entry, ptr, &entry->vio);
		if (result != VDO_SUCCESS) {
			free_vio_pool(pool);
			return result;
		}

		ptr += VDO_BLOCK_SIZE;
		INIT_LIST_HEAD(&entry->available_entry);
		list_add_tail(&entry->available_entry, &pool->available);
		pool->size++;
	}

	*pool_ptr = pool;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vio_pool(struct vio_pool *pool)
{
	struct vio_pool_entry *entry;
	size_t i;

	if (pool == NULL) {
		return;
	}

	// Remove all available entries from the object pool.
	ASSERT_LOG_ONLY(!has_waiters(&pool->waiting),
			"VIO pool must not have any waiters when being freed");
	ASSERT_LOG_ONLY((pool->busy_count == 0),
			"VIO pool must not have %zu busy entries when being freed",
			pool->busy_count);
	ASSERT_LOG_ONLY(list_empty(&pool->busy),
			"VIO pool must not have busy entries when being freed");

	while (!list_empty(&pool->available)) {
		entry = as_vio_pool_entry(pool->available.next);
		list_del_init(pool->available.next);
		free_vio(UDS_FORGET(entry->vio));
	}

	// Make sure every vio_pool_entry has been removed.
	for (i = 0; i < pool->size; i++) {
		entry = &pool->entries[i];
		ASSERT_LOG_ONLY(list_empty(&entry->available_entry),
				"VIO Pool entry still in use: VIO is in use for physical block %llu for operation %u",
				(unsigned long long) entry->vio->physical,
				entry->vio->operation);
	}

	UDS_FREE(UDS_FORGET(pool->buffer));
	UDS_FREE(pool);
}

/**********************************************************************/
bool is_vio_pool_busy(struct vio_pool *pool)
{
	return (pool->busy_count != 0);
}

/**********************************************************************/
int acquire_vio_from_pool(struct vio_pool *pool, struct waiter *waiter)
{
	struct list_head *entry;

	ASSERT_LOG_ONLY((pool->thread_id == vdo_get_callback_thread_id()),
			"acquire from active vio_pool called from correct thread");

	if (list_empty(&pool->available)) {
		return enqueue_waiter(&pool->waiting, waiter);
	}

	pool->busy_count++;
	entry = pool->available.next;
	list_move_tail(entry, &pool->busy);
	(*waiter->callback)(waiter, as_vio_pool_entry(entry));
	return VDO_SUCCESS;
}

/**********************************************************************/
void return_vio_to_pool(struct vio_pool *pool, struct vio_pool_entry *entry)
{
	ASSERT_LOG_ONLY((pool->thread_id == vdo_get_callback_thread_id()),
			"vio pool entry returned on same thread as it was acquired");
	entry->vio->completion.error_handler = NULL;
	if (has_waiters(&pool->waiting)) {
		notify_next_waiter(&pool->waiting, NULL, entry);
		return;
	}

	list_move_tail(&entry->available_entry, &pool->available);
	--pool->busy_count;
 }

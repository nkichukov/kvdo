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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoPageCache.c#51 $
 */

#include "vdoPageCacheInternals.h"

#include <linux/ratelimit.h>

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "adminState.h"
#include "constants.h"
#include "numUtils.h"
#include "readOnlyNotifier.h"
#include "statusCodes.h"
#include "types.h"
#include "vio.h"

enum {
	LOG_INTERVAL = 4000,
	DISPLAY_INTERVAL = 100000,
};

/*
 * For adjusting VDO page cache statistic fields which are only mutated on the
 * logical zone thread. Prevents any compiler shenanigans from affecting other
 * threads reading those stats.
 */
#define ADD_ONCE(value, delta) WRITE_ONCE(value, (value) + (delta))

/**********************************************************************/
static char *get_page_buffer(struct page_info *info)
{
	struct vdo_page_cache *cache = info->cache;
	return &cache->pages[(info - cache->infos) * VDO_BLOCK_SIZE];
}

/**
 * Allocate components of the cache which require their own allocation. The
 * caller is responsible for all clean up on errors.
 *
 * @param cache     The cache being constructed
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check allocate_cache_components(struct vdo_page_cache *cache)
{
	uint64_t size = cache->page_count * (uint64_t) VDO_BLOCK_SIZE;

	int result = ALLOCATE(cache->page_count,
			      struct page_info,
			      "page infos",
			      &cache->infos);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = allocate_memory(size, VDO_BLOCK_SIZE, "cache pages",
				 &cache->pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return make_int_map(cache->page_count, 0, &cache->page_map);
}

/**
 * Initialize all page info structures and put them on the free list.
 *
 * @param cache  The cache to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
static int initialize_info(struct vdo_page_cache *cache)
{
	struct page_info *info;
	INIT_LIST_HEAD(&cache->free_list);
	for (info = cache->infos; info < cache->infos + cache->page_count;
	     ++info) {
		int result;
		info->cache = cache;
		info->state = PS_FREE;
		info->pbn = NO_PAGE;

		result = kvdo_create_metadata_vio(cache->layer, VIO_TYPE_BLOCK_MAP,
						  VIO_PRIORITY_METADATA, info,
						  get_page_buffer(info),
						  &info->vio);
		if (result != VDO_SUCCESS) {
			return result;
		}

		// The thread ID should never change.
		info->vio->completion.callback_thread_id =
			cache->zone->thread_id;

		INIT_LIST_HEAD(&info->state_entry);
		list_add_tail(&info->state_entry, &cache->free_list);
		INIT_LIST_HEAD(&info->lru_entry);
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
static void write_dirty_pages_callback(struct list_head *entry, void *context);

/**********************************************************************/
int make_vdo_page_cache(PhysicalLayer *layer,
			page_count_t page_count,
			vdo_page_read_function *read_hook,
			vdo_page_write_function *write_hook,
			size_t page_context_size,
			block_count_t maximum_age,
			struct block_map_zone *zone,
			struct vdo_page_cache **cache_ptr)
{
	struct vdo_page_cache *cache;
	int result = ASSERT(page_context_size <= MAX_PAGE_CONTEXT_SIZE,
			    "page context size %zu cannot exceed %u bytes",
			    page_context_size,
			    MAX_PAGE_CONTEXT_SIZE);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ALLOCATE(1, struct vdo_page_cache, "page cache", &cache);
	if (result != UDS_SUCCESS) {
		return result;
	}

	cache->layer = layer;
	cache->page_count = page_count;
	cache->read_hook = read_hook;
	cache->write_hook = write_hook;
	cache->zone = zone;
	cache->stats.free_pages = page_count;

	result = allocate_cache_components(cache);
	if (result != VDO_SUCCESS) {
		free_vdo_page_cache(&cache);
		return result;
	}

	result = initialize_info(cache);
	if (result != VDO_SUCCESS) {
		free_vdo_page_cache(&cache);
		return result;
	}

	result = make_dirty_lists(maximum_age, write_dirty_pages_callback,
				  cache, &cache->dirty_lists);
	if (result != VDO_SUCCESS) {
		free_vdo_page_cache(&cache);
		return result;
	}

	// initialize empty circular queues
	INIT_LIST_HEAD(&cache->lru_list);
	INIT_LIST_HEAD(&cache->outgoing_list);

	*cache_ptr = cache;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_page_cache(struct vdo_page_cache **cache_ptr)
{
	struct vdo_page_cache *cache = *cache_ptr;
	if (cache == NULL) {
		return;
	}

	if (cache->infos != NULL) {
		struct page_info *info;
		for (info = cache->infos;
		     info < cache->infos + cache->page_count;
		     ++info) {
			free_vio(&info->vio);
		}
	}

	free_dirty_lists(&cache->dirty_lists);
	free_int_map(&cache->page_map);
	FREE(cache->infos);
	FREE(cache->pages);
	FREE(cache);
	*cache_ptr = NULL;
}

/**********************************************************************/
void set_vdo_page_cache_initial_period(struct vdo_page_cache *cache,
				       sequence_number_t period)
{
	set_current_period(cache->dirty_lists, period);
}

/**********************************************************************/
void set_vdo_page_cache_rebuild_mode(struct vdo_page_cache *cache,
				     bool rebuilding)
{
	cache->rebuilding = rebuilding;
}

/**
 * Assert that a function has been called on the VDO page cache's thread.
 *
 * @param cache          the page cache
 * @param function_name  the name of the function
 **/
static inline void assert_on_cache_thread(struct vdo_page_cache *cache,
					  const char *function_name)
{
	thread_id_t thread_id = get_callback_thread_id();
	ASSERT_LOG_ONLY((thread_id == cache->zone->thread_id),
			"%s() must only be called on cache thread %d, not thread %d",
			function_name,
			cache->zone->thread_id,
			thread_id);
}

/**
 * Assert that a page cache may issue I/O.
 *
 * @param cache  the page cache
 **/
static inline void assert_io_allowed(struct vdo_page_cache *cache)
{
	ASSERT_LOG_ONLY(!is_quiescent(&cache->zone->state),
			"VDO page cache may issue I/O");
}

/**
 * Log and, if enabled, report cache pressure.
 *
 * @param cache         the page cache
 **/
static void report_cache_pressure(struct vdo_page_cache *cache)
{
	ADD_ONCE(cache->stats.cache_pressure, 1);
	if (cache->waiter_count > cache->page_count) {
		if ((cache->pressure_report % LOG_INTERVAL) == 0) {
			log_info("page cache pressure %u",
				 cache->stats.cache_pressure);
		}

		if (++cache->pressure_report >= DISPLAY_INTERVAL) {
			cache->pressure_report = 0;
		}
	}
}

/**********************************************************************/
const char *vpc_page_state_name(page_state state)
{
	int result;
	static const char *state_names[] = {
		"FREE", "INCOMING", "FAILED", "RESIDENT", "DIRTY", "OUTGOING"
	};
	STATIC_ASSERT(COUNT_OF(state_names) == PAGE_STATE_COUNT);

	result = ASSERT(state < COUNT_OF(state_names),
			"Unknown page_state value %d",
			state);
	if (result != UDS_SUCCESS) {
		return "[UNKNOWN PAGE STATE]";
	}

	return state_names[state];
}

/**
 * Update the counter associated with a given state.
 *
 * @param info   the page info to count
 * @param delta  the delta to apply to the counter
 **/
static void update_counter(struct page_info *info, int32_t delta)
{
	struct block_map_statistics *stats = &info->cache->stats;
	switch (info->state) {
	case PS_FREE:
		ADD_ONCE(stats->free_pages, delta);
		return;

	case PS_INCOMING:
		ADD_ONCE(stats->incoming_pages, delta);
		return;

	case PS_OUTGOING:
		ADD_ONCE(stats->outgoing_pages, delta);
		return;

	case PS_FAILED:
		ADD_ONCE(stats->failed_pages, delta);
		return;

	case PS_RESIDENT:
		ADD_ONCE(stats->clean_pages, delta);
		return;

	case PS_DIRTY:
		ADD_ONCE(stats->dirty_pages, delta);
		return;

	default:
		return;
	}
}

/**
 * Update the lru information for an active page.
 **/
static void update_lru(struct page_info *info)
{
	struct vdo_page_cache *cache = info->cache;

	if (cache->lru_list.prev != &info->lru_entry) {
		list_move_tail(&info->lru_entry, &cache->lru_list);
	}
}

/**
 * Set the state of a page_info and put it on the right list, adjusting
 * counters.
 *
 * @param info       the page_info to modify
 * @param new_state  the new state for the page_info
 **/
static void set_info_state(struct page_info *info, page_state new_state)
{
	if (new_state == info->state) {
		return;
	}

	update_counter(info, -1);
	info->state = new_state;
	update_counter(info, 1);

	switch (info->state) {
	case PS_FREE:
	case PS_FAILED:
		list_move_tail(&info->state_entry, &info->cache->free_list);
		return;

	case PS_OUTGOING:
		list_move_tail(&info->state_entry, &info->cache->outgoing_list);
		return;

	case PS_DIRTY:
		return;

	default:
		list_del_init(&info->state_entry);
	}
}

/**
 * Set the pbn for an info, updating the map as needed.
 *
 * @param info  The page info
 * @param pbn   The physical block number to set
 **/
static int __must_check
set_info_pbn(struct page_info *info, physical_block_number_t pbn)
{
	struct vdo_page_cache *cache = info->cache;

	// Either the new or the old page number must be NO_PAGE.
	int result = ASSERT((pbn == NO_PAGE) || (info->pbn == NO_PAGE),
			    "Must free a page before reusing it.");
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (info->pbn != NO_PAGE) {
		int_map_remove(cache->page_map, info->pbn);
	}

	info->pbn = pbn;

	if (pbn != NO_PAGE) {
		result = int_map_put(cache->page_map, pbn, info, true, NULL);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return VDO_SUCCESS;
}

/**
 * Reset page info to represent an unallocated page.
 **/
static int reset_page_info(struct page_info *info)
{
	int result = ASSERT(info->busy == 0, "VDO Page must not be busy");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(!has_waiters(&info->waiting),
			"VDO Page must not have waiters");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = set_info_pbn(info, NO_PAGE);
	set_info_state(info, PS_FREE);
	list_del_init(&info->lru_entry);
	return result;
}

/**
 * Find a free page.
 *
 * @param cache         the page cache
 *
 * @return a pointer to the page info structure (if found), NULL otherwise
 **/
static struct page_info * __must_check
find_free_page(struct vdo_page_cache *cache)
{
	struct page_info *info;
	if (cache->free_list.next == &cache->free_list) {
		return NULL;
	}
	info = page_info_from_state_entry(cache->free_list.next);
	list_del_init(&info->state_entry);
	return info;
}

/**********************************************************************/
struct page_info *vpc_find_page(struct vdo_page_cache *cache,
				physical_block_number_t pbn)
{
	if ((cache->last_found != NULL) && (cache->last_found->pbn == pbn)) {
		return cache->last_found;
	}
	cache->last_found = int_map_get(cache->page_map, pbn);
	return cache->last_found;
}

/**
 * Determine which page is least recently used.
 *
 * @param cache         the page cache structure
 *
 * @return a pointer to the info structure for a relevant page,
 *         or NULL if no such page can be found. The page can be
 *         dirty or resident.
 *
 * @note Picks the least recently used from among the non-busy entries
 *       at the front of each of the lru ring.
 *       Since whenever we mark a page busy we also put it to the end
 *       of the ring it is unlikely that the entries at the front
 *       are busy unless the queue is very short, but not impossible.
 **/
static struct page_info * __must_check
select_lru_page(struct vdo_page_cache *cache)
{
	struct list_head *lru;
	list_for_each(lru, &cache->lru_list) {
		struct page_info *info = page_info_from_lru_entry(lru);
		if ((info->busy == 0) && !is_in_flight(info)) {
			return info;
		}
	}

	return NULL;
}

/**********************************************************************/
struct block_map_statistics
get_vdo_page_cache_statistics(const struct vdo_page_cache *cache)
{
	const struct block_map_statistics *stats = &cache->stats;
	return (struct block_map_statistics) {
		.dirty_pages = READ_ONCE(stats->dirty_pages),
		.clean_pages = READ_ONCE(stats->clean_pages),
		.free_pages = READ_ONCE(stats->free_pages),
		.failed_pages = READ_ONCE(stats->failed_pages),
		.incoming_pages = READ_ONCE(stats->incoming_pages),
		.outgoing_pages = READ_ONCE(stats->outgoing_pages),

		.cache_pressure = READ_ONCE(stats->cache_pressure),
		.read_count = READ_ONCE(stats->read_count),
		.write_count = READ_ONCE(stats->write_count),
		.failed_reads = READ_ONCE(stats->failed_reads),
		.failed_writes = READ_ONCE(stats->failed_writes),
		.reclaimed = READ_ONCE(stats->reclaimed),
		.read_outgoing = READ_ONCE(stats->read_outgoing),
		.found_in_cache = READ_ONCE(stats->found_in_cache),
		.discard_required = READ_ONCE(stats->discard_required),
		.wait_for_page = READ_ONCE(stats->wait_for_page),
		.fetch_required = READ_ONCE(stats->fetch_required),
		.pages_loaded = READ_ONCE(stats->pages_loaded),
		.pages_saved = READ_ONCE(stats->pages_saved),
		.flush_count = READ_ONCE(stats->flush_count),
	};
}

// ASYNCHRONOUS INTERFACE BEYOND THIS POINT

/**
 * Helper to complete the VDO Page Completion request successfully.
 *
 * @param info            the page info representing the result page
 * @param vdo_page_comp   the VDO page completion to complete
 **/
static void complete_with_page(struct page_info *info,
			       struct vdo_page_completion *vdo_page_comp)
{
	bool available =
		vdo_page_comp->writable ? is_present(info) : is_valid(info);
	if (!available) {
		log_error_strerror(VDO_BAD_PAGE,
				   "Requested cache page %llu in state %s is not %s",
				   info->pbn,
				   vpc_page_state_name(info->state),
				   vdo_page_comp->writable ? "present" :
				   "valid");
		finish_completion(&vdo_page_comp->completion, VDO_BAD_PAGE);
		return;
	}

	vdo_page_comp->info = info;
	vdo_page_comp->ready = true;
	finish_completion(&vdo_page_comp->completion, VDO_SUCCESS);
}

/**
 * Complete a page completion with an error code. Implements WaiterCallback.
 *
 * @param waiter         The page completion, as a waiter
 * @param result_ptr     A pointer to the error code.
 **/
static void complete_waiter_with_error(struct waiter *waiter, void *result_ptr)
{
	int *result = result_ptr;
	struct vdo_page_completion *completion =
		page_completion_from_waiter(waiter);
	finish_completion(&completion->completion, *result);
}

/**
 * Complete a queue of VDO page completions with an error code.
 *
 * @param [in]      result      the error result
 * @param [in, out] queue       a pointer to the queue
 *
 * @note upon completion the queue will be empty
 **/
static void distribute_error_over_queue(int result, struct wait_queue *queue)
{
	notify_all_waiters(queue, complete_waiter_with_error, &result);
}

/**
 * Complete a page completion with a page. Implements WaiterCallback.
 *
 * @param waiter         The page completion, as a waiter
 * @param page_info      The page info to complete with
 **/
static void complete_waiter_with_page(struct waiter *waiter, void *page_info)
{
	struct page_info *info = page_info;
	struct vdo_page_completion *completion =
		page_completion_from_waiter(waiter);
	complete_with_page(info, completion);
}

/**
 * Complete a queue of VDO page completions with a page result.
 *
 * @param [in]      info        the page info describing the page
 * @param [in, out] queue       a pointer to a queue of waiters
 *
 * @return the number of pages distributed
 *
 * @note upon completion the queue will be empty
 *
 **/
static unsigned int distribute_page_over_queue(struct page_info *info,
					       struct wait_queue *queue)
{
	size_t pages;

	update_lru(info);
	pages = count_waiters(queue);

	/*
	 * Increment the busy count once for each pending completion so that
	 * this page does not stop being busy until all completions have
	 * been processed (VDO-83).
	 */
	info->busy += pages;

	notify_all_waiters(queue, complete_waiter_with_page, info);
	return pages;
}

/**
 * Set a persistent error which all requests will receive in the future.
 *
 * @param cache         the page cache
 * @param context       a string describing what triggered the error
 * @param result        the error result
 *
 * Once triggered, all enqueued completions will get this error.
 * Any future requests will result in this error as well.
 **/
static void set_persistent_error(struct vdo_page_cache *cache,
				 const char *context,
				 int result)
{
	struct page_info *info;
	// If we're already read-only, there's no need to log.
	struct read_only_notifier *notifier = cache->zone->read_only_notifier;
	if ((result != VDO_READ_ONLY) && !is_read_only(notifier)) {
		log_error_strerror(result,
				   "VDO Page Cache persistent error: %s",
				   context);
		enter_read_only_mode(notifier, result);
	}

	assert_on_cache_thread(cache, __func__);

	distribute_error_over_queue(result, &cache->free_waiters);
	cache->waiter_count = 0;

	for (info = cache->infos; info < cache->infos + cache->page_count;
	     ++info) {
		distribute_error_over_queue(result, &info->waiting);
	}
}

/**********************************************************************/
void init_vdo_page_completion(struct vdo_page_completion *page_completion,
			      struct vdo_page_cache *cache,
			      physical_block_number_t pbn,
			      bool writable,
			      void *parent,
			      vdo_action *callback,
			      vdo_action *error_handler)
{
	struct vdo_completion *completion = &page_completion->completion;
	ASSERT_LOG_ONLY((page_completion->waiter.next_waiter == NULL),
			"New page completion was not already on a wait queue");

	*page_completion = (struct vdo_page_completion) {
		.pbn = pbn,
		.writable = writable,
		.cache = cache,
	};

	initialize_completion(completion, VDO_PAGE_COMPLETION, cache->layer);
	prepare_completion(completion,
			   callback,
			   error_handler,
			   cache->zone->thread_id,
			   parent);
}

/**
 * Helper function to check that a completion represents a successfully
 * completed VDO Page Completion referring to a valid page.
 *
 * @param completion    a VDO completion
 * @param writable      whether a writable page is required
 *
 * @return the embedding completion if valid, NULL if not
 **/
static struct vdo_page_completion * __must_check
validate_completed_page(struct vdo_completion *completion, bool writable)
{
	struct vdo_page_completion *vpc = as_vdo_page_completion(completion);

	int result = ASSERT(vpc->ready, "VDO Page completion not ready");
	if (result != UDS_SUCCESS) {
		return NULL;
	}

	result = ASSERT(vpc->info != NULL,
			"VDO Page Completion must be complete");
	if (result != UDS_SUCCESS) {
		return NULL;
	}

	result = ASSERT(vpc->info->pbn == vpc->pbn,
			"VDO Page Completion pbn must be consistent");
	if (result != UDS_SUCCESS) {
		return NULL;
	}

	result = ASSERT(is_valid(vpc->info),
			"VDO Page Completion page must be valid");
	if (result != UDS_SUCCESS) {
		return NULL;
	}

	if (writable) {
		result = ASSERT(vpc->writable,
				"VDO Page Completion is writable");
		if (result != UDS_SUCCESS) {
			return NULL;
		}
	}

	return vpc;
}

/**********************************************************************/
bool is_page_cache_active(struct vdo_page_cache *cache)
{
	return ((cache->outstanding_reads != 0) ||
		(cache->outstanding_writes != 0));
}

/**
 * vio callback used when a page has been loaded.
 *
 * @param completion  A completion for the vio, the parent of which is a
 *                    page_info.
 **/
static void page_is_loaded(struct vdo_completion *completion)
{
	struct page_info *info = completion->parent;
	struct vdo_page_cache *cache = info->cache;
	assert_on_cache_thread(cache, __func__);

	set_info_state(info, PS_RESIDENT);
	distribute_page_over_queue(info, &info->waiting);

	/*
	 * Don't decrement until right before calling check_for_drain_complete()
	 * to ensure that the above work can't cause the page cache to be freed
	 * out from under us.
	 */
	cache->outstanding_reads--;
	check_for_drain_complete(cache->zone);
}

/**
 * Handle page load errors.
 *
 * @param completion  The page read vio
 **/
static void handle_load_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct page_info *info = completion->parent;
	struct vdo_page_cache *cache = info->cache;
	assert_on_cache_thread(cache, __func__);

	enter_read_only_mode(cache->zone->read_only_notifier, result);
	ADD_ONCE(cache->stats.failed_reads, 1);
	set_info_state(info, PS_FAILED);
	distribute_error_over_queue(result, &info->waiting);
	reset_page_info(info);

	/*
	 * Don't decrement until right before calling check_for_drain_complete()
	 * to ensure that the above work can't cause the page cache to be freed
	 * out from under us.
	 */
	cache->outstanding_reads--;
	check_for_drain_complete(cache->zone);
}

/**
 * Run the read hook after a page is loaded. This callback is registered in
 * launch_page_load() when there is a read hook.
 *
 * @param completion  The page load completion
 **/
static void run_read_hook(struct vdo_completion *completion)
{
	int result;
	struct page_info *info = completion->parent;
	completion->callback = page_is_loaded;
	reset_completion(completion);
	result = info->cache->read_hook(get_page_buffer(info),
					info->pbn,
					info->cache->zone,
					info->context);
	continue_completion(completion, result);
}

/**
 * Handle a read error during a read-only rebuild.
 *
 * @param completion  The page load completion
 **/
static void handle_rebuild_read_error(struct vdo_completion *completion)
{
	struct page_info *info = completion->parent;
	struct vdo_page_cache *cache = info->cache;
	assert_on_cache_thread(cache, __func__);

	// We are doing a read-only rebuild, so treat this as a successful read
	// of an uninitialized page.
	ADD_ONCE(cache->stats.failed_reads, 1);
	memset(get_page_buffer(info), 0, VDO_BLOCK_SIZE);
	reset_completion(completion);
	if (cache->read_hook != NULL) {
		run_read_hook(completion);
	} else {
		page_is_loaded(completion);
	}
}

/**
 * Begin the process of loading a page.
 *
 * @param info  the page info representing where to load the page
 * @param pbn   the absolute pbn of the desired page
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
launch_page_load(struct page_info *info, physical_block_number_t pbn)
{
	int result;
	struct vdo_page_cache *cache = info->cache;
	assert_io_allowed(cache);

	result = set_info_pbn(info, pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT((info->busy == 0), "Page is not busy before loading.");
	if (result != VDO_SUCCESS) {
		return result;
	}

	set_info_state(info, PS_INCOMING);
	cache->outstanding_reads++;
	ADD_ONCE(cache->stats.pages_loaded, 1);
	launch_read_metadata_vio(info->vio,
				 pbn,
				 (cache->read_hook != NULL) ?
				 run_read_hook : page_is_loaded,
				 (cache->rebuilding ?
				  handle_rebuild_read_error
				  : handle_load_error));
	return VDO_SUCCESS;
}

/**********************************************************************/
static void write_pages(struct vdo_completion *completion);

/**
 * Handle errors flushing the layer.
 *
 * @param completion  The flush vio
 **/
static void handle_flush_error(struct vdo_completion *completion)
{
	struct vdo_page_cache *cache =
		((struct page_info *) completion->parent)->cache;
	set_persistent_error(cache, "flush failed", completion->result);
	write_pages(completion);
}

/**
 * Attempt to save the outgoing pages by first flushing the layer.
 *
 * @param cache  The cache
 **/
static void save_pages(struct vdo_page_cache *cache)
{
	struct page_info *info;
	struct vio *vio;
	PhysicalLayer *layer;

	if ((cache->pages_in_flush > 0) || (cache->pages_to_flush == 0)) {
		return;
	}

	assert_io_allowed(cache);

	info = page_info_from_state_entry(cache->outgoing_list.next);
	cache->pages_in_flush = cache->pages_to_flush;
	cache->pages_to_flush = 0;
	ADD_ONCE(cache->stats.flush_count, 1);

	vio = info->vio;
	layer = vio->completion.layer;

	/*
	 * We must make sure that the recovery journal entries that changed
	 * these pages were successfully persisted, and thus must issue a flush
	 * before each batch of pages is written to ensure this. However, in
	 * sync mode, every journal block is written with FUA, thus guaranteeing
	 * the journal persisted already.
	 */
	if (layer->getWritePolicy(layer) != WRITE_POLICY_SYNC) {
		launch_flush(vio, write_pages, handle_flush_error);
		return;
	}

	write_pages(&vio->completion);
}

/**
 * Add a page to the outgoing list of pages waiting to be saved. Once in the
 * list, a page may not be used until it has been written out.
 *
 * @param info  The page to save
 **/
static void schedule_page_save(struct page_info *info)
{
	if (info->busy > 0) {
		info->write_status = WRITE_STATUS_DEFERRED;
		return;
	}

	info->cache->pages_to_flush++;
	info->cache->outstanding_writes++;
	set_info_state(info, PS_OUTGOING);
}

/**********************************************************************/
static void write_dirty_pages_callback(struct list_head *expired,
				       void *context)
{
	while (!list_empty(expired)) {
		struct list_head *entry = expired->next;
		list_del_init(entry);
		schedule_page_save(page_info_from_state_entry(entry));
	}

	save_pages((struct vdo_page_cache *) context);
}

/**
 * Add a page to outgoing pages waiting to be saved, and then start saving
 * pages if another save is not in progress.
 *
 * @param info  The page to save
 **/
static void launch_page_save(struct page_info *info)
{
	schedule_page_save(info);
	save_pages(info->cache);
}

/**
 * Determine whether a given vdo_page_completion (as a waiter) is requesting a
 * given page number. Implements WaiterMatch.
 *
 * @param waiter        The page completion in question
 * @param context       A pointer to the pbn of the desired page
 *
 * @return true if the page completion is for the desired page number
 **/
static bool completion_needs_page(struct waiter *waiter, void *context)
{
	physical_block_number_t *pbn = context;
	return (page_completion_from_waiter(waiter)->pbn == *pbn);
}

/**
 * Allocate a free page to the first completion in the waiting queue,
 * and any other completions that match it in page number.
 **/
static void allocate_free_page(struct page_info *info)
{
	int result;
	struct waiter *oldest_waiter;
	physical_block_number_t pbn;
	struct vdo_page_cache *cache = info->cache;
	assert_on_cache_thread(cache, __func__);

	if (!has_waiters(&cache->free_waiters)) {
		if (cache->stats.cache_pressure > 0) {
			log_info("page cache pressure relieved");
			WRITE_ONCE(cache->stats.cache_pressure, 0);
		}
		return;
	}

	result = reset_page_info(info);
	if (result != VDO_SUCCESS) {
		set_persistent_error(cache, "cannot reset page info", result);
		return;
	}

	oldest_waiter = get_first_waiter(&cache->free_waiters);
	pbn = page_completion_from_waiter(oldest_waiter)->pbn;

	// Remove all entries which match the page number in question
	// and push them onto the page info's wait queue.
	dequeue_matching_waiters(&cache->free_waiters, completion_needs_page,
				 &pbn, &info->waiting);
	cache->waiter_count -= count_waiters(&info->waiting);

	result = launch_page_load(info, pbn);
	if (result != VDO_SUCCESS) {
		distribute_error_over_queue(result, &info->waiting);
	}
}

/**
 * Begin the process of discarding a page.
 *
 * @param cache         the page cache
 *
 * @note If no page is discardable, increments a count of deferred frees so
 *       that the next release of a page which is no longer busy will kick
 *       off another discard cycle. This is an indication that the cache is
 *       not big enough.
 *
 * @note If the selected page is not dirty, immediately allocates the page
 *       to the oldest completion waiting for a free page.
 **/
static void discard_a_page(struct vdo_page_cache *cache)
{
	struct page_info *info = select_lru_page(cache);
	if (info == NULL) {
		report_cache_pressure(cache);
		return;
	}

	if (!is_dirty(info)) {
		allocate_free_page(info);
		return;
	}

	ASSERT_LOG_ONLY(!is_in_flight(info),
			"page selected for discard is not in flight");

	++cache->discard_count;
	info->write_status = WRITE_STATUS_DISCARD;
	launch_page_save(info);
}

/**
 * Helper used to trigger a discard so that the completion can get a different
 * page.
 *
 * @param vdo_page_comp   the VDO Page completion
 **/
static void
discard_page_for_completion(struct vdo_page_completion *vdo_page_comp)
{
	int result;
	struct vdo_page_cache *cache = vdo_page_comp->cache;

	++cache->waiter_count;

	result = enqueue_waiter(&cache->free_waiters, &vdo_page_comp->waiter);
	if (result != VDO_SUCCESS) {
		set_persistent_error(cache, "cannot enqueue waiter", result);
	}

	discard_a_page(cache);
}

/**
 * Helper used to trigger a discard if the cache needs another free page.
 *
 * @param cache         the page cache
 **/
static void discard_page_if_needed(struct vdo_page_cache *cache)
{
	if (cache->waiter_count > cache->discard_count) {
		discard_a_page(cache);
	}
}

/**********************************************************************/
void advance_vdo_page_cache_period(struct vdo_page_cache *cache,
				   sequence_number_t period)
{
	assert_on_cache_thread(cache, __func__);
	advance_period(cache->dirty_lists, period);
}

/**
 * Inform the cache that a write has finished (possibly with an error).
 *
 * @param info  The info structure for the page whose write just completed
 *
 * @return <code>true</code> if the page write was a discard
 **/
static bool write_has_finished(struct page_info *info)
{
	bool was_discard = (info->write_status == WRITE_STATUS_DISCARD);
	assert_on_cache_thread(info->cache, __func__);
	info->cache->outstanding_writes--;

	info->write_status = WRITE_STATUS_NORMAL;
	return was_discard;
}

/**
 * Handler for page write errors.
 *
 * @param completion  The page write vio
 **/
static void handle_page_write_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct page_info *info = completion->parent;
	struct vdo_page_cache *cache = info->cache;

	// If we're already read-only, write failures are to be expected.
	if (result != VDO_READ_ONLY) {
		static DEFINE_RATELIMIT_STATE(errorLimiter,
					      DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);

		if (__ratelimit(&errorLimiter)) {
			uds_log_error("failed to write block map page %llu",
				      info->pbn);
		}
	}

	set_info_state(info, PS_DIRTY);
	ADD_ONCE(cache->stats.failed_writes, 1);
	set_persistent_error(cache, "cannot write page", result);

	if (!write_has_finished(info)) {
		discard_page_if_needed(cache);
	}

	check_for_drain_complete(cache->zone);
}

/**
 * vio callback used when a page has been written out.
 *
 * @param completion    A completion for the vio, the parent of which
 *                      is embedded in page_info.
 **/
static void page_is_written_out(struct vdo_completion *completion)
{
	bool was_discard, reclaimed;
	uint32_t reclamations;

	struct page_info *info = completion->parent;
	struct vdo_page_cache *cache = info->cache;

	if (cache->write_hook != NULL) {
		bool rewrite = cache->write_hook(get_page_buffer(info),
						cache->zone, info->context);
		if (rewrite) {
			launch_write_metadata_vio_with_flush(info->vio,
							     info->pbn,
							     page_is_written_out,
							     handle_page_write_error,
							     true,
							     false);
			return;
		}
	}

	was_discard = write_has_finished(info);
	reclaimed = (!was_discard || (info->busy > 0) ||
		     has_waiters(&info->waiting));

	set_info_state(info, PS_RESIDENT);

	reclamations = distribute_page_over_queue(info, &info->waiting);
	ADD_ONCE(cache->stats.reclaimed, reclamations);

	if (was_discard) {
		cache->discard_count--;
	}

	if (reclaimed) {
		discard_page_if_needed(cache);
	} else {
		allocate_free_page(info);
	}

	check_for_drain_complete(cache->zone);
}

/**
 * Write the batch of pages which were covered by the layer flush which just
 * completed. This callback is registered in save_pages().
 *
 * @param flush_completion  The flush vio
 **/
static void write_pages(struct vdo_completion *flush_completion)
{
	struct vdo_page_cache *cache =
		((struct page_info *) flush_completion->parent)->cache;

	/*
	 * We need to cache these two values on the stack since in the error
	 * case below, it is possible for the last page info to cause the page
	 * cache to get freed. Hence once we launch the last page, it may be
	 * unsafe to dereference the cache [VDO-4724].
	 */
	bool has_unflushed_pages = (cache->pages_to_flush > 0);
	page_count_t pages_in_flush = cache->pages_in_flush;
	cache->pages_in_flush = 0;
	while (pages_in_flush-- > 0) {
		struct list_head *entry = cache->outgoing_list.next;
		struct page_info *info = page_info_from_state_entry(entry);
		list_del_init(entry);
		if (is_read_only(info->cache->zone->read_only_notifier)) {
			struct vdo_completion *completion =
				&info->vio->completion;
			reset_completion(completion);
			completion->callback = page_is_written_out;
			completion->error_handler = handle_page_write_error;
			finish_completion(completion, VDO_READ_ONLY);
			continue;
		}
		ADD_ONCE(info->cache->stats.pages_saved, 1);
		launch_write_metadata_vio(info->vio,
					  info->pbn,
					  page_is_written_out,
					  handle_page_write_error);
	}

	if (has_unflushed_pages) {
		// If there are unflushed pages, the cache can't have been
		// freed, so this call is safe.
		save_pages(cache);
	}
}

/**********************************************************************/
void release_vdo_page_completion(struct vdo_completion *completion)
{
	struct page_info *discard_info = NULL;
	struct vdo_page_completion *page_completion;
	struct vdo_page_cache *cache;
	if (completion == NULL) {
		return;
	}

	if (completion->result == VDO_SUCCESS) {
		page_completion = validate_completed_page(completion, false);
		if (--page_completion->info->busy == 0) {
			discard_info = page_completion->info;
		}
	} else {
		// Do not check for errors if the completion was not successful.
		page_completion = as_vdo_page_completion(completion);
	}
	ASSERT_LOG_ONLY((page_completion->waiter.next_waiter == NULL),
			"Page being released after leaving all queues");

	cache = page_completion->cache;
	assert_on_cache_thread(cache, __func__);
	memset(page_completion, 0, sizeof(struct vdo_page_completion));

	if (discard_info != NULL) {
		if (discard_info->write_status == WRITE_STATUS_DEFERRED) {
			discard_info->write_status = WRITE_STATUS_NORMAL;
			launch_page_save(discard_info);
		}
		// if there are excess requests for pages (that have not already
		// started discards) we need to discard some page (which may be
		// this one)
		discard_page_if_needed(cache);
	}
}

/**
 * Helper function to load a page as described by a VDO Page Completion.
 *
 * @param info            the page info representing where to load the page
 * @param vdo_page_comp   the VDO Page Completion describing the page
 **/
static void load_page_for_completion(struct page_info *info,
				     struct vdo_page_completion *vdo_page_comp)
{
	int result = enqueue_waiter(&info->waiting, &vdo_page_comp->waiter);
	if (result != VDO_SUCCESS) {
		finish_completion(&vdo_page_comp->completion, result);
		return;
	}

	result = launch_page_load(info, vdo_page_comp->pbn);
	if (result != VDO_SUCCESS) {
		distribute_error_over_queue(result, &info->waiting);
	}
}

/**********************************************************************/
void get_vdo_page(struct vdo_completion *completion)
{
	struct page_info *info;

	struct vdo_page_completion *vdo_page_comp =
		as_vdo_page_completion(completion);
	struct vdo_page_cache *cache = vdo_page_comp->cache;
	assert_on_cache_thread(cache, __func__);

	if (vdo_page_comp->writable &&
	    is_read_only(cache->zone->read_only_notifier)) {
		finish_completion(completion, VDO_READ_ONLY);
		return;
	}

	if (vdo_page_comp->writable) {
		ADD_ONCE(cache->stats.write_count, 1);
	} else {
		ADD_ONCE(cache->stats.read_count, 1);
	}

	info = vpc_find_page(cache, vdo_page_comp->pbn);
	if (info != NULL) {
		// The page is in the cache already.
		if ((info->write_status == WRITE_STATUS_DEFERRED) ||
		    is_incoming(info) ||
		    (is_outgoing(info) && vdo_page_comp->writable)) {
			int result;
			// The page is unusable until it has finished I/O.
			ADD_ONCE(cache->stats.wait_for_page, 1);
			result = enqueue_waiter(&info->waiting,
						&vdo_page_comp->waiter);
			if (result != VDO_SUCCESS) {
				finish_completion(&vdo_page_comp->completion,
						  result);
			}

			return;
		}

		if (is_valid(info)) {
			// The page is usable.
			ADD_ONCE(cache->stats.found_in_cache, 1);
			if (!is_present(info)) {
				ADD_ONCE(cache->stats.read_outgoing, 1);
			}
			update_lru(info);
			++info->busy;
			complete_with_page(info, vdo_page_comp);
			return;
		}
		// Something horrible has gone wrong.
		ASSERT_LOG_ONLY(false, "Info found in a usable state.");
	}

	// The page must be fetched.
	info = find_free_page(cache);
	if (info != NULL) {
		ADD_ONCE(cache->stats.fetch_required, 1);
		load_page_for_completion(info, vdo_page_comp);
		return;
	}

	// The page must wait for a page to be discarded.
	ADD_ONCE(cache->stats.discard_required, 1);
	discard_page_for_completion(vdo_page_comp);
}

/**********************************************************************/
void mark_completed_vdo_page_dirty(struct vdo_completion *completion,
				   sequence_number_t old_dirty_period,
				   sequence_number_t new_dirty_period)
{
	struct page_info *info;

	struct vdo_page_completion *vdo_page_comp =
		validate_completed_page(completion, true);
	if (vdo_page_comp == NULL) {
		return;
	}

	info = vdo_page_comp->info;
	set_info_state(info, PS_DIRTY);
	add_to_dirty_lists(info->cache->dirty_lists,
			   &info->state_entry,
			   old_dirty_period,
			   new_dirty_period);
}

/**********************************************************************/
void request_vdo_page_write(struct vdo_completion *completion)
{
	struct page_info *info;

	struct vdo_page_completion *vdo_page_comp =
		validate_completed_page(completion, true);
	if (vdo_page_comp == NULL) {
		return;
	}

	info = vdo_page_comp->info;
	set_info_state(info, PS_DIRTY);
	launch_page_save(info);
}

/**********************************************************************/
static void *dereference_page_completion(struct vdo_page_completion *completion)
{
	return ((completion != NULL) ? get_page_buffer(completion->info) : NULL);
}

/**********************************************************************/
const void *dereference_readable_vdo_page(struct vdo_completion *completion)
{
	return dereference_page_completion(
		validate_completed_page(completion, false));
}

/**********************************************************************/
void *dereference_writable_vdo_page(struct vdo_completion *completion)
{
	return dereference_page_completion(validate_completed_page(completion,
								   true));
}

/**********************************************************************/
void *get_vdo_page_completion_context(struct vdo_completion *completion)
{
	struct vdo_page_completion *page_completion =
		as_vdo_page_completion(completion);
	struct page_info *info =
		((page_completion != NULL) ? page_completion->info : NULL);
	return (((info != NULL) && is_valid(info)) ? info->context : NULL);
}

/**********************************************************************/
void drain_vdo_page_cache(struct vdo_page_cache *cache)
{
	assert_on_cache_thread(cache, __func__);
	ASSERT_LOG_ONLY(is_draining(&cache->zone->state),
			"drain_vdo_page_cache() called during block map drain");

	if (!is_suspending(&cache->zone->state)) {
		flush_dirty_lists(cache->dirty_lists);
		save_pages(cache);
	}
}

/**********************************************************************/
int invalidate_vdo_page_cache(struct vdo_page_cache *cache)
{
	struct page_info *info;
	assert_on_cache_thread(cache, __func__);

	// Make sure we don't throw away any dirty pages.
	for (info = cache->infos; info < cache->infos + cache->page_count;
	     info++) {
		int result = ASSERT(!is_dirty(info),
				    "cache must have no dirty pages");
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	// Reset the page map by re-allocating it.
	free_int_map(&cache->page_map);
	return make_int_map(cache->page_count, 0, &cache->page_map);
}

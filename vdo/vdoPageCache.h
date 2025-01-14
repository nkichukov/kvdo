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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoPageCache.h#9 $
 */

#ifndef VDO_PAGE_CACHE_H
#define VDO_PAGE_CACHE_H

#include "adminState.h"
#include "completion.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

/**
 * Structure describing page meta data (defined internally).
 **/
struct page_info;

/**
 * Structure describing entire page cache.
 **/
struct vdo_page_cache;

/**
 * Generation counter for page references.
 **/
typedef uint32_t vdo_page_generation;

/**
 * Signature for a function to call when a page is read into the cache.
 *
 * <p>If specified, this function is called when a page is fetched from disk.
 *
 * @param raw_page      The raw memory of the freshly-fetched page
 * @param pbn           The absolute physical block number of the page
 * @param zone          The block map zone to which the cache belongs
 * @param page_context  A pointer to client-specific data for the new page
 *
 * @return VDO_SUCCESS on success or VDO_BAD_PAGE if the page is incorrectly
 *         formatted
 **/
typedef int vdo_page_read_function(void *raw_page,
				   physical_block_number_t pbn,
				   struct block_map_zone *zone,
				   void *page_context);

/**
 * Signature for a function to call when a page is written from the cache.
 *
 * <p>If specified, this function is called when a page is written to disk.
 *
 * @param raw_page      The raw memory of the freshly-written page
 * @param zone          The block map zone to which the cache belongs
 * @param page_context  A pointer to client-specific data for the new page
 *
 * @return whether the page needs to be rewritten
 **/
typedef bool vdo_page_write_function(void *raw_page,
				     struct block_map_zone *zone,
				     void *page_context);

/**
 * Construct a page cache.
 *
 * @param [in]  vdo                 The vdo
 * @param [in]  page_count          The number of cache pages to hold
 * @param [in]  read_hook           The function to be called when a page is
 *                                  read into the cache
 * @param [in]  write_hook          The function to be called after a page is
 *                                  written from the cache
 * @param [in]  page_context_size   The size of the per-page context that will
 *                                  be passed to the read and write hooks
 * @param [in]  maximum_age         The number of journal blocks before a
 *                                  dirtied page is considered old and must be
 *                                  written out
 * @param [in]  zone                The block map zone which owns this cache
 * @param [out] cache_ptr           A pointer to hold the cache
 *
 * @return a success or error code
 **/
int __must_check make_vdo_page_cache(struct vdo *vdo,
				     page_count_t page_count,
				     vdo_page_read_function *read_hook,
				     vdo_page_write_function *write_hook,
				     size_t page_context_size,
				     block_count_t maximum_age,
				     struct block_map_zone *zone,
				     struct vdo_page_cache **cache_ptr);

/**
 * Free the page cache structure.
 *
 * @param cache  The cache to free
 **/
void free_vdo_page_cache(struct vdo_page_cache *cache);

/**
 * Set the initial dirty period for a page cache.
 *
 * @param cache  The cache
 * @param period The initial dirty period to set
 **/
void set_vdo_page_cache_initial_period(struct vdo_page_cache *cache,
				       sequence_number_t period);

/**
 * Switch the page cache into or out of read-only rebuild mode.
 *
 * @param cache       The cache
 * @param rebuilding  <code>true</code> if the cache should be put into
 *                    read-only rebuild mode, <code>false</code> otherwise
 **/
void set_vdo_page_cache_rebuild_mode(struct vdo_page_cache *cache,
				     bool rebuilding);

/**
 * Check whether a page cache is active (i.e. has any active lookups,
 * outstanding I/O, or pending I/O).
 *
 * @param cache  The cache to check
 *
 * @return <code>true</code> if the cache is active
 **/
bool __must_check is_vdo_page_cache_active(struct vdo_page_cache *cache);

/**
 * Advance the dirty period for a page cache.
 *
 * @param cache   The cache to advance
 * @param period  The new dirty period
 **/
void advance_vdo_page_cache_period(struct vdo_page_cache *cache,
				   sequence_number_t period);

/**
 * Write one or more batches of dirty pages.
 *
 * All writable pages in the ancient era and some number in the old era
 * are scheduled for writing.
 *
 * @param cache    the VDO page cache
 * @param batches  how many batches to write now
 * @param total    how many batches (including those being written now) remain
 *                 in this era
 **/
void write_vdo_page_cache_pages(struct vdo_page_cache *cache,
				size_t batches,
				size_t total);

/**
 * Rotate the dirty page eras.
 *
 * Move all pages in the old era to the ancient era and then move
 * the current era bin into the old era.
 *
 * @param cache   the VDO page cache
 **/
void rotate_vdo_page_cache_eras(struct vdo_page_cache *cache);

// ASYNC

/**
 * A completion awaiting a specific page.  Also a live reference into the
 * page once completed, until freed.
 **/
struct vdo_page_completion {
	/** The generic completion */
	struct vdo_completion completion;
	/** The cache involved */
	struct vdo_page_cache *cache;
	/** The waiter for the pending list */
	struct waiter waiter;
	/** The absolute physical block number of the page on disk */
	physical_block_number_t pbn;
	/** Whether the page may be modified */
	bool writable;
	/** Whether the page is available */
	bool ready;
	/** The info structure for the page, only valid when ready */
	struct page_info *info;
};

/**
 * Initialize a VDO Page Completion, requesting a particular page from the
 * cache.
 *
 * @param page_completion  The vdo_page_completion to initialize
 * @param cache            The VDO page cache
 * @param pbn              The absolute physical block of the desired page
 * @param writable         Whether the page can be modified
 * @param parent           The parent object
 * @param callback         The completion callback
 * @param error_handler    The handler for page errors
 *
 * @note Once a completion has occurred for the get_vdo_page() operation,
 *       the underlying page shall be busy (stuck in memory) until the
 *       vdo_completion returned by this operation has been released.
 **/
void init_vdo_page_completion(struct vdo_page_completion *page_completion,
			      struct vdo_page_cache *cache,
			      physical_block_number_t pbn,
			      bool writable,
			      void *parent,
			      vdo_action *callback,
			      vdo_action *error_handler);

/**
 * Release a VDO Page Completion.
 *
 * The page referenced by this completion (if any) will no longer be
 * held busy by this completion. If a page becomes discardable and
 * there are completions awaiting free pages then a new round of
 * page discarding is started.
 *
 * @param completion The completion to release
 **/
void release_vdo_page_completion(struct vdo_completion *completion);

/**
 * Asynchronous operation to get a VDO page.
 *
 * May cause another page to be discarded (potentially writing a dirty page)
 * and the one nominated by the completion to be loaded from disk.
 *
 * When the page becomes available the callback registered in the completion
 * provided is triggered. Once triggered the page is marked busy until
 * the completion is destroyed.
 *
 * @param completion    the completion initialized by
 *                      init_vdo_page_completion().
 **/
void get_vdo_page(struct vdo_completion *completion);

/**
 * Mark a VDO page referenced by a completed vdo_page_completion as dirty.
 *
 * @param completion        a VDO Page Completion whose callback has been
 *                          called
 * @param old_dirty_period  the period in which the page was already dirty (0
 *                          if it wasn't)
 * @param new_dirty_period  the period in which the page is now dirty
 **/
void mark_completed_vdo_page_dirty(struct vdo_completion *completion,
				   sequence_number_t old_dirty_period,
				   sequence_number_t new_dirty_period);

/**
 * Request that a VDO page be written out as soon as it is not busy.
 *
 * @param completion  the vdo_page_completion containing the page
 **/
void request_vdo_page_write(struct vdo_completion *completion);

/**
 * Access the raw memory for a read-only page of a completed
 * vdo_page_completion.
 *
 * @param completion    a vdo page completion whose callback has been called
 *
 * @return a pointer to the raw memory at the beginning of the page, or
 *         NULL if the page is not available.
 **/
const void *dereference_readable_vdo_page(struct vdo_completion *completion);

/**
 * Access the raw memory for a writable page of a completed
 * vdo_page_completion.
 *
 * @param completion    a vdo page completion whose callback has been called
 *
 * @return a pointer to the raw memory at the beginning of the page, or
 *         NULL if the page is not available, or if the page is read-only
 **/
void *dereference_writable_vdo_page(struct vdo_completion *completion);

/**
 * Get the per-page client context for the page in a page completion whose
 * callback has been invoked. Should only be called after dereferencing the
 * page completion to validate the page.
 *
 * @param completion    a vdo page completion whose callback has been invoked
 *
 * @return a pointer to the per-page client context, or NULL if
 *         the page is not available
 **/
void *get_vdo_page_completion_context(struct vdo_completion *completion);

/**
 * Drain I/O for a page cache.
 *
 * @param cache  The cache to drain
 **/
void drain_vdo_page_cache(struct vdo_page_cache *cache);

/**
 * Invalidate all entries in the VDO page cache. There must not be any
 * dirty pages in the cache.
 *
 * @param cache  the cache to invalidate
 *
 * @return a success or error code
 **/
int __must_check invalidate_vdo_page_cache(struct vdo_page_cache *cache);

// STATISTICS & TESTING

/**
 * Get current cache statistics.
 *
 * @param cache  the page cache
 *
 * @return the statistics
 **/
struct block_map_statistics __must_check
get_vdo_page_cache_statistics(const struct vdo_page_cache *cache);

#endif // VDO_PAGE_CACHE_H

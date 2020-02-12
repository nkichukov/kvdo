/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoPageCache.h#10 $
 */

#ifndef VDO_PAGE_CACHE_H
#define VDO_PAGE_CACHE_H

#include "adminState.h"
#include "atomic.h"
#include "completion.h"
#include "types.h"
#include "waitQueue.h"

/**
 * Structure describing page meta data (defined internally).
 **/
struct page_info;

/**
 * Structure describing entire page cache.
 * (Unfortunately the name "PageCache" is already taken by Albireo.)
 **/
struct vdo_page_cache;

/**
 * Generation counter for page references.
 **/
typedef uint32_t VDOPageGeneration;

/**
 * Page-state count statistics sub-structure.
 **/
struct atomic_page_state_counts {
  /* free pages */
  Atomic64 freePages;
  /* clean (resident) pages */
  Atomic64 cleanPages;
  /* dirty pages per era */
  Atomic64 dirtyPages;
  /* pages incoming */
  Atomic64 incomingPages;
  /* pages outgoing */
  Atomic64 outgoingPages;
  /* pages in failed state */
  Atomic64 failedPages;
};

/**
 * Statistics and debugging fields for the page cache.
 */
struct atomic_page_cache_statistics {
  /* counts of how many pages are in each state */
  struct atomic_page_state_counts counts;
  /* how many times free page not available */
  Atomic64                        cachePressure;
  /* number of getVDOPageAsync() for read */
  Atomic64                        readCount;
  /* number or getVDOPageAsync() for write */
  Atomic64                        writeCount;
  /* number of times pages failed to read */
  Atomic64                        failedReads;
  /* number of times pages failed to write */
  Atomic64                        failedWrites;
  /* number of gets that are reclaimed */
  Atomic64                        reclaimed;
  /* number of gets for outgoing pages */
  Atomic64                        readOutgoing;
  /* number of gets that were already there */
  Atomic64                        foundInCache;
  /* number of gets requiring discard */
  Atomic64                        discardRequired;
  /* number of gets enqueued for their page */
  Atomic64                        waitForPage;
  /* number of gets that have to fetch */
  Atomic64                        fetchRequired;
  /* number of page fetches */
  Atomic64                        pagesLoaded;
  /* number of page saves */
  Atomic64                        pagesSaved;
  /* number of flushes initiated */
  Atomic64                        flushCount;
};

/**
 * Signature for a function to call when a page is read into the cache.
 *
 * <p>If specified, this function is called when a page is fetched from disk.
 *
 * @param rawPage      The raw memory of the freshly-fetched page
 * @param pbn          The absolute physical block number of the page
 * @param zone         The block map zone to which the cache belongs
 * @param pageContext  A pointer to client-specific data for the new page
 *
 * @return VDO_SUCCESS on success or VDO_BAD_PAGE if the page is incorrectly
 *         formatted
 **/
typedef int VDOPageReadFunction(void                  *rawPage,
                                PhysicalBlockNumber    pbn,
                                struct block_map_zone *zone,
                                void                  *pageContext);

/**
 * Signature for a function to call when a page is written from the cache.
 *
 * <p>If specified, this function is called when a page is written to disk.
 *
 * @param rawPage      The raw memory of the freshly-written page
 * @param zone         The block map zone to which the cache belongs
 * @param pageContext  A pointer to client-specific data for the new page
 *
 * @return whether the page needs to be rewritten
 **/
typedef bool VDOPageWriteFunction(void                  *rawPage,
                                  struct block_map_zone *zone,
                                  void                  *pageContext);

/**
 * Construct a PageCache.
 *
 * @param [in]  layer             The physical layer to read and write
 * @param [in]  pageCount         The number of cache pages to hold
 * @param [in]  readHook          The function to be called when a page is read
 *                                into the cache
 * @param [in]  writeHook         The function to be called after a page is
 *                                written from the cache
 * @param [in]  pageContextSize   The size of the per-page context that will be
 *                                passed to the read and write hooks
 * @param [in]  maximumAge        The number of journal blocks before a dirtied
 *                                page is considered old and must be written
 *                                out
 * @param [in]  zone              The block map zone which owns this cache
 * @param [out] cachePtr          A pointer to hold the cache
 *
 * @return a success or error code
 **/
int makeVDOPageCache(PhysicalLayer          *layer,
                     PageCount               pageCount,
                     VDOPageReadFunction    *readHook,
                     VDOPageWriteFunction   *writeHook,
                     size_t                  pageContextSize,
                     BlockCount              maximumAge,
                     struct block_map_zone  *zone,
                     struct vdo_page_cache **cachePtr)
  __attribute__((warn_unused_result));

/**
 * Free the page cache structure and null out the reference to it.
 *
 * @param cachePtr a pointer to the cache to free
 **/
void freeVDOPageCache(struct vdo_page_cache **cachePtr);

/**
 * Set the initial dirty period for a page cache.
 *
 * @param cache  The cache
 * @param period The initial dirty period to set
 **/
void setVDOPageCacheInitialPeriod(struct vdo_page_cache *cache,
                                  SequenceNumber         period);

/**
 * Switch the page cache into or out of read-only rebuild mode.
 *
 * @param cache       The cache
 * @param rebuilding  <code>true</code> if the cache should be put into
 *                    read-only rebuild mode, <code>false</code> otherwise
 **/
void setVDOPageCacheRebuildMode(struct vdo_page_cache *cache, bool rebuilding);

/**
 * Check whether a page cache is active (i.e. has any active lookups,
 * outstanding I/O, or pending I/O).
 *
 * @param cache  The cache to check
 *
 * @return <code>true</code> if the cache is active
 **/
bool isPageCacheActive(struct vdo_page_cache *cache)
  __attribute__((warn_unused_result));

/**
 * Advance the dirty period for a page cache.
 *
 * @param cache   The cache to advance
 * @param period  The new dirty period
 **/
void advanceVDOPageCachePeriod(struct vdo_page_cache *cache,
                               SequenceNumber         period);

/**
 * Write one or more batches of dirty pages.
 *
 * All writable pages in the ancient era and some number in the old era
 * are scheduled for writing.
 *
 * @param cache    the VDO page cache
 * @param batches  how many batches to write now
 * @param total    how many batches (including those being written now) remain
 *                   in this era
 **/
void writeVDOPageCachePages(struct vdo_page_cache *cache,
                            size_t                 batches,
                            size_t                 total);

/**
 * Rotate the dirty page eras.
 *
 * Move all pages in the old era to the ancient era and then move
 * the current era bin into the old era.
 *
 * @param cache   the VDO page cache
 **/
void rotateVDOPageCacheEras(struct vdo_page_cache *cache);

// ASYNC

/**
 * A completion awaiting a specific page.  Also a live reference into the
 * page once completed, until freed.
 **/
struct vdo_page_completion {
  /** The generic completion */
  struct vdo_completion  completion;
  /** The cache involved */
  struct vdo_page_cache *cache;
  /** The waiter for the pending list */
  struct waiter          waiter;
  /** The absolute physical block number of the page on disk */
  PhysicalBlockNumber    pbn;
  /** Whether the page may be modified */
  bool                   writable;
  /** Whether the page is available */
  bool                   ready;
  /** The info structure for the page, only valid when ready */
  struct page_info      *info;
};

/**
 * Initialize a VDO Page Completion, requesting a particular page from the
 * cache.
 *
 * @param pageCompletion  The vdo_page_completion to initialize
 * @param cache           The VDO page cache
 * @param pbn             The absolute physical block of the desired page
 * @param writable        Whether the page can be modified
 * @param parent          The parent object
 * @param callback        The completion callback
 * @param errorHandler    The handler for page errors
 *
 * @note Once a completion has occurred for the getVDOPageAsync operation,
 *       the underlying page shall be busy (stuck in memory) until the
 *       vdo_completion returned by this operation has been released.
 **/
void initVDOPageCompletion(struct vdo_page_completion *pageCompletion,
                           struct vdo_page_cache      *cache,
                           PhysicalBlockNumber         pbn,
                           bool                        writable,
                           void                       *parent,
                           VDOAction                  *callback,
                           VDOAction                  *errorHandler);

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
void releaseVDOPageCompletion(struct vdo_completion *completion);

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
 * @param completion    the completion initialized my initVDOPageCompletion().
 **/
void getVDOPageAsync(struct vdo_completion *completion);

/**
 * Mark a VDO page referenced by a completed vdo_page_completion as dirty.
 *
 * @param completion      a VDO Page Completion whose callback has been called
 * @param oldDirtyPeriod  the period in which the page was already dirty (0 if
 *                        it wasn't)
 * @param newDirtyPeriod  the period in which the page is now dirty
 **/
void markCompletedVDOPageDirty(struct vdo_completion *completion,
                               SequenceNumber         oldDirtyPeriod,
                               SequenceNumber         newDirtyPeriod);

/**
 * Request that a VDO page be written out as soon as it is not busy.
 *
 * @param completion  the vdo_page_completion containing the page
 **/
void requestVDOPageWrite(struct vdo_completion *completion);

/**
 * Access the raw memory for a read-only page of a completed
 * vdo_page_completion.
 *
 * @param completion    a vdo page completion whose callback has been called
 *
 * @return a pointer to the raw memory at the beginning of the page, or
 *         NULL if the page is not available.
 **/
const void *dereferenceReadableVDOPage(struct vdo_completion *completion);

/**
 * Access the raw memory for a writable page of a completed
 * vdo_page_completion.
 *
 * @param completion    a vdo page completion whose callback has been called
 *
 * @return a pointer to the raw memory at the beginning of the page, or
 *         NULL if the page is not available, or if the page is read-only
 **/
void *dereferenceWritableVDOPage(struct vdo_completion *completion);

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
void *getVDOPageCompletionContext(struct vdo_completion *completion);

/**
 * Drain I/O for a page cache.
 *
 * @param cache  The cache to drain
 **/
void drainVDOPageCache(struct vdo_page_cache *cache);

/**
 * Invalidate all entries in the VDO page cache. There must not be any
 * dirty pages in the cache.
 *
 * @param cache  the cache to invalidate
 *
 * @return a success or error code
 **/
int invalidateVDOPageCache(struct vdo_page_cache *cache)
  __attribute__((warn_unused_result));

// STATISTICS & TESTING

/**
 * Get current cache statistics.
 *
 * @param cache  the page cache
 *
 * @return the statistics
 **/
struct atomic_page_cache_statistics *
getVDOPageCacheStatistics(struct vdo_page_cache *cache)
  __attribute__((warn_unused_result));

#endif // VDO_PAGE_CACHE_H

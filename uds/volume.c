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
 * $Id: //eng/uds-releases/krusty/src/uds/volume.c#26 $
 */

#include "volume.h"

#include "cacheCounters.h"
#include "chapterIndex.h"
#include "compiler.h"
#include "errors.h"
#include "geometry.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "recordPage.h"
#include "request.h"
#include "sparseCache.h"
#include "stringUtils.h"
#include "threads.h"

enum {
  MAX_BAD_CHAPTERS = 100,           // max number of contiguous bad chapters
  DEFAULT_VOLUME_READ_THREADS = 2,  // Default number of reader threads
  MAX_VOLUME_READ_THREADS = 16,     // Maximum number of reader threads
};

/**********************************************************************/
static unsigned int getReadThreads(const struct uds_parameters *userParams)
{
  unsigned int readThreads = (userParams == NULL
                              ? DEFAULT_VOLUME_READ_THREADS
                              : userParams->read_threads);
  if (readThreads < 1) {
    readThreads = 1;
  }
  if (readThreads > MAX_VOLUME_READ_THREADS) {
    readThreads = MAX_VOLUME_READ_THREADS;
  }
  return readThreads;
}

/**********************************************************************/
static INLINE unsigned int mapToPageNumber(struct geometry *geometry,
                                           unsigned int     physicalPage)
{
  return ((physicalPage - 1) % geometry->pages_per_chapter);
}

/**********************************************************************/
static INLINE unsigned int mapToChapterNumber(struct geometry *geometry,
                                              unsigned int     physicalPage)
{
  return ((physicalPage - 1) / geometry->pages_per_chapter);
}

/**********************************************************************/
static INLINE bool isRecordPage(struct geometry *geometry,
                                unsigned int physicalPage)
{
  return (((physicalPage - 1) % geometry->pages_per_chapter)
          >= geometry->index_pages_per_chapter);
}

/**********************************************************************/
static INLINE unsigned int getZoneNumber(Request *request)
{
  return (request == NULL) ? 0 : request->zone_number;
}

/**********************************************************************/
int mapToPhysicalPage(const struct geometry *geometry, int chapter, int page)
{
  // Page zero is the header page, so the first index page in the
  // first chapter is physical page one.
  return (1 + (geometry->pages_per_chapter * chapter) + page);
}

/**********************************************************************/
static void waitForReadQueueNotFull(struct volume *volume, Request *request)
{
  unsigned int zoneNumber = getZoneNumber(request);
  invalidate_counter_t invalidateCounter
    = get_invalidate_counter(volume->pageCache, zoneNumber);

  if (search_pending(invalidateCounter)) {
    // Increment the invalidate counter to avoid deadlock where the reader
    // threads cannot make progress because they are waiting on the counter
    // and the index thread cannot because the read queue is full.
    end_pending_search(volume->pageCache, zoneNumber);
  }

  while (read_queue_is_full(volume->pageCache)) {
    logDebug("Waiting until read queue not full");
    signalCond(&volume->readThreadsCond);
    waitCond(&volume->readThreadsReadDoneCond, &volume->readThreadsMutex);
  }

  if (search_pending(invalidateCounter)) {
    // Increment again so we get back to an odd value.
    begin_pending_search(volume->pageCache,
                         page_being_searched(invalidateCounter), zoneNumber);
  }
}

/**********************************************************************/
int enqueuePageRead(struct volume *volume, Request *request, int physicalPage)
{
  // Don't allow new requests if we are shutting down, but make sure
  // to process any requests that are still in the pipeline.
  if ((volume->readerState & READER_STATE_EXIT) != 0) {
    logInfo("failed to queue read while shutting down");
    return UDS_SHUTTINGDOWN;
  }

  // Mark the page as queued in the volume cache, for chapter invalidation to
  // be able to cancel a read.
  // If we are unable to do this because the queues are full, flush them first
  int result;
  while ((result = enqueue_read(volume->pageCache, request, physicalPage))
         == UDS_SUCCESS) {
    logDebug("Read queues full, waiting for reads to finish");
    waitForReadQueueNotFull(volume, request);
  }

  if (result == UDS_QUEUED) {
    /* signal a read thread */
    signalCond(&volume->readThreadsCond);
  }

  return result;
}

/**********************************************************************/
static INLINE void waitToReserveReadQueueEntry(struct volume *volume,
                                               unsigned int  *queuePos,
                                               Request      **requestList,
                                               unsigned int  *physicalPage,
                                               bool          *invalid)
{
  while (((volume->readerState & READER_STATE_EXIT) == 0)
         && (((volume->readerState & READER_STATE_STOP) != 0)
             || !reserve_read_queue_entry(volume->pageCache,
                                          queuePos,
                                          requestList,
                                          physicalPage,
                                          invalid))) {
    waitCond(&volume->readThreadsCond, &volume->readThreadsMutex);
  }
}

/**********************************************************************/
static int initChapterIndexPage(const struct volume     *volume,
                                byte                    *indexPage,
                                unsigned int             chapter,
                                unsigned int             indexPageNumber,
                                struct delta_index_page *chapterIndexPage)
{
  struct geometry *geometry = volume->geometry;

  int result = initialize_chapter_index_page(chapterIndexPage, geometry,
                                             indexPage, volume->nonce);
  if (volume->lookupMode == LOOKUP_FOR_REBUILD) {
    return result;
  }
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "Reading chapter index page for chapter %u"
                                   " page %u",
                                   chapter, indexPageNumber);
  }

  struct index_page_bounds bounds;
  result = get_list_number_bounds(volume->indexPageMap, chapter,
                                  indexPageNumber, &bounds);
  if (result != UDS_SUCCESS) {
    return result;
  }

  uint64_t     ciVirtual = chapterIndexPage->virtual_chapter_number;
  unsigned int ciChapter = map_to_physical_chapter(geometry, ciVirtual);
  if ((chapter == ciChapter) &&
      (bounds.lowest_list == chapterIndexPage->lowest_list_number) &&
      (bounds.highest_list == chapterIndexPage->highest_list_number)) {
    return UDS_SUCCESS;
  }

  logWarning("Index page map updated to %llu",
             get_last_update(volume->indexPageMap));
  logWarning("Page map expects that chapter %u page %u has range %u to %u, "
             "but chapter index page has chapter %" PRIu64
             " with range %u to %u",
             chapter, indexPageNumber, bounds.lowest_list, bounds.highest_list,
             ciVirtual, chapterIndexPage->lowest_list_number,
             chapterIndexPage->highest_list_number);
  return ASSERT_WITH_ERROR_CODE(false,
                                UDS_CORRUPT_DATA,
                                "index page map mismatch with chapter index");
}

/**********************************************************************/
static int initializeIndexPage(const struct volume *volume,
                               unsigned int         physicalPage,
                               struct cached_page  *page)
{
  unsigned int chapter = mapToChapterNumber(volume->geometry, physicalPage);
  unsigned int indexPageNumber = mapToPageNumber(volume->geometry,
                                                 physicalPage);
  int result = initChapterIndexPage(volume, get_page_data(&page->cp_page_data),
                                    chapter, indexPageNumber,
                                    &page->cp_index_page);
  return result;
}

/**********************************************************************/
static void readThreadFunction(void *arg)
{
  struct volume *volume  = arg;
  unsigned int   queuePos;
  Request       *requestList;
  unsigned int   physicalPage;
  bool           invalid = false;

  logDebug("reader starting");
  lockMutex(&volume->readThreadsMutex);
  while (true) {
    waitToReserveReadQueueEntry(volume, &queuePos, &requestList, &physicalPage,
                                &invalid);
    if ((volume->readerState & READER_STATE_EXIT) != 0) {
      break;
    }

    volume->busyReaderThreads++;

    bool recordPage = isRecordPage(volume->geometry, physicalPage);

    struct cached_page *page = NULL;
    int result = UDS_SUCCESS;
    if (!invalid) {
      // Find a place to put the read queue page we reserved above.
      result = select_victim_in_cache(volume->pageCache, &page);
      if (result == UDS_SUCCESS) {
        unlockMutex(&volume->readThreadsMutex);
        result = read_volume_page(&volume->volumeStore, physicalPage,
                                  &page->cp_page_data);
        if (result != UDS_SUCCESS) {
          logWarning("Error reading page %u from volume", physicalPage);
          cancel_page_in_cache(volume->pageCache, physicalPage, page);
        }
        lockMutex(&volume->readThreadsMutex);
      } else {
        logWarning("Error selecting cache victim for page read");
      }

      if (result == UDS_SUCCESS) {
        if (!volume->pageCache->read_queue[queuePos].invalid) {
          if (!recordPage) {
            result = initializeIndexPage(volume, physicalPage, page);
            if (result != UDS_SUCCESS) {
              logWarning("Error initializing chapter index page");
              cancel_page_in_cache(volume->pageCache, physicalPage, page);
            }
          }

          if (result == UDS_SUCCESS) {
            result = put_page_in_cache(volume->pageCache, physicalPage, page);
            if (result != UDS_SUCCESS) {
              logWarning("Error putting page %u in cache", physicalPage);
              cancel_page_in_cache(volume->pageCache, physicalPage, page);
            }
          }
        } else {
          logWarning("Page %u invalidated after read", physicalPage);
          cancel_page_in_cache(volume->pageCache, physicalPage, page);
          invalid = true;
        }
      }
    } else {
      logDebug("Requeuing requests for invalid page");
    }

    if (invalid) {
      result = UDS_SUCCESS;
      page = NULL;
    }

    while (requestList != NULL) {
      Request *request = requestList;
      requestList = request->next_request;

      /*
       * If we've read in a record page, we're going to do an immediate search,
       * in an attempt to speed up processing when we requeue the request, so
       * that it doesn't have to go back into the getRecordFromZone code again.
       * However, if we've just read in an index page, we don't want to search.
       * We want the request to be processed again and getRecordFromZone to be
       * run.  We have added new fields in request to allow the index code to
       * know whether it can stop processing before getRecordFromZone is called
       * again.
       */
      if ((result == UDS_SUCCESS) && (page != NULL) && recordPage) {
        if (search_record_page(get_page_data(&page->cp_page_data),
                               &request->chunk_name, volume->geometry,
                               &request->old_metadata)) {
          request->sl_location = LOC_IN_DENSE;
        } else {
          request->sl_location = LOC_UNAVAILABLE;
        }
        request->sl_location_known = true;
      }

      // reflect any read failures in the request status
      request->status = result;
      restart_request(request);
    }

    release_read_queue_entry(volume->pageCache, queuePos);

    volume->busyReaderThreads--;
    broadcastCond(&volume->readThreadsReadDoneCond);
  }
  unlockMutex(&volume->readThreadsMutex);
  logDebug("reader done");
}

/**********************************************************************/
static int readPageLocked(struct volume       *volume,
                          Request             *request,
                          unsigned int         physicalPage,
                          bool                 syncRead,
                          struct cached_page **pagePtr)
{
  syncRead |= ((volume->lookupMode == LOOKUP_FOR_REBUILD)
               || (request == NULL)
               || (request->session == NULL));

  int result = UDS_SUCCESS;

  struct cached_page *page = NULL;
  if (syncRead) {
    // Find a place to put the page.
    result = select_victim_in_cache(volume->pageCache, &page);
    if (result != UDS_SUCCESS) {
      logWarning("Error selecting cache victim for page read");
      return result;
    }
    result = read_volume_page(&volume->volumeStore, physicalPage,
                              &page->cp_page_data);
    if (result != UDS_SUCCESS) {
      logWarning("Error reading page %u from volume", physicalPage);
      cancel_page_in_cache(volume->pageCache, physicalPage, page);
      return result;
    }
    if (!isRecordPage(volume->geometry, physicalPage)) {
      result = initializeIndexPage(volume, physicalPage, page);
      if (result != UDS_SUCCESS) {
        if (volume->lookupMode != LOOKUP_FOR_REBUILD) {
          logWarning("Corrupt index page %u", physicalPage);
        }
        cancel_page_in_cache(volume->pageCache, physicalPage, page);
        return result;
      }
    }
    result = put_page_in_cache(volume->pageCache, physicalPage, page);
    if (result != UDS_SUCCESS) {
      logWarning("Error putting page %u in cache", physicalPage);
      cancel_page_in_cache(volume->pageCache, physicalPage, page);
      return result;
    }
  } else {
    result = enqueuePageRead(volume, request, physicalPage);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  *pagePtr = page;

  return UDS_SUCCESS;
}

/**********************************************************************/
int getPageLocked(struct volume          *volume,
                  Request                *request,
                  unsigned int            physicalPage,
                  cache_probe_type_t      probeType,
                  struct cached_page    **pagePtr)
{
  struct cached_page *page = NULL;
  int result = get_page_from_cache(volume->pageCache, physicalPage, probeType,
                                   &page);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (page == NULL) {
    result = readPageLocked(volume, request, physicalPage, true, &page);
    if (result != UDS_SUCCESS) {
      return result;
    }
  } else if (getZoneNumber(request) == 0) {
    // Only 1 zone is responsible for updating LRU
    make_page_most_recent(volume->pageCache, page);
  }

  *pagePtr = page;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getPageProtected(struct volume       *volume,
                     Request             *request,
                     unsigned int         physicalPage,
                     cache_probe_type_t   probeType,
                     struct cached_page **pagePtr)
{
  struct cached_page *page = NULL;
  int result = get_page_from_cache(volume->pageCache, physicalPage,
                                   probeType | CACHE_PROBE_IGNORE_FAILURE,
                                   &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int zoneNumber = getZoneNumber(request);
  // If we didn't find a page we need to enqueue a read for it, in which
  // case we need to grab the mutex.
  if (page == NULL) {
    end_pending_search(volume->pageCache, zoneNumber);
    lockMutex(&volume->readThreadsMutex);

    /*
     * Do the lookup again while holding the read mutex (no longer the fast
     * case so this should be ok to repeat). We need to do this because an
     * page may have been added to the page map by the reader thread between
     * the time searched above and the time we went to actually try to enqueue
     * it below. This could result in us enqueuing another read for an page
     * which is already in the cache, which would mean we end up with two
     * entries in the cache for the same page.
     */
    result
      = get_page_from_cache(volume->pageCache, physicalPage, probeType, &page);
    if (result != UDS_SUCCESS) {
      /*
       * In non-success cases (anything not UDS_SUCCESS, meaning both
       * UDS_QUEUED and "real" errors), the caller doesn't get a
       * handle on a cache page, so it can't continue the search, and
       * we don't need to prevent other threads from messing with the
       * cache.
       *
       * However, we do need to set the "search pending" flag because
       * the callers expect it to always be set on return, even if
       * they can't actually do the search.
       *
       * Doing the calls in this order ought to be faster, since we
       * let other threads have the reader thread mutex (which can
       * require a syscall) ASAP, and set the "search pending" state
       * that can block the reader thread as the last thing.
       */
      unlockMutex(&volume->readThreadsMutex);
      begin_pending_search(volume->pageCache, physicalPage, zoneNumber);
      return result;
    }

    // If we found the page now, we can release the mutex and proceed
    // as if this were the fast case.
    if (page != NULL) {
      /*
       * If we found a page (*pagePtr != NULL and return
       * UDS_SUCCESS), then we're telling the caller where to look for
       * the cache page, and need to switch to "reader thread
       * unlocked" and "search pending" state in careful order so no
       * other thread can mess with the data before our caller gets to
       * look at it.
       */
      begin_pending_search(volume->pageCache, physicalPage, zoneNumber);
      unlockMutex(&volume->readThreadsMutex);
    }
  }

  if (page == NULL) {
    result = readPageLocked(volume, request, physicalPage, false, &page);
    if (result != UDS_SUCCESS) {
      /*
       * This code path is used frequently in the UDS_QUEUED case, so
       * the performance gain from unlocking first, while "search
       * pending" mode is off, turns out to be significant in some
       * cases.
       */
      unlockMutex(&volume->readThreadsMutex);
      begin_pending_search(volume->pageCache, physicalPage, zoneNumber);
      return result;
    }

    // See above re: ordering requirement.
    begin_pending_search(volume->pageCache, physicalPage, zoneNumber);
    unlockMutex(&volume->readThreadsMutex);
  } else {
    if (getZoneNumber(request) == 0 ) {
      // Only 1 zone is responsible for updating LRU
      make_page_most_recent(volume->pageCache, page);
    }
  }

  *pagePtr = page;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getPage(struct volume            *volume,
            unsigned int              chapter,
            unsigned int              pageNumber,
            cache_probe_type_t        probeType,
            byte                    **dataPtr,
            struct delta_index_page **indexPagePtr)
{
  unsigned int physicalPage
    = mapToPhysicalPage(volume->geometry, chapter, pageNumber);

  lockMutex(&volume->readThreadsMutex);
  struct cached_page *page = NULL;
  int result = getPageLocked(volume, NULL, physicalPage, probeType, &page);
  unlockMutex(&volume->readThreadsMutex);

  if (dataPtr != NULL) {
    *dataPtr = (page != NULL) ? get_page_data(&page->cp_page_data) : NULL;
  }
  if (indexPagePtr != NULL) {
    *indexPagePtr = (page != NULL) ? &page->cp_index_page : NULL;
  }
  return result;
}

/**
 * Search for a chunk name in a cached index page or chapter index, returning
 * the record page number from a chapter index match.
 *
 * @param volume           the volume containing the index page to search
 * @param request          the request originating the search (may be NULL for
 *                         a direct query from volume replay)
 * @param name             the name of the block or chunk
 * @param chapter          the chapter to search
 * @param indexPageNumber  the index page number of the page to search
 * @param recordPageNumber pointer to return the chapter record page number
 *                         (value will be NO_CHAPTER_INDEX_ENTRY if the name
 *                         was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
static int searchCachedIndexPage(struct volume               *volume,
                                 Request                     *request,
                                 const struct uds_chunk_name *name,
                                 unsigned int                 chapter,
                                 unsigned int                 indexPageNumber,
                                 int                         *recordPageNumber)
{
  unsigned int zoneNumber = getZoneNumber(request);
  unsigned int physicalPage
    = mapToPhysicalPage(volume->geometry, chapter, indexPageNumber);

  /*
   * Make sure the invalidate counter is updated before we try and read from
   * the page map.  This prevents this thread from reading a page in the
   * page map which has already been marked for invalidation by the reader
   * thread, before the reader thread has noticed that the invalidateCounter
   * has been incremented.
   */
  begin_pending_search(volume->pageCache, physicalPage, zoneNumber);

  struct cached_page *page = NULL;
  int result = getPageProtected(volume, request, physicalPage,
                                cache_probe_type(request, true), &page);
  if (result != UDS_SUCCESS) {
    end_pending_search(volume->pageCache, zoneNumber);
    return result;
  }

  result
    = ASSERT_LOG_ONLY(search_pending(get_invalidate_counter(volume->pageCache,
                                                            zoneNumber)),
                      "Search is pending for zone %u", zoneNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = search_chapter_index_page(&page->cp_index_page,
                                     volume->geometry, name,
                                     recordPageNumber);
  end_pending_search(volume->pageCache, zoneNumber);
  return result;
}

/**********************************************************************/
int searchCachedRecordPage(struct volume               *volume,
                           Request                     *request,
                           const struct uds_chunk_name *name,
                           unsigned int                 chapter,
                           int                          recordPageNumber,
                           struct uds_chunk_data       *duplicate,
                           bool                        *found)
{
  *found = false;

  if (recordPageNumber == NO_CHAPTER_INDEX_ENTRY) {
    // No record for that name can exist in the chapter.
    return UDS_SUCCESS;
  }

  struct geometry *geometry = volume->geometry;
  int result = ASSERT(((recordPageNumber >= 0)
                       && ((unsigned int) recordPageNumber
                           < geometry->record_pages_per_chapter)),
                      "0 <= %d <= %u",
                      recordPageNumber, geometry->record_pages_per_chapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int pageNumber
    = geometry->index_pages_per_chapter + recordPageNumber;

  unsigned int zoneNumber = getZoneNumber(request);
  int physicalPage
    = mapToPhysicalPage(volume->geometry, chapter, pageNumber);

  /*
   * Make sure the invalidate counter is updated before we try and read from
   * the page map. This prevents this thread from reading a page in the page
   * map which has already been marked for invalidation by the reader thread,
   * before the reader thread has noticed that the invalidateCounter has been
   * incremented.
   */
  begin_pending_search(volume->pageCache, physicalPage, zoneNumber);

  struct cached_page *recordPage;
  result = getPageProtected(volume, request, physicalPage,
                            cache_probe_type(request, false), &recordPage);
  if (result != UDS_SUCCESS) {
    end_pending_search(volume->pageCache, zoneNumber);
    return result;
  }

  if (search_record_page(get_page_data(&recordPage->cp_page_data), name, geometry,
                       duplicate)) {
    *found = true;
  }
  end_pending_search(volume->pageCache, zoneNumber);
  return UDS_SUCCESS;
}

/**********************************************************************/
int readChapterIndexFromVolume(const struct volume     *volume,
                               uint64_t                 virtualChapter,
                               struct volume_page       volume_pages[],
                               struct delta_index_page  indexPages[])
{
  const struct geometry *geometry = volume->geometry;
  unsigned int physicalChapter = map_to_physical_chapter(geometry,
                                                         virtualChapter);
  int physicalPage = mapToPhysicalPage(geometry, physicalChapter, 0);
  prefetch_volume_pages(&volume->volumeStore, physicalPage,
                        geometry->index_pages_per_chapter);

  unsigned int i;
  struct volume_page volume_page;
  int result = initialize_volume_page(geometry, &volume_page);
  for (i = 0; i < geometry->index_pages_per_chapter; i++) {
    int result = read_volume_page(&volume->volumeStore, physicalPage + i,
                                  &volume_pages[i]);
    if (result != UDS_SUCCESS) {
      break;
    }
    byte *indexPage = get_page_data(&volume_pages[i]);
    result = initChapterIndexPage(volume, indexPage, physicalChapter, i,
                                  &indexPages[i]);
    if (result != UDS_SUCCESS) {
      break;
    }
  }
  destroy_volume_page(&volume_page);
  return result;
}

/**********************************************************************/
int searchVolumePageCache(struct volume               *volume,
                          Request                     *request,
                          const struct uds_chunk_name *name,
                          uint64_t                     virtualChapter,
                          struct uds_chunk_data       *metadata,
                          bool                        *found)
{
  unsigned int physicalChapter
    = map_to_physical_chapter(volume->geometry, virtualChapter);
  unsigned int indexPageNumber;
  int result = find_index_page_number(volume->indexPageMap, name,
  				      physicalChapter, &indexPageNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }

  int recordPageNumber;
  result = searchCachedIndexPage(volume, request, name, physicalChapter,
                                 indexPageNumber, &recordPageNumber);
  if (result == UDS_SUCCESS) {
    result = searchCachedRecordPage(volume, request, name, physicalChapter,
                                    recordPageNumber, metadata, found);
  }

  return result;
}

/**********************************************************************/
int forgetChapter(struct volume            *volume,
                  uint64_t                  virtualChapter,
                  enum invalidation_reason  reason)
{
  logDebug("forgetting chapter %llu", virtualChapter);
  unsigned int physicalChapter
    = map_to_physical_chapter(volume->geometry, virtualChapter);
  lockMutex(&volume->readThreadsMutex);
  int result
    = invalidate_page_cache_for_chapter(volume->pageCache, physicalChapter,
                                        volume->geometry->pages_per_chapter,
                                        reason);
  unlockMutex(&volume->readThreadsMutex);
  return result;
}

/**
 * Donate index page data to the page cache for an index page that was just
 * written to the volume.  The caller must already hold the reader thread
 * mutex.
 *
 * @param volume           the volume
 * @param physicalChapter  the physical chapter number of the index page
 * @param indexPageNumber  the chapter page number of the index page
 * @param scratchPage      the index page data
 **/
static int donateIndexPageLocked(struct volume      *volume,
                                 unsigned int        physicalChapter,
                                 unsigned int        indexPageNumber,
                                 struct volume_page *scratchPage)
{
  unsigned int physicalPage
    = mapToPhysicalPage(volume->geometry, physicalChapter, indexPageNumber);

  // Find a place to put the page.
  struct cached_page *page = NULL;
  int result = select_victim_in_cache(volume->pageCache, &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Exchange the scratch page with the cache page
  swap_volume_pages(&page->cp_page_data, scratchPage);

  result = initChapterIndexPage(volume, get_page_data(&page->cp_page_data),
                                physicalChapter, indexPageNumber,
                                &page->cp_index_page);
  if (result != UDS_SUCCESS) {
    logWarning("Error initialize chapter index page");
    cancel_page_in_cache(volume->pageCache, physicalPage, page);
    return result;
  }

  result = put_page_in_cache(volume->pageCache, physicalPage, page);
  if (result != UDS_SUCCESS) {
    logWarning("Error putting page %u in cache", physicalPage);
    cancel_page_in_cache(volume->pageCache, physicalPage, page);
    return result;
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int writeIndexPages(struct volume              *volume,
                    int                         physicalPage,
                    struct open_chapter_index  *chapterIndex,
                    byte                      **pages)
{
  struct geometry *geometry = volume->geometry;
  unsigned int physicalChapterNumber
    = map_to_physical_chapter(geometry, chapterIndex->virtual_chapter_number);
  unsigned int deltaListNumber = 0;

  unsigned int indexPageNumber;
  for (indexPageNumber = 0;
       indexPageNumber < geometry->index_pages_per_chapter;
       indexPageNumber++) {
    int result = prepare_to_write_volume_page(&volume->volumeStore,
                                              physicalPage + indexPageNumber,
                                              &volume->scratchPage);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result, "failed to prepare index page");
    }

    // Pack as many delta lists into the index page as will fit.
    unsigned int listsPacked;
    bool lastPage
      = ((indexPageNumber + 1) == geometry->index_pages_per_chapter);
    result = pack_open_chapter_index_page(chapterIndex,
                                          get_page_data(&volume->scratchPage),
                                          deltaListNumber,
                                          lastPage,
                                          &listsPacked);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result, "failed to pack index page");
    }

    result = write_volume_page(&volume->volumeStore,
                               physicalPage + indexPageNumber,
                               &volume->scratchPage);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to write chapter index page");
    }

    if (pages != NULL) {
      memcpy(pages[indexPageNumber], get_page_data(&volume->scratchPage),
             geometry->bytes_per_page);
    }

    // Tell the index page map the list number of the last delta list that was
    // packed into the index page.
    if (listsPacked == 0) {
      logDebug("no delta lists packed on chapter %u page %u",
               physicalChapterNumber, indexPageNumber);
    } else {
      deltaListNumber += listsPacked;
    }
    result = update_index_page_map(volume->indexPageMap,
    				   chapterIndex->virtual_chapter_number,
    				   physicalChapterNumber,
    				   indexPageNumber,
    				   deltaListNumber - 1);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "failed to update index page map");
    }

    // Donate the page data for the index page to the page cache.
    lockMutex(&volume->readThreadsMutex);
    result = donateIndexPageLocked(volume, physicalChapterNumber,
                                   indexPageNumber, &volume->scratchPage);
    unlockMutex(&volume->readThreadsMutex);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int writeRecordPages(struct volume                  *volume,
                     int                             physicalPage,
                     const struct uds_chunk_record   records[],
                     byte                          **pages)
{
  struct geometry *geometry = volume->geometry;
  // Skip over the index pages, which come before the record pages
  physicalPage += geometry->index_pages_per_chapter;
  // The record array from the open chapter is 1-based.
  const struct uds_chunk_record *nextRecord = &records[1];

  unsigned int recordPageNumber;
  for (recordPageNumber = 0;
       recordPageNumber < geometry->record_pages_per_chapter;
       recordPageNumber++) {
    int result = prepare_to_write_volume_page(&volume->volumeStore,
                                              physicalPage + recordPageNumber,
                                              &volume->scratchPage);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to prepare record page");
    }

    // Sort the next page of records and copy them to the record page as a
    // binary tree stored in heap order.
    result = encode_record_page(volume, nextRecord,
                              get_page_data(&volume->scratchPage));
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to encode record page %u",
                                       recordPageNumber);
    }
    nextRecord += geometry->records_per_page;

    result = write_volume_page(&volume->volumeStore,
                               physicalPage + recordPageNumber,
                               &volume->scratchPage);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to write chapter record page");
    }

    if (pages != NULL) {
      memcpy(pages[recordPageNumber], get_page_data(&volume->scratchPage),
             geometry->bytes_per_page);
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int writeChapter(struct volume                 *volume,
                 struct open_chapter_index     *chapterIndex,
                 const struct uds_chunk_record  records[])
{
  // Determine the position of the virtual chapter in the volume file.
  struct geometry *geometry = volume->geometry;
  unsigned int physicalChapterNumber
    = map_to_physical_chapter(geometry, chapterIndex->virtual_chapter_number);
  int physicalPage = mapToPhysicalPage(geometry, physicalChapterNumber, 0);

  // Pack and write the delta chapter index pages to the volume.
  int result = writeIndexPages(volume, physicalPage, chapterIndex, NULL);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Sort and write the record pages to the volume.
  result = writeRecordPages(volume, physicalPage, records, NULL);
  if (result != UDS_SUCCESS) {
    return result;
  }
  release_volume_page(&volume->scratchPage);
  // Flush the data to permanent storage.
  return sync_volume_store(&volume->volumeStore);
}

/**********************************************************************/
size_t getCacheSize(struct volume *volume)
{
  size_t size = get_page_cache_size(volume->pageCache);
  if (is_sparse(volume->geometry)) {
    size += get_sparse_cache_memory_size(volume->sparseCache);
  }
  return size;
}

/**********************************************************************/
static int probeChapter(struct volume *volume,
                        unsigned int   chapterNumber,
                        uint64_t      *virtualChapterNumber)
{
  const struct geometry *geometry = volume->geometry;
  unsigned int expectedListNumber = 0;
  uint64_t lastVCN = UINT64_MAX;

  prefetch_volume_pages(&volume->volumeStore,
                        mapToPhysicalPage(geometry, chapterNumber, 0),
                        geometry->index_pages_per_chapter);

  unsigned int i;
  for (i = 0; i < geometry->index_pages_per_chapter; ++i) {
    struct delta_index_page *page;
    int result = getPage(volume, chapterNumber, i, CACHE_PROBE_INDEX_FIRST,
                         NULL, &page);
    if (result != UDS_SUCCESS) {
      return result;
    }

    uint64_t vcn = page->virtual_chapter_number;
    if (lastVCN == UINT64_MAX) {
      lastVCN = vcn;
    } else if (vcn != lastVCN) {
      logError("inconsistent chapter %u index page %u: expected vcn %"
               PRIu64 ", got vcn %llu",
               chapterNumber, i, lastVCN, vcn);
      return UDS_CORRUPT_COMPONENT;
    }

    if (expectedListNumber != page->lowest_list_number) {
      logError("inconsistent chapter %u index page %u: expected list number %u"
               ", got list number %u",
               chapterNumber, i, expectedListNumber, page->lowest_list_number);
      return UDS_CORRUPT_COMPONENT;
    }
    expectedListNumber = page->highest_list_number + 1;

    result = validate_chapter_index_page(page, geometry);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  if (lastVCN == UINT64_MAX) {
    logError("no chapter %u virtual chapter number determined", chapterNumber);
    return UDS_CORRUPT_COMPONENT;
  }
  if (chapterNumber != lastVCN % geometry->chapters_per_volume) {
    logError("chapter %u vcn %llu is out of phase (%u)",
             chapterNumber, lastVCN, geometry->chapters_per_volume);
    return UDS_CORRUPT_COMPONENT;
  }
  *virtualChapterNumber = lastVCN;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int probeWrapper(void         *aux,
                        unsigned int  chapterNumber,
                        uint64_t     *virtualChapterNumber)
{
  struct volume *volume = aux;
  int result = probeChapter(volume, chapterNumber, virtualChapterNumber);
  if ((result == UDS_CORRUPT_COMPONENT) || (result == UDS_CORRUPT_DATA)) {
    *virtualChapterNumber = UINT64_MAX;
    return UDS_SUCCESS;
  }
  return result;
}

/**********************************************************************/
static int findRealEndOfVolume(struct volume *volume,
                               unsigned int   limit,
                               unsigned int  *limitPtr)
{
  /*
   * Start checking from the end of the volume. As long as we hit corrupt
   * data, start skipping larger and larger amounts until we find real data.
   * If we find real data, reduce the span and try again until we find
   * the exact boundary.
   */
  unsigned int span = 1;
  unsigned int tries = 0;
  while (limit > 0) {
    unsigned int chapter = (span > limit) ? 0 : limit - span;
    uint64_t vcn = 0;
    int result = probeChapter(volume, chapter, &vcn);
    if (result == UDS_SUCCESS) {
      if (span == 1) {
        break;
      }
      span /= 2;
      tries = 0;
    } else if (result == UDS_CORRUPT_COMPONENT) {
      limit = chapter;
      if (++tries > 1) {
        span *= 2;
      }
    } else {
      return logErrorWithStringError(result, "cannot determine end of volume");
    }
  }

  if (limitPtr != NULL) {
    *limitPtr = limit;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int findVolumeChapterBoundaries(struct volume *volume,
                                uint64_t      *lowestVCN,
                                uint64_t      *highestVCN,
                                bool          *isEmpty)
{
  unsigned int chapterLimit = volume->geometry->chapters_per_volume;

  int result = findRealEndOfVolume(volume, chapterLimit, &chapterLimit);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot find end of volume");
  }

  if (chapterLimit == 0) {
    *lowestVCN = 0;
    *highestVCN = 0;
    *isEmpty = true;
    return UDS_SUCCESS;
  }

  *isEmpty = false;
  return findVolumeChapterBoundariesImpl(chapterLimit, MAX_BAD_CHAPTERS,
                                         lowestVCN, highestVCN, probeWrapper,
                                         volume);
}

/**********************************************************************/
int findVolumeChapterBoundariesImpl(unsigned int  chapterLimit,
                                    unsigned int  maxBadChapters,
                                    uint64_t     *lowestVCN,
                                    uint64_t     *highestVCN,
                                    int (*probeFunc)(void         *aux,
                                                     unsigned int  chapter,
                                                     uint64_t     *vcn),
                                    void *aux)
{
  if (chapterLimit == 0) {
    *lowestVCN = 0;
    *highestVCN = 0;
    return UDS_SUCCESS;
  }

  /*
   * This method assumes there is at most one run of contiguous bad chapters
   * caused by unflushed writes. Either the bad spot is at the beginning and
   * end, or somewhere in the middle. Wherever it is, the highest and lowest
   * VCNs are adjacent to it. Otherwise the volume is cleanly saved and
   * somewhere in the middle of it the highest VCN immediately preceeds the
   * lowest one.
   */

  uint64_t firstVCN = UINT64_MAX;

  // doesn't matter if this results in a bad spot (UINT64_MAX)
  int result = (*probeFunc)(aux, 0, &firstVCN);
  if (result != UDS_SUCCESS) {
    return UDS_SUCCESS;
  }

  /*
   * Binary search for end of the discontinuity in the monotonically
   * increasing virtual chapter numbers; bad spots are treated as a span of
   * UINT64_MAX values. In effect we're searching for the index of the
   * smallest value less than firstVCN. In the case we go off the end it means
   * that chapter 0 has the lowest vcn.
   */

  unsigned int leftChapter = 0;
  unsigned int rightChapter = chapterLimit;

  while (leftChapter < rightChapter) {
    unsigned int chapter = (leftChapter + rightChapter) / 2;
    uint64_t probeVCN;

    result = (*probeFunc)(aux, chapter, &probeVCN);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (firstVCN <= probeVCN) {
      leftChapter = chapter + 1;
    } else {
      rightChapter = chapter;
    }
  }

  uint64_t lowest = UINT64_MAX;
  uint64_t highest = UINT64_MAX;

  result = ASSERT(leftChapter == rightChapter, "leftChapter == rightChapter");
  if (result != UDS_SUCCESS) {
    return result;
  }

  leftChapter %= chapterLimit;  // in case we're at the end

  // At this point, leftChapter is the chapter with the lowest virtual chapter
  // number.

  result = (*probeFunc)(aux, leftChapter, &lowest);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((lowest != UINT64_MAX), "invalid lowest chapter");
  if (result != UDS_SUCCESS) {
    return result;
  }

  // We now circularly scan backwards, moving over any bad chapters until we
  // find the chapter with the highest vcn (the first good chapter we
  // encounter).

  unsigned int badChapters = 0;

  for (;;) {
    rightChapter = (rightChapter + chapterLimit - 1) % chapterLimit;
    result = (*probeFunc)(aux, rightChapter, &highest);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (highest != UINT64_MAX) {
      break;
    }
    if (++badChapters >= maxBadChapters) {
      logError("too many bad chapters in volume: %u", badChapters);
      return UDS_CORRUPT_COMPONENT;
    }
  }

  *lowestVCN = lowest;
  *highestVCN = highest;
  return UDS_SUCCESS;
}

/**
 * Allocate a volume.
 *
 * @param config            The configuration to use
 * @param layout            The index layout
 * @param readQueueMaxSize  The maximum size of the read queue
 * @param zoneCount         The number of zones to use
 * @param newVolume         A pointer to hold the new volume
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check allocateVolume(const struct configuration *config,
                                       struct index_layout *layout,
                                       unsigned int readQueueMaxSize,
                                       unsigned int zoneCount,
                                       struct volume **newVolume)
{
  struct volume *volume;
  int result = ALLOCATE(1, struct volume, "volume", &volume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  volume->nonce = get_volume_nonce(layout);
  // It is safe to call freeVolume now to clean up and close the volume

  result = copy_geometry(config->geometry, &volume->geometry);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return logWarningWithStringError(result,
                                     "failed to allocate geometry: error");
  }

  // Need a buffer for each entry in the page cache
  unsigned int reservedBuffers
    = config->cache_chapters * config->geometry->record_pages_per_chapter;
  // And a buffer for the chapter writer
  reservedBuffers += 1;
  // And a buffer for each entry in the sparse cache
  if (is_sparse(volume->geometry)) {
    reservedBuffers
      += config->cache_chapters * config->geometry->index_pages_per_chapter;
  }
  result = open_volume_store(&volume->volumeStore, layout, reservedBuffers,
                             config->geometry->bytes_per_page);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = initialize_volume_page(config->geometry, &volume->scratchPage);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  result = make_radix_sorter(config->geometry->records_per_page,
                             &volume->radixSorter);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  result = ALLOCATE(config->geometry->records_per_page,
                    const struct uds_chunk_record *,
                    "record pointers", &volume->recordPointers);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  if (is_sparse(volume->geometry)) {
    result = make_sparse_cache(volume->geometry, config->cache_chapters,
                             zoneCount, &volume->sparseCache);
    if (result != UDS_SUCCESS) {
      freeVolume(volume);
      return result;
    }
  }
  result = make_page_cache(volume->geometry, config->cache_chapters,
                           readQueueMaxSize, zoneCount, &volume->pageCache);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = make_index_page_map(volume->geometry, &volume->indexPageMap);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  *newVolume = volume;
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeVolume(const struct configuration   *config,
               struct index_layout          *layout,
               const struct uds_parameters  *userParams,
               unsigned int                  readQueueMaxSize,
               unsigned int                  zoneCount,
               struct volume               **newVolume)
{
  unsigned int volumeReadThreads = getReadThreads(userParams);

  if (readQueueMaxSize <= volumeReadThreads) {
    logError("Number of read threads must be smaller than read queue");
    return UDS_INVALID_ARGUMENT;
  }

  struct volume *volume = NULL;
  int result = allocateVolume(config, layout, readQueueMaxSize, zoneCount,
                              &volume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initMutex(&volume->readThreadsMutex);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = initCond(&volume->readThreadsReadDoneCond);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = initCond(&volume->readThreadsCond);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  // Start the reader threads.  If this allocation succeeds, freeVolume knows
  // that it needs to try and stop those threads.
  result = ALLOCATE(volumeReadThreads, Thread, "reader threads",
                    &volume->readerThreads);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  unsigned int i;
  for (i = 0; i < volumeReadThreads; i++) {
    result = createThread(readThreadFunction, (void *) volume, "reader",
                          &volume->readerThreads[i]);
    if (result != UDS_SUCCESS) {
      freeVolume(volume);
      return result;
    }
    // We only stop as many threads as actually got started.
    volume->numReadThreads = i + 1;
  }

  *newVolume = volume;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeVolume(struct volume *volume)
{
  if (volume == NULL) {
    return;
  }

  // If readerThreads is NULL, then we haven't set up the reader threads.
  if (volume->readerThreads != NULL) {
    // Stop the reader threads.  It is ok if there aren't any of them.
    lockMutex(&volume->readThreadsMutex);
    volume->readerState |= READER_STATE_EXIT;
    broadcastCond(&volume->readThreadsCond);
    unlockMutex(&volume->readThreadsMutex);
    unsigned int i;
    for (i = 0; i < volume->numReadThreads; i++) {
      joinThreads(volume->readerThreads[i]);
    }
    FREE(volume->readerThreads);
    volume->readerThreads = NULL;
  }

  // Must close the volume store AFTER freeing the scratch page and the caches
  destroy_volume_page(&volume->scratchPage);
  free_page_cache(volume->pageCache);
  free_sparse_cache(volume->sparseCache);
  close_volume_store(&volume->volumeStore);

  destroyCond(&volume->readThreadsCond);
  destroyCond(&volume->readThreadsReadDoneCond);
  destroyMutex(&volume->readThreadsMutex);
  free_index_page_map(volume->indexPageMap);
  free_radix_sorter(volume->radixSorter);
  FREE(volume->geometry);
  FREE(volume->recordPointers);
  FREE(volume);
}

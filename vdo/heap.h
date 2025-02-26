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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/heap.h#5 $
 */

#ifndef HEAP_H
#define HEAP_H

#include "common.h"

/**
 * Prototype for functions which compare two array elements. All the time
 * complexity claims in this module assume this operation has O(1) time
 * complexity.
 *
 * @param item1  The first element to compare
 * @param item2  The second element to compare
 *
 * @return An integer which is less than, equal to, or greater than 0
 *         depending on whether item1 is less than, equal to, or greater
 *         than item2, respectively
 **/
typedef int heap_comparator(const void *item1, const void *item2);

/**
 * Prototype for functions which swap two array elements.
 *
 * @param item1  The first element to swap
 * @param item2  The second element to swap
 **/
typedef void heap_swapper(void *item1, void *item2);

/**
 * A heap array can be any array of fixed-length elements in which the heap
 * invariant can be established. In a max-heap, every child of a node must be
 * at least as large as its children. Once that invariant is established in an
 * array by calling build_heap(), all the other heap operations may be used on
 * that array.
 **/
struct heap {
	/** the 1-based array of heap elements (nodes) */
	byte *array;
	/** the function to use to compare two elements */
	heap_comparator *comparator;
	/** the function to use to swap two elements */
	heap_swapper *swapper;
	/** the maximum number of elements that can be stored */
	size_t capacity;
	/** the size of every element (in bytes) */
	size_t element_size;
	/** the current number of elements in the heap */
	size_t count;
};

/**
 * Initialize an binary heap by wrapping it around an array of elements.
 *
 * The heap will not own the array it wraps. Use build_heap() subsequently to
 * arrange any elements contained in the array into a valid heap.
 *
 * @param heap          The heap to initialize
 * @param comparator    The function to use to compare two heap elements
 * @param swapper       The function to use to swap two heap elements
 * @param array         The array of elements (not modified by this call)
 * @param capacity      The maximum number of elements which fit in the array
 * @param element_size   The size of every array element, in bytes
 **/
void initialize_heap(struct heap *heap, heap_comparator *comparator,
		     heap_swapper *swapper, void *array, size_t capacity,
		     size_t element_size);

/**
 * Build a max-heap in place in an array (heapify it) by re-ordering the
 * elements to establish the heap invariant. Before calling this function,
 * first copy the elements to be arranged into a heap into the array that was
 * passed to initialize_heap(). This operation has O(N) time complexity in the
 * number of elements in the array.
 *
 * @param heap   The heap to build
 * @param count  The number of elements in the array to build into a heap
 **/
void build_heap(struct heap *heap, size_t count);

/**
 * Check whether the heap is currently empty.
 *
 * @param heap  The heap to query
 *
 * @return <code>true</code> if there are no elements in the heap
 **/
static inline bool is_heap_empty(const struct heap *heap)
{
	return (heap->count == 0);
}

/**
 * Remove the largest element from the top of the heap and restore the heap
 * invariant on the remaining elements. This operation has O(log2(N)) time
 * complexity.
 *
 * @param [in]  heap         The heap to modify
 * @param [out] element_ptr  A pointer to receive the largest element (may be
 *                           NULL if the caller just wishes to discard it)
 *
 * @return <code>false</code> if the heap was empty, so no element was removed
 **/
bool pop_max_heap_element(struct heap *heap, void *element_ptr);

/**
 * Sort the elements contained in a heap.
 *
 * This function re-orders the elements contained in the heap to a sorted
 * array in-place by repeatedly popping the maximum element off the heap and
 * moving it to the spot vacated at the end of the heap array. When the
 * function returns, the heap will be empty and the array will contain the
 * elements in sorted order, from heap minimum to heap maximum. The sort is
 * unstable--relative ordering of equal keys is not preserved. This operation
 * has O(N*log2(N)) time complexity.
 *
 * @param heap  The heap containing the elements to sort
 *
 * @return the number of elements that were sorted
 **/
size_t sort_heap(struct heap *heap);

/**
 * Gets the next sorted heap element and returns a pointer to it, in O(log2(N))
 * time.
 *
 * @param heap  The heap to sort one more step
 *
 * @return a pointer to the element sorted, or NULL if already fully sorted.
 **/
void *sort_next_heap_element(struct heap *heap);

#endif /* HEAP_H */

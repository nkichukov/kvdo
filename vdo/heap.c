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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/heap.c#7 $
 */

#include "heap.h"

#include "errors.h"
#include "logger.h"
#include "numeric.h"

#include "statusCodes.h"

/**********************************************************************/
void initialize_heap(struct heap *heap, heap_comparator *comparator,
		     heap_swapper *swapper, void *array, size_t capacity,
		     size_t element_size)
{
	*heap = (struct heap) {
		.comparator = comparator,
		.swapper = swapper,
		.capacity = capacity,
		.element_size = element_size,
	};
	if (array != NULL) {
		// Calculating child indexes is simplified by pretending the
		// element array is 1-based.
		heap->array = ((byte *) array - element_size);
	}
}

/**********************************************************************/
static void sift_heap_down(struct heap *heap, size_t top_node, size_t last_node)
{
	// Keep sifting until the sub-heap rooted at top_node has no children.
	size_t left_child;
	while ((left_child = (2 * top_node)) <= last_node) {
		// If there are two children, select the largest child to swap
		// with.
		size_t swap_node = left_child;
		if (left_child < last_node) {
			size_t right_child = left_child + heap->element_size;
			if (heap->comparator(&heap->array[left_child],
					     &heap->array[right_child])
			    < 0) {
				swap_node = right_child;
			}
		}

		// Stop sifting if top_node is at least as large as its largest
		// child, which means the heap invariant was restored by the
		// previous swap.
		if (heap->comparator(&heap->array[top_node],
				     &heap->array[swap_node]) >= 0) {
			return;
		}

		// Swap the element we've been sifting down with the larger
		// child.
		heap->swapper(&heap->array[top_node], &heap->array[swap_node]);

		// Descend into the sub-heap rooted at that child, going around
		// the loop again in place of a tail-recursive call to
		// sift_heap_down().
		top_node = swap_node;
	}

	// We sifted the element all the way to a leaf node of the heap, so the
	// heap invariant has now been restored.
}

/**********************************************************************/
void build_heap(struct heap *heap, size_t count)
{
	size_t size, last_parent, last_node, top_node;

	heap->count = min(count, heap->capacity);

	if ((heap->count < 2) || (heap->element_size == 0)) {
		return;
	}

	/*
	 * All the leaf nodes are trivially valid sub-heaps. Starting with the
	 * parent of the right-most leaf node, restore the heap invariant in
	 * that sub-heap by sifting the top node of the sub-heap down into one
	 * of its children's valid sub-heaps (or not, if the top node is already
	 * larger than its children). Continue iterating through all the
	 * interior nodes in the heap, in sort of a reverse breadth-first
	 * traversal, restoring the heap invariant for each (increasingly
	 * larger) sub-heap until we reach the root of the heap. Once we sift
	 * the root node down into one of its two valid children, the entire
	 * heap must be valid, by induction.
	 *
	 * Even though we operate on every node and potentially perform an O(log
	 * N) traversal for each node, the combined probabilities of actually
	 * needing to do a swap and the heights of the sub-heaps sum to a
	 * constant, so restoring a heap from the bottom-up like this has only
	 * O(N) complexity.
	 */
	size = heap->element_size;
	last_parent = size * (heap->count / 2);
	last_node = size * heap->count;
	for (top_node = last_parent; top_node > 0; top_node -= size) {
		sift_heap_down(heap, top_node, last_node);
	}
}

/**********************************************************************/
bool pop_max_heap_element(struct heap *heap, void *element_ptr)
{
	size_t root_node, last_node;

	if (heap->count == 0) {
		return false;
	}

	root_node = (heap->element_size * 1);
	last_node = (heap->element_size * heap->count);

	// Return the maximum element (the root of the heap) if the caller
	// wanted it.
	if (element_ptr != NULL) {
		memcpy(element_ptr, &heap->array[root_node], heap->element_size);
	}

	// Move the right-most leaf node to the vacated root node, reducing the
	// number of elements by one and violating the heap invariant.
	if (root_node != last_node) {
		memcpy(&heap->array[root_node], &heap->array[last_node],
		       heap->element_size);
	}
	heap->count -= 1;
	last_node -= heap->element_size;

	// Restore the heap invariant by sifting the root back down into the
	// heap.
	sift_heap_down(heap, root_node, last_node);
	return true;
}

/**********************************************************************/
static inline size_t sift_and_sort(struct heap *heap, size_t root_node,
				   size_t last_node)
{
	/*
	 * We have a valid heap, so the largest unsorted element is now at the
	 * top of the heap. That element belongs at the start of the
	 * partially-sorted array, preceding all the larger elements that we've
	 * already removed from the heap. Swap that largest unsorted element
	 * with the the right-most leaf node in the heap, moving it to its
	 * sorted position in the array.
	 */
	heap->swapper(&heap->array[root_node], &heap->array[last_node]);
	// The sorted list is now one element larger and valid. The heap is
	// one element smaller, and invalid.
	last_node -= heap->element_size;
	// Restore the heap invariant by sifting the swapped element back down
	// into the heap.
	sift_heap_down(heap, root_node, last_node);
	return last_node;
}

/**********************************************************************/
size_t sort_heap(struct heap *heap)
{
	size_t root_node, last_node, count;

	// All zero-length records are identical and therefore already sorted,
	// as are empty or singleton arrays.
	if ((heap->count < 2) || (heap->element_size == 0)) {
		return heap->count;
	}

	// Get the byte array offset of the root node, and the right-most leaf
	// node in the 1-based array of records that will form the heap.
	root_node = (heap->element_size * 1);
	last_node = (heap->element_size * heap->count);

	while (last_node > root_node) {
		last_node = sift_and_sort(heap, root_node, last_node);
	}

	count = heap->count;
	heap->count = 0;
	return count;
}

/**********************************************************************/
void *sort_next_heap_element(struct heap *heap)
{
	size_t root_node, last_node;

	if ((heap->count == 0) || (heap->element_size == 0)) {
		return NULL;
	}

	// Get the byte array offset of the root node, and the right-most leaf
	// node in the 1-based array of records that will form the heap.
	root_node = (heap->element_size * 1);
	last_node = (heap->element_size * heap->count);
	if (heap->count > 1) {
		sift_and_sort(heap, root_node, last_node);
	}
	heap->count--;

	return &heap->array[last_node];
}

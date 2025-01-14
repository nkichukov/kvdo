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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/pointerMap.h#7 $
 */

#ifndef POINTER_MAP_H
#define POINTER_MAP_H

#include "common.h"

/**
 * A pointer_map associates pointer values (<code>void *</code>) with the data
 * referenced by pointer keys (<code>void *</code>). <code>NULL</code> pointer
 * values are not supported. A <code>NULL</code> key value is supported when
 * the instance's key comparator and hasher functions support it.
 *
 * The map is implemented as hash table, which should provide constant-time
 * insert, query, and remove operations, although the insert may occasionally
 * grow the table, which is linear in the number of entries in the map. The
 * table will grow as needed to hold new entries, but will not shrink as
 * entries are removed.
 *
 * The key and value pointers passed to the map are retained and used by the
 * map, but are not owned by the map. Freeing the map does not attempt to free
 * the pointers. The client is entirely responsible for the memory managment
 * of the keys and values. The current interface and implementation assume
 * that keys will be properties of the values, or that keys will not be memory
 * managed, or that keys will not need to be freed as a result of being
 * replaced when a key is re-mapped.
 **/

struct pointer_map;

/**
 * The prototype of functions that compare the referents of two pointer keys
 * for equality. If two keys are equal, then both keys must have the same the
 * hash code associated with them by the hasher function defined below.

 * @param this_key  The first element to compare
 * @param that_key  The second element to compare
 *
 * @return <code>true</code> if and only if the referents of the two
 *         key pointers are to be treated as the same key by the map
 **/
typedef bool pointer_key_comparator(const void *this_key, const void *that_key);

/**
 * The prototype of functions that get or calculate a hash code associated
 * with the referent of pointer key. The hash code must be uniformly
 * distributed over all uint32_t values. The hash code associated with a given
 * key must not change while the key is in the map. If the comparator function
 * says two keys are equal, then this function must return the same hash code
 * for both keys. This function may be called many times for a key while an
 * entry is stored for it in the map.
 *
 * @param key  The pointer key to hash
 *
 * @return the hash code for the key
 **/
typedef uint32_t pointer_key_hasher(const void *key);

/**
 * Allocate and initialize a pointer_map.
 *
 * @param [in]  initial_capacity  The number of entries the map should
 *                                initially be capable of holding (zero tells
 *                                the map to use its own small default)
 * @param [in]  initial_load      The load factor of the map, expressed as an
 *                                integer percentage (typically in the range
 *                                50 to 90, with zero telling the map to use
 *                                its own default)
 * @param [in]  comparator        The function to use to compare the referents
 *                                of two pointer keys for equality
 * @param [in]  hasher            The function to use obtain the hash code
 *                                associated with each pointer key
 * @param [out] map_ptr           A pointer to hold the new pointer_map
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_pointer_map(size_t initial_capacity,
				  unsigned int initial_load,
				  pointer_key_comparator comparator,
				  pointer_key_hasher hasher,
				  struct pointer_map **map_ptr);

/**
 * Free a pointer_map. NOTE: The map does not own the pointer keys and values
 * stored in the map and they are not freed by this call.
 *
 * @param map  The pointer_map to free
 **/
void free_pointer_map(struct pointer_map *map);

/**
 * Get the number of entries stored in a pointer_map.
 *
 * @param map  The pointer_map to query
 *
 * @return the number of entries in the map
 **/
size_t pointer_map_size(const struct pointer_map *map);

/**
 * Retrieve the value associated with a given key from the pointer_map.
 *
 * @param map  The pointer_map to query
 * @param key  The key to look up (may be <code>NULL</code> if the
 *             comparator and hasher functions support it)
 *
 * @return the value associated with the given key, or <code>NULL</code>
 *         if the key is not mapped to any value
 **/
void *pointer_map_get(struct pointer_map *map, const void *key);

/**
 * Try to associate a value (a pointer) with an integer in a pointer_map.
 * If the map already contains a mapping for the provided key, the old value is
 * only replaced with the specified value if update is true. In either case
 * the old value is returned. If the map does not already contain a value for
 * the specified key, the new value is added regardless of the value of update.
 *
 * If the value stored in the map is updated, then the key stored in the map
 * will also be updated with the key provided by this call. The old key will
 * not be returned due to the memory managment assumptions described in the
 * interface header comment.
 *
 * @param [in]  map            The pointer_map to attempt to modify
 * @param [in]  key            The key with which to associate the new value
 *                             (may be <code>NULL</code> if the comparator and
 *                             hasher functions support it)
 * @param [in]  new_value      The value to be associated with the key
 * @param [in]  update         Whether to overwrite an existing value
 * @param [out] old_value_ptr  A pointer in which to store either the old value
 *                             (if the key was already mapped) or
 *                             <code>NULL</code> if the map did not contain the
 *                             key; <code>NULL</code> may be provided if the
 *                             caller does not need to know the old value
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check pointer_map_put(struct pointer_map *map,
				 const void *key,
				 void *new_value,
				 bool update,
				 void **old_value_ptr);

/**
 * Remove the mapping for a given key from the pointer_map.
 *
 * @param map  The pointer_map from which to remove the mapping
 * @param key  The key whose mapping is to be removed (may be <code>NULL</code>
 *             if the comparator and hasher functions support it)
 *
 * @return the value that was associated with the key, or
 *         <code>NULL</code> if it was not mapped
 **/
void *pointer_map_remove(struct pointer_map *map, const void *key);

#endif /* POINTER_MAP_H */

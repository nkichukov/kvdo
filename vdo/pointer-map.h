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
 */

#ifndef POINTER_MAP_H
#define POINTER_MAP_H

#include "compiler.h"
#include "typeDefs.h"

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

int __must_check make_pointer_map(size_t initial_capacity,
				  unsigned int initial_load,
				  pointer_key_comparator comparator,
				  pointer_key_hasher hasher,
				  struct pointer_map **map_ptr);

void free_pointer_map(struct pointer_map *map);

size_t pointer_map_size(const struct pointer_map *map);

void *pointer_map_get(struct pointer_map *map, const void *key);

int __must_check pointer_map_put(struct pointer_map *map,
				 const void *key,
				 void *new_value,
				 bool update,
				 void **old_value_ptr);

void *pointer_map_remove(struct pointer_map *map, const void *key);

#endif /* POINTER_MAP_H */

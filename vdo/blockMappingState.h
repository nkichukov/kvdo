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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/blockMappingState.h#10 $
 */

#ifndef BLOCK_MAPPING_STATE_H
#define BLOCK_MAPPING_STATE_H

#include "common.h"

/**
 * Four bits of each five-byte block map entry contain a mapping state value
 * used to distinguish unmapped or trimmed logical blocks (which are treated
 * as mapped to the zero block) from entries that have been mapped to a
 * physical block, including the zero block.
 **/
enum block_mapping_state {
	VDO_MAPPING_STATE_UNMAPPED = 0, // Must be zero to be the default value
	VDO_MAPPING_STATE_UNCOMPRESSED = 1, // A normal (uncompressed) block
	VDO_MAPPING_STATE_COMPRESSED_BASE = 2, // Compressed in slot 0
	VDO_MAPPING_STATE_COMPRESSED_MAX = 15, // Compressed in slot 13
};

/**
 * The total number of compressed blocks that can live in a physical block.
 **/
enum {
	VDO_MAX_COMPRESSION_SLOTS = (VDO_MAPPING_STATE_COMPRESSED_MAX
				     - VDO_MAPPING_STATE_COMPRESSED_BASE + 1),
};

/**********************************************************************/
static inline enum block_mapping_state vdo_get_state_for_slot(byte slot_number)
{
	return (slot_number + VDO_MAPPING_STATE_COMPRESSED_BASE);
}

/**********************************************************************/
static inline byte
vdo_get_slot_from_state(enum block_mapping_state mapping_state)
{
	return (mapping_state - VDO_MAPPING_STATE_COMPRESSED_BASE);
}

/**********************************************************************/
static inline bool
vdo_is_state_compressed(const enum block_mapping_state mapping_state)
{
	return (mapping_state > VDO_MAPPING_STATE_UNCOMPRESSED);
}

#endif // BLOCK_MAPPING_STATE_H

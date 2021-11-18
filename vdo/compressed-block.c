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

#include "compressed-block.h"

#include "permassert.h"
#include "string-utils.h"

#include "status-codes.h"

static const struct version_number COMPRESSED_BLOCK_1_0 = {
	.major_version = 1,
	.minor_version = 0,
};

enum {
	COMPRESSED_BLOCK_1_0_SIZE = 4 + 4 + (2 * VDO_MAX_COMPRESSION_SLOTS),
};

/**********************************************************************/
static uint16_t
get_compressed_fragment_size(const struct compressed_block_header *header,
			     byte slot)
{
	return __le16_to_cpu(header->sizes[slot]);
}

/**
 * This method initializes the compressed block in the compressed write
 * agent. Because the compressor already put the agent's compressed fragment at
 * the start of the compressed block's data field, it needn't be copied. So all
 * we need do is initialize the header and set the size of the agent's
 * fragment.
 *
 * @param block  The compressed block to initialize
 * @param size   The size of the agent's fragment
 **/
void vdo_initialize_compressed_block(struct compressed_block *block,
				     uint16_t size)
{
	/*
	 * Make sure the block layout isn't accidentally changed by changing
	 * the length of the block header.
	 */
	STATIC_ASSERT_SIZEOF(struct compressed_block_header,
			     COMPRESSED_BLOCK_1_0_SIZE);

	block->header.version = vdo_pack_version_number(COMPRESSED_BLOCK_1_0);
	block->header.sizes[0] = __cpu_to_le16(size);
}

/**
 * Get a reference to a compressed fragment from a compression block.
 *
 * @param [in]  mapping_state    the mapping state for the look up
 * @param [in]  buffer           buffer that contains compressed data
 * @param [in]  block_size       size of a data block
 * @param [out] fragment_offset  the offset of the fragment within a
 *                               compressed block
 * @param [out] fragment_size    the size of the fragment
 *
 * @return If a valid compressed fragment is found, VDO_SUCCESS;
 *         otherwise, VDO_INVALID_FRAGMENT if the fragment is invalid.
 **/
int vdo_get_compressed_block_fragment(enum block_mapping_state mapping_state,
				      char *buffer,
				      block_size_t block_size,
				      uint16_t *fragment_offset,
				      uint16_t *fragment_size)
{
	uint16_t compressed_size, offset;
	unsigned int i;
	byte slot;
	struct version_number version;
	struct compressed_block_header *header =
		(struct compressed_block_header *) buffer;

	if (!vdo_is_state_compressed(mapping_state)) {
		return VDO_INVALID_FRAGMENT;
	}

	version = vdo_unpack_version_number(header->version);
	if (!vdo_are_same_version(version, COMPRESSED_BLOCK_1_0)) {
		return VDO_INVALID_FRAGMENT;
	}

	slot = vdo_get_slot_from_state(mapping_state);
	if (slot >= VDO_MAX_COMPRESSION_SLOTS) {
		return VDO_INVALID_FRAGMENT;
	}

	compressed_size = get_compressed_fragment_size(header, slot);
	offset = sizeof(struct compressed_block_header);
	for (i = 0; i < slot; i++) {
		offset += get_compressed_fragment_size(header, i);
		if (offset >= block_size) {
			return VDO_INVALID_FRAGMENT;
		}
	}

	if ((offset + compressed_size) > block_size) {
		return VDO_INVALID_FRAGMENT;
	}

	*fragment_offset = offset;
	*fragment_size = compressed_size;
	return VDO_SUCCESS;
}

/**
 * Copy a fragment into the compressed block.
 *
 * @param block      the compressed block
 * @param fragment   the number of the fragment
 * @param offset     the byte offset of the fragment in the data area
 * @param data       a pointer to the compressed data
 * @param size       the size of the data
 *
 * @note no bounds checking -- the data better fit without smashing other stuff
 **/
void vdo_put_compressed_block_fragment(struct compressed_block *block,
				       unsigned int fragment,
				       uint16_t offset,
				       const char *data,
				       uint16_t size)
{
	block->header.sizes[fragment] = __cpu_to_le16(size);
	memcpy(&block->data[offset], data, size);
}

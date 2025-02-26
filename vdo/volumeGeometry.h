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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/volumeGeometry.h#14 $
 */

#ifndef VOLUME_GEOMETRY_H
#define VOLUME_GEOMETRY_H


#include <linux/blk_types.h>
#include <linux/uuid.h>

#include "uds.h"

#include "types.h"

enum {
	GEOMETRY_BLOCK_LOCATION = 0,
};

struct index_config {
	uint32_t mem;
	uint32_t checkpoint_frequency;
	bool sparse;
} __packed;

enum volume_region_id {
	INDEX_REGION = 0,
	DATA_REGION = 1,
	VOLUME_REGION_COUNT,
};

struct volume_region {
	/** The ID of the region */
	enum volume_region_id id;
	/**
	 * The absolute starting offset on the device. The region continues
	 * until the next region begins.
	 */
	physical_block_number_t start_block;
} __packed;

struct volume_geometry {
	/** The release version number of this volume */
	release_version_number_t release_version;
	/** The nonce of this volume */
	nonce_t nonce;
	/** The uuid of this volume */
	uuid_t uuid;
	/** The block offset to be applied to bios */
	block_count_t bio_offset;
	/** The regions in ID order */
	struct volume_region regions[VOLUME_REGION_COUNT];
	/** The index config */
	struct index_config index_config;
} __packed;

/** This volume geometry struct is used for sizing only */
struct volume_geometry_4_0 {
	/** The release version number of this volume */
	release_version_number_t release_version;
	/** The nonce of this volume */
	nonce_t nonce;
	/** The uuid of this volume */
	uuid_t uuid;
	/** The regions in ID order */
	struct volume_region regions[VOLUME_REGION_COUNT];
	/** The index config */
	struct index_config index_config;
} __packed;

/**
 * Get the start of the index region from a geometry.
 *
 * @param geometry  The geometry
 *
 * @return The start of the index region
 **/
static inline physical_block_number_t __must_check
vdo_get_index_region_start(struct volume_geometry geometry)
{
	return geometry.regions[INDEX_REGION].start_block;
}

/**
 * Get the start of the data region from a geometry.
 *
 * @param geometry  The geometry
 *
 * @return The start of the data region
 **/
static inline physical_block_number_t __must_check
vdo_get_data_region_start(struct volume_geometry geometry)
{
	return geometry.regions[DATA_REGION].start_block;
}

/**
 * Get the size of the index region from a geometry.
 *
 * @param geometry  The geometry
 *
 * @return the size of the index region
 **/
static inline physical_block_number_t __must_check
vdo_get_index_region_size(struct volume_geometry geometry)
{
	return vdo_get_data_region_start(geometry) -
		vdo_get_index_region_start(geometry);
}

/**
 * Synchronously read a geometry block from a block device.
 *
 * @param bdev       The block device containing the block to read
 * @param geometry   A volume_geometry to read into
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
vdo_read_geometry_block(struct block_device *bdev,
			struct volume_geometry *geometry);

/**
 * Convert an index config to a UDS configuration, which can be used by UDS.
 *
 * @param index_config    The index config to convert
 * @param uds_config_ptr  A pointer to return the UDS configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
vdo_index_config_to_uds_configuration(const struct index_config *index_config,
				      struct uds_configuration **uds_config_ptr);

/**
 * Modify the uds_parameters to match the requested index config.
 *
 * @param index_config  The index config to convert
 * @param user_params   The uds_parameters to modify
 **/
void vdo_index_config_to_uds_parameters(const struct index_config *index_config,
					struct uds_parameters *user_params);

#endif // VOLUME_GEOMETRY_H

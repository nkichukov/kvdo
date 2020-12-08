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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoInternal.h#46 $
 */

#ifndef VDO_INTERNAL_H
#define VDO_INTERNAL_H

#include "vdo.h"

#include "atomicDefs.h"

#include "adminCompletion.h"
#include "adminState.h"
#include "header.h"
#include "packer.h"
#include "statistics.h"
#include "superBlock.h"
#include "readOnlyNotifier.h"
#include "types.h"
#include "uds.h"
#include "vdoComponent.h"
#include "vdoComponentStates.h"
#include "vdoLayout.h"
#include "vdoState.h"

/**
 * Error counters are atomic since updates can arrive concurrently from
 * arbitrary threads.
 **/
struct atomic_error_statistics {
	// Dedupe path error stats
	atomic64_t invalid_advice_pbn_count;
	atomic64_t no_space_error_count;
	atomic64_t read_only_error_count;
};

struct vdo {
	/* The atomic version of the state of this vdo */
	atomic_t state;
	/* The full state of all components */
	struct vdo_component_states states;
	/* The read-only notifier */
	struct read_only_notifier *read_only_notifier;
	/* The load-time configuration of this vdo */
	struct vdo_load_config load_config;

	/* The super block */
	struct vdo_super_block *super_block;

	/* The physical storage below us */
	PhysicalLayer *layer;

	/* Our partitioning of the physical layer's storage */
	struct vdo_layout *layout;

	/* The block map */
	struct block_map *block_map;

	/* The journal for block map recovery */
	struct recovery_journal *recovery_journal;

	/* The slab depot */
	struct slab_depot *depot;

	/* The compressed-block packer */
	struct packer *packer;
	/* Whether incoming data should be compressed */
	bool compressing;

	/* The handler for flush requests */
	struct flusher *flusher;

	/* The state the vdo was in when loaded (primarily for unit tests) */
	VDOState load_state;
	/* Whether VIO tracing is enabled */
	bool vio_trace_recording;

	/* The logical zones of this vdo */
	struct logical_zones *logical_zones;

	/* The physical zones of this vdo */
	struct physical_zone **physical_zones;

	/* The hash lock zones of this vdo */
	struct hash_zone **hash_zones;

	/* The completion for administrative operations */
	struct admin_completion admin_completion;

	/* The administrative state of the vdo */
	struct admin_state admin_state;

	/* Whether a close is required */
	bool close_required;

	/* Atomic global counts of error events */
	struct atomic_error_statistics error_stats;
};

/**
 * Get the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo  The vdo
 *
 * @return the current state of the vdo
 **/
VDOState __must_check get_vdo_state(const struct vdo *vdo);

/**
 * Set the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo    The vdo whose state is to be set
 * @param state  The new state of the vdo
 **/
void set_vdo_state(struct vdo *vdo, VDOState state);

/**
 * Encode the vdo and save the super block asynchronously. All non-user mode
 * super block savers should use this bottle neck instead of calling
 * saveSuperBlockAsync() directly.
 *
 * @param vdo     The vdo whose state is being saved
 * @param parent  The completion to notify when the save is complete
 **/
void save_vdo_components(struct vdo *vdo, struct vdo_completion *parent);

/**
 * Enable a vdo to enter read-only mode on errors.
 *
 * @param vdo  The vdo to enable
 *
 * @return VDO_SUCCESS or an error
 **/
int enable_read_only_entry(struct vdo *vdo);

/**
 * Get the block map.
 *
 * @param vdo  The vdo whose block map is desired
 *
 * @return the block map from the vdo
 **/
struct block_map * __must_check get_block_map(const struct vdo *vdo);

/**
 * Get the slab depot from a vdo.
 *
 * @param vdo  The vdo whose slab depot is desired
 *
 * @return the slab depot from the vdo
 **/
struct slab_depot * __must_check get_slab_depot(struct vdo *vdo);

/**
 * Get the recovery journal from a vdo.
 *
 * @param vdo  The vdo whose recovery journal is desired
 *
 * @return the recovery journal from the vdo
 **/
struct recovery_journal * __must_check get_recovery_journal(struct vdo *vdo);

/**
 * Check whether a vdo is in read-only mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in read-only mode
 **/
bool __must_check in_read_only_mode(const struct vdo *vdo);

/**
 * Check whether the vdo requires a read-only mode rebuild.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo requires a read-only rebuild
 **/
bool __must_check requires_read_only_rebuild(const struct vdo *vdo);

/**
 * Check whether a vdo requires rebuilding.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo must be rebuilt
 **/
bool __must_check requires_rebuild(const struct vdo *vdo);

/**
 * Check whether a vdo should enter recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo requires recovery
 **/
bool __must_check requires_recovery(const struct vdo *vdo);

/**
 * Check whether a vdo was replaying the recovery journal into the block map
 * when it crashed.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo crashed while reconstructing the
 *         block map
 **/
bool __must_check is_replaying(const struct vdo *vdo);

/**
 * Check whether the vdo is in recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in recovery mode
 **/
bool __must_check in_recovery_mode(const struct vdo *vdo);

/**
 * Put the vdo into recovery mode
 *
 * @param vdo  The vdo
 **/
void enter_recovery_mode(struct vdo *vdo);

/**
 * Assert that we are running on the admin thread.
 *
 * @param vdo   The vdo
 * @param name  The name of the function which should be running on the admin
 *              thread (for logging).
 **/
void assert_on_admin_thread(struct vdo *vdo, const char *name);

/**
 * Assert that this function was called on the specified logical zone thread.
 *
 * @param vdo           The vdo
 * @param logical_zone  The number of the logical zone
 * @param name          The name of the calling function
 **/
void assert_on_logical_zone_thread(const struct vdo *vdo,
				   zone_count_t logical_zone,
				   const char *name);

/**
 * Assert that this function was called on the specified physical zone thread.
 *
 * @param vdo            The vdo
 * @param physical_zone  The number of the physical zone
 * @param name           The name of the calling function
 **/
void assert_on_physical_zone_thread(const struct vdo *vdo,
				    zone_count_t physical_zone,
				    const char *name);

/**
 * Select the hash zone responsible for locking a given chunk name.
 *
 * @param vdo   The vdo containing the hash zones
 * @param name  The chunk name
 *
 * @return  The hash zone responsible for the chunk name
 **/
struct hash_zone * __must_check
select_hash_zone(const struct vdo *vdo, const struct uds_chunk_name *name);

/**
 * Get the physical zone responsible for a given physical block number of a
 * data block in this vdo instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the vdo
 * into read-only mode.
 *
 * @param [in]  vdo       The vdo containing the physical zones
 * @param [in]  pbn       The PBN of the data block
 * @param [out] zone_ptr  A pointer to return the physical zone
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure
 **/
int __must_check get_physical_zone(const struct vdo *vdo,
				   physical_block_number_t pbn,
				   struct physical_zone **zone_ptr);

/**********************************************************************/
// Asynchronous callback to share a duplicate block. This is only public so
// test code may compare it against the current callback in the completion.
void share_block(struct vdo_completion *completion);

#endif /* VDO_INTERNAL_H */
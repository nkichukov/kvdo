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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/allocationSelectorInternals.h#2 $
 */

#ifndef ALLOCATION_SELECTOR_INTERNALS_H
#define ALLOCATION_SELECTOR_INTERNALS_H

#include "types.h"

/** Structure used to select which physical zone to allocate from */
struct allocation_selector {
	/** The number of allocations done in the current zone */
	block_count_t allocation_count;
	/** The physical zone to allocate from next */
	zone_count_t next_allocation_zone;
	/** The number of the last physical zone */
	zone_count_t last_physical_zone;
};

#endif /* ALLOCATION_SELECTOR_INTERNALS_H */

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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/kernelVDOInternals.h#2 $
 */

#ifndef KERNEL_VDO_INTERNALS_H
#define KERNEL_VDO_INTERNALS_H

#include "kernelVDO.h"

/**
 * Enqueue a work item to be performed in the base code in a particular thread.
 *
 * @param thread  The VDO thread on which to run the work item
 * @param item    The work item to be run
 **/
void enqueue_vdo_thread_work(struct vdo_thread *thread,
			     struct vdo_work_item *item);

#endif // KERNEL_VDO_INTERNALS_H

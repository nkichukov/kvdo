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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/dump.h#9 $
 */

#ifndef DUMP_H
#define DUMP_H

#include "kernelLayer.h"

/**
 * Dump internal state and/or statistics to the kernel log, as specified by
 * zero or more string arguments.
 *
 * @param vdo   The vdo
 * @param argc  Number of arguments
 * @param argv  The argument list
 * @param why   Reason for doing the dump
 **/
int vdo_dump(struct vdo *vdo,
	     unsigned int argc,
	     char *const *argv,
	     const char *why);

/**
 * Dump lots of internal state and statistics to the kernel log. Identical to
 * "dump all", without each caller needing to set up the argument list.
 *
 * @param vdo  The vdo
 * @param why  Reason for doing the dump
 **/
void vdo_dump_all(struct vdo *vdo, const char *why);

#endif // DUMP_H

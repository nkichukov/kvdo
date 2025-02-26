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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/messageStats.h#7 $
 *
 */

#ifndef MESSAGE_STATS_H
#define MESSAGE_STATS_H

#include "types.h"

/**
 * Write vdo statistics to a buffer
 *
 * @param vdo     the vdo
 * @param buf     pointer to the buffer
 * @param maxlen  the maximum length of the buffer
 */
int write_vdo_stats(struct vdo *vdo, char *buf, unsigned int maxlen);

#endif  /* MESSAGE_STATS_H */

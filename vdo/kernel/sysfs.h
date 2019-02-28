/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/sysfs.h#2 $
 */

#ifndef SYSFS_H
#define SYSFS_H

#include <linux/kobject.h>

/**
 * Initialize the sysfs objects global to all VDO devices.
 *
 * @param module_object  The kvdo module's global kobject
 */
int vdo_init_sysfs(struct kobject *module_object);

/**
 * Release the global sysfs objects.
 *
 * @param module_object  The kvdo module's global kobject
 */
void vdo_put_sysfs(struct kobject *module_object);

#endif /* SYSFS_H */

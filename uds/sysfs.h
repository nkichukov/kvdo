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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/sysfs.h#4 $
 */

#ifndef SYSFS_H
#define SYSFS_H

/**
 * Called when the module is loaded to initialize the /sys/\<module_name\>
 * tree.
 *
 * @return 0 on success, or non-zero on error
 **/
int init_uds_sysfs(void);

/**
 * Called when the module is being unloaded to terminate the
 * /sys/\<module_name\> tree.
 **/
void put_uds_sysfs(void);

#endif /*  SYSFS_H  */

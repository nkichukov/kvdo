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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/readOnlyNotifier.h#7 $
 */

/*
 * A read_only_notifier is responsible for propogating the fact that the VDO
 * has encountered an unrecoverable error to all base threads. It also persists
 * the read-only state to the super block.
 *
 * The notifier also provides the ability to wait for any notifications to be
 * complete in order to not cause super block write races when shutting down
 * the VDO.
 */

#ifndef READ_ONLY_NOTIFIER_H
#define READ_ONLY_NOTIFIER_H

#include "completion.h"

/**
 * A function to notify a listener that the VDO has gone read-only.
 *
 * @param listener  The object to notify
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
typedef void vdo_read_only_notification(void *listener,
					struct vdo_completion *parent);

/**
 * Create a read-only notifer.
 *
 * @param [in]  is_read_only     Whether the VDO is already read-only
 * @param [in]  thread_config    The thread configuration of the VDO
 * @param [in]  vdo              The VDO
 * @param [out] notifier_ptr     A pointer to receive the new notifier
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_vdo_read_only_notifier(bool is_read_only,
			    const struct thread_config *thread_config,
			    struct vdo *vdo,
			    struct read_only_notifier **notifier_ptr);

/**
 * Free a read_only_notifier.
 *
 * @param notifier  The notifier to free
 **/
void free_vdo_read_only_notifier(struct read_only_notifier *notifier);

/**
 * Wait until no read-only notifications are in progress and prevent any
 * subsequent notifications. Notifications may be re-enabled by calling
 * vdo_allow_read_only_mode_entry().
 *
 * @param notifier  The read-only notifier on which to wait
 * @param parent    The completion to notify when no threads are entering
 *                  read-only mode
 **/
void
vdo_wait_until_not_entering_read_only_mode(struct read_only_notifier *notifier,
					   struct vdo_completion *parent);

/**
 * Allow the notifier to put the VDO into read-only mode, reversing the effects
 * of vdo_wait_until_not_entering_read_only_mode(). If some thread tried to put
 * the VDO into read-only mode while notifications were disallowed, it will be
 * done when this method is called. If that happens, the parent will not be
 * notified until the VDO has actually entered read-only mode and attempted to
 * save the super block.
 *
 * <p>This method may only be called from the admin thread.
 *
 * @param notifier  The notifier
 * @param parent    The object to notify once the operation is complete
 **/
void vdo_allow_read_only_mode_entry(struct read_only_notifier *notifier,
				    struct vdo_completion *parent);

/**
 * Put a VDO into read-only mode and save the read-only state in the super
 * block. This method is a no-op if the VDO is already read-only.
 *
 * @param notifier         The read-only notifier of the VDO
 * @param error_code       The error which caused the VDO to enter read-only
 *                         mode
 **/
void vdo_enter_read_only_mode(struct read_only_notifier *notifier,
			      int error_code);

/**
 * Check whether the VDO is read-only. This method may be called from any
 * thread, as opposed to examining the VDO's state field which is only safe
 * to check from the admin thread.
 *
 * @param notifier        The read-only notifier of the VDO
 *
 * @return <code>true</code> if the VDO is read-only
 **/
bool __must_check vdo_is_read_only(struct read_only_notifier *notifier);

/**
 * Check whether the VDO is or will be read-only (i.e. some thread has started
 * the process of entering read-only mode, but not all threads have been
 * notified yet). This method should only be called in cases where the expense
 * of reading atomic state is not a problem. It was introduced in order to allow
 * suppresion of spurious error messages resulting from VIO cleanup racing with
 * read-only notification.
 *
 * @param notifier  The read-only notifier of the VDO
 *
 * @return <code>true</code> if the VDO has started (and possibly finished)
 *         the process of entering read-only mode
 **/
bool __must_check
vdo_is_or_will_be_read_only(struct read_only_notifier *notifier);

/**
 * Register a listener to be notified when the VDO goes read-only.
 *
 * @param notifier       The notifier to register with
 * @param listener       The object to notify
 * @param notification   The function to call to send the notification
 * @param thread_id      The id of the thread on which to send the notification
 *
 * @return VDO_SUCCESS or an error
 **/
int register_vdo_read_only_listener(struct read_only_notifier *notifier,
				    void *listener,
				    vdo_read_only_notification *notification,
				    thread_id_t thread_id);

#endif /* READ_ONLY_NOTIFIER_H */

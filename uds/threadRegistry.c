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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/threadRegistry.c#10 $
 */

#include "threadRegistry.h"

#include <linux/rculist.h>

#include "permassert.h"

/*
 * We need to be careful when using other facilities that may use
 * threadRegistry functions in their normal operation.  For example,
 * we do not want to invoke the logger while holding a lock.
 */

/**********************************************************************/
void uds_initialize_thread_registry(struct thread_registry *registry)
{
	INIT_LIST_HEAD(&registry->links);
	spin_lock_init(&registry->lock);
}

/**********************************************************************/
void uds_register_thread(struct thread_registry *registry,
			 struct registered_thread *new_thread,
			 const void *pointer)
{
	struct registered_thread *thread;
	bool found_it = false;

	INIT_LIST_HEAD(&new_thread->links);
	new_thread->pointer = pointer;
	new_thread->task = current;

	spin_lock(&registry->lock);
	list_for_each_entry(thread, &registry->links, links) {
		if (thread->task == current) {
			// This should not have been there.
			// We'll complain after releasing the lock.
			list_del_rcu(&thread->links);
			found_it = true;
			break;
		}
	}
	list_add_tail_rcu(&new_thread->links, &registry->links);
	spin_unlock(&registry->lock);

	ASSERT_LOG_ONLY(!found_it, "new thread not already in registry");
	if (found_it) {
		// Ensure no RCU iterators see it before re-initializing.
		synchronize_rcu();
		INIT_LIST_HEAD(&thread->links);
	}
}

/**********************************************************************/
void uds_unregister_thread(struct thread_registry *registry)
{
	struct registered_thread *thread;
	bool found_it = false;

	spin_lock(&registry->lock);
	list_for_each_entry(thread, &registry->links, links) {
		if (thread->task == current) {
			list_del_rcu(&thread->links);
			found_it = true;
			break;
		}
	}
	spin_unlock(&registry->lock);

	ASSERT_LOG_ONLY(found_it, "thread found in registry");
	if (found_it) {
		// Ensure no RCU iterators see it before re-initializing.
		synchronize_rcu();
		INIT_LIST_HEAD(&thread->links);
	}
}

/**********************************************************************/
const void *uds_lookup_thread(struct thread_registry *registry)
{
	struct registered_thread *thread;
	const void *result = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(thread, &registry->links, links) {
		if (thread->task == current) {
			result = thread->pointer;
			break;
		}
	}
	rcu_read_unlock();

	return result;
}

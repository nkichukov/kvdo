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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/completion.h#22 $
 */

#ifndef COMPLETION_H
#define COMPLETION_H

#include "permassert.h"

#include "statusCodes.h"
#include "types.h"

#include "workQueue.h"

enum vdo_completion_type {
	// Keep VDO_UNSET_COMPLETION_TYPE at the top.
	VDO_UNSET_COMPLETION_TYPE = 0,

	// Keep this block in sorted order. If you add or remove an entry, be
	// sure to update the corresponding list in completion.c.
	VDO_ACTION_COMPLETION,
	VDO_ADMIN_COMPLETION,
	VDO_BLOCK_ALLOCATOR_COMPLETION,
	VDO_BLOCK_MAP_RECOVERY_COMPLETION,
	VDO_EXTENT_COMPLETION,
	VDO_FLUSH_NOTIFICATION_COMPLETION,
	VDO_GENERATION_FLUSHED_COMPLETION,
	VDO_LOCK_COUNTER_COMPLETION,
	VDO_PAGE_COMPLETION,
	VDO_PARTITION_COPY_COMPLETION,
	VDO_READ_ONLY_MODE_COMPLETION,
	VDO_READ_ONLY_REBUILD_COMPLETION,
	VDO_RECOVERY_COMPLETION,
	VDO_REFERENCE_COUNT_REBUILD_COMPLETION,
	VDO_SLAB_SCRUBBER_COMPLETION,
	VDO_SUB_TASK_COMPLETION,
	VDO_SYNC_COMPLETION,
	VIO_COMPLETION,


	// Keep VDO_MAX_COMPLETION_TYPE at the bottom.
	VDO_MAX_COMPLETION_TYPE
} __packed;

/**
 * An asynchronous VDO operation.
 *
 * @param completion    the completion of the operation
 **/
typedef void vdo_action(struct vdo_completion *completion);

struct vdo_completion {
	/** The type of completion this is */
	enum vdo_completion_type type;

	/**
	 * <code>true</code> once the processing of the operation is complete.
	 * This flag should not be used by waiters external to the VDO base as
	 * it is used to gate calling the callback.
	 **/
	bool complete;

	/**
	 * If true, queue this completion on the next callback invocation, even
	 *if it is already running on the correct thread.
	 **/
	bool requeue;

	/** The ID of the thread which should run the next callback */
	thread_id_t callback_thread_id;

	/** The result of the operation */
	int result;

	/** The VDO on which this completion operates */
	struct vdo *vdo;

	/** The callback which will be called once the operation is complete */
	vdo_action *callback;

	/** Callback which, if set, will be called if an error result is set */
	vdo_action *error_handler;

	/** The parent object, if any, that spawned this completion */
	void *parent;

	/** The work item for enqueuing this completion */
	struct vdo_work_item work_item;
};

/**
 * Actually run the callback. This function must be called from the correct
 * callback thread.
 **/
static inline void
run_vdo_completion_callback(struct vdo_completion *completion)
{
	if ((completion->result != VDO_SUCCESS) &&
	    (completion->error_handler != NULL)) {
		completion->error_handler(completion);
		return;
	}

	completion->callback(completion);
}

/**
 * Set the result of a completion. Older errors will not be masked.
 *
 * @param completion The completion whose result is to be set
 * @param result     The result to set
 **/
void set_vdo_completion_result(struct vdo_completion *completion, int result);

/**
 * Initialize a completion to a clean state, for reused completions.
 *
 * @param completion The completion to initialize
 * @param vdo        The VDO instance
 * @param type       The type of the completion
 **/
void initialize_vdo_completion(struct vdo_completion *completion,
			       struct vdo *vdo,
			       enum vdo_completion_type type);

/**
 * Reset a completion to a clean state, while keeping
 * the type, vdo and parent information.
 *
 * @param completion the completion to reset
 **/
void reset_vdo_completion(struct vdo_completion *completion);

/**
 * Invoke the callback of a completion. If called on the correct thread (i.e.
 * the one specified in the completion's callback_thread_id field), the
 * completion will be run immediately. Otherwise, the completion will be
 * enqueued on the correct callback thread.
 **/
void invoke_vdo_completion_callback(struct vdo_completion *completion);

/**
 * Continue processing a completion by setting the current result and calling
 * invoke_vdo_completion_callback().
 *
 * @param completion  The completion to continue
 * @param result      The current result (will not mask older errors)
 **/
void continue_vdo_completion(struct vdo_completion *completion, int result);

/**
 * Complete a completion.
 *
 * @param completion  The completion to complete
 **/
void complete_vdo_completion(struct vdo_completion *completion);

/**
 * Finish a completion.
 *
 * @param completion The completion to finish
 * @param result     The result of the completion (will not mask older errors)
 **/
static inline void finish_vdo_completion(struct vdo_completion *completion,
					 int result)
{
	set_vdo_completion_result(completion, result);
	complete_vdo_completion(completion);
}

/**
 * A callback to finish the parent of a completion.
 *
 * @param completion  The completion which has finished and whose parent should
 *                    be finished
 **/
void finish_vdo_completion_parent_callback(struct vdo_completion *completion);

/**
 * Error handler which preserves an error in the parent (if there is one),
 * and then resets the failing completion and calls its non-error callback.
 *
 * @param completion  The completion which failed
 **/
void
preserve_vdo_completion_error_and_continue(struct vdo_completion *completion);

/**
 * A callback which does nothing. This callback is intended to be set as an
 * error handler in the case where an error should do nothing.
 *
 * @param completion  The completion being called back
 **/
static inline
void noop_vdo_completion_callback(struct vdo_completion *completion __always_unused)
{
}

/**
 * Assert that a completion is of the correct type
 *
 * @param actual    The actual completion type
 * @param expected  The expected completion type
 *
 * @return          VDO_SUCCESS or VDO_PARAMETER_MISMATCH
 **/
int assert_vdo_completion_type(enum vdo_completion_type actual,
			       enum vdo_completion_type expected);

/**
 * Return the name of a completion type.
 *
 * @param completion_type  the completion type
 *
 * @return a pointer to a static string; if the completion_type is unknown
 *         this is to a static buffer that may be overwritten.
 **/
const char *
get_vdo_completion_type_name(enum vdo_completion_type completion_type);

/**
 * Set the callback for a completion.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 **/
static inline void
set_vdo_completion_callback(struct vdo_completion *completion,
			    vdo_action *callback,
			    thread_id_t thread_id)
{
	completion->callback = callback;
	completion->callback_thread_id = thread_id;
}

/**
 * Set the callback for a completion and invoke it immediately.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 **/
static inline void
launch_vdo_completion_callback(struct vdo_completion *completion,
			       vdo_action *callback,
			       thread_id_t thread_id)
{
	set_vdo_completion_callback(completion, callback, thread_id);
	invoke_vdo_completion_callback(completion);
}

/**
 * Set the callback and parent for a completion.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 * @param parent      The new parent of the completion
 **/
static inline void
set_vdo_completion_callback_with_parent(struct vdo_completion *completion,
					vdo_action *callback,
					thread_id_t thread_id,
					void *parent)
{
	set_vdo_completion_callback(completion, callback, thread_id);
	completion->parent = parent;
}

/**
 * Set the callback and parent for a completion and invoke the callback
 * immediately.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param thread_id   The ID of the thread on which the callback should run
 * @param parent      The new parent of the completion
 **/
static inline void
launch_vdo_completion_callback_with_parent(struct vdo_completion *completion,
					   vdo_action *callback,
					   thread_id_t thread_id,
					   void *parent)
{
	set_vdo_completion_callback_with_parent(completion, callback,
						thread_id, parent);
	invoke_vdo_completion_callback(completion);
}

/**
 * Prepare a completion for launch. Reset it, and then set its callback, error
 * handler, callback thread, and parent.
 *
 * @param completion     The completion
 * @param callback       The callback to register
 * @param error_handler	 The error handler to register
 * @param thread_id      The ID of the thread on which the callback should run
 * @param parent         The new parent of the completion
 **/
static inline void prepare_vdo_completion(struct vdo_completion *completion,
					  vdo_action *callback,
					  vdo_action *error_handler,
					  thread_id_t thread_id,
					  void *parent)
{
	reset_vdo_completion(completion);
	set_vdo_completion_callback_with_parent(completion, callback,
						thread_id, parent);
	completion->error_handler = error_handler;
}

/**
 * Prepare a completion for launch ensuring that it will always be requeued.
 * Reset it, and then set its callback, error handler, callback thread, and
 * parent.
 *
 * @param completion     The completion
 * @param callback       The callback to register
 * @param error_handler  The error handler to register
 * @param thread_id      The ID of the thread on which the callback should run
 * @param parent         The new parent of the completion
 **/
static inline void
prepare_vdo_completion_for_requeue(struct vdo_completion *completion,
				   vdo_action *callback,
				   vdo_action *error_handler,
				   thread_id_t thread_id,
				   void *parent)
{
	prepare_vdo_completion(completion,
			       callback,
			       error_handler,
			       thread_id,
			       parent);
	completion->requeue = true;
}

/**
 * Prepare a completion for launch which will complete its parent when
 * finished.
 *
 * @param completion  The completion
 * @param parent      The parent to complete
 **/
static inline void
prepare_vdo_completion_to_finish_parent(struct vdo_completion *completion,
					struct vdo_completion *parent)
{
	prepare_vdo_completion(completion,
			       finish_vdo_completion_parent_callback,
			       finish_vdo_completion_parent_callback,
			       parent->callback_thread_id,
			       parent);
}

/**
 * A function to enqueue a vdo_completion to run on the thread specified by its
 * callback_thread_id field.
 *
 * @param completion  The completion to be enqueued
 **/
void enqueue_vdo_completion(struct vdo_completion *completion);

#endif // COMPLETION_H

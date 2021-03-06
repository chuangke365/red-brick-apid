/*
 * redapid
 * Copyright (C) 2014-2015 Matthias Bolte <matthias@tinkerforge.com>
 *
 * object.c: Object implementation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * the RED Brick API is object oriented. the Object type is the base for all
 * objects. it has an internal and external reference count and a lock count.
 * if the sum of the reference counts drops to zero the object is destroyed.
 *
 * the reference count is split into two to protect against users that release
 * more references than they actually own. this would allow a user to destroy
 * an object while it is still in use by other objects resulting in a crash.
 * with the two reference counts a user cannot release internal references.
 *
 * a lock count greater zero indicates that the object is locked. typically
 * the lock count is increased and decreased along with the internal reference
 * count. for some object types locked means write protected. currently the
 * String and List objects interpret locked as write protected. for example,
 * the open function of the File object will take an internal reference to the
 * name String object and lock it. this stops the user from modifying the name
 * String object behind the back of the File object.
 */

#include <errno.h>
#include <stdlib.h>

#include <daemonlib/log.h>

#include "object.h"

#include "inventory.h"

static LogSource _log_source = LOG_SOURCE_INITIALIZER;

const char *object_get_type_name(ObjectType type) {
	switch (type) {
	case OBJECT_TYPE_STRING:    return "string";
	case OBJECT_TYPE_LIST:      return "list";
	case OBJECT_TYPE_FILE:      return "file";
	case OBJECT_TYPE_DIRECTORY: return "directory";
	case OBJECT_TYPE_PROCESS:   return "process";
	case OBJECT_TYPE_PROGRAM:   return "program";

	default:                    return "<unknown>";
	}
}

bool object_is_valid_type(ObjectType type) {
	switch (type) {
	case OBJECT_TYPE_STRING:
	case OBJECT_TYPE_LIST:
	case OBJECT_TYPE_FILE:
	case OBJECT_TYPE_DIRECTORY:
	case OBJECT_TYPE_PROCESS:
	case OBJECT_TYPE_PROGRAM:
		return true;

	default:
		return false;
	}
}

APIE object_create(Object *object, ObjectType type, Session *session,
                   uint32_t create_flags, ObjectDestroyFunction destroy,
                   ObjectSignatureFunction signature) {
	APIE error_code;

	object->id = OBJECT_ID_ZERO;
	object->type = type;
	object->destroy = destroy;
	object->signature = signature;
	object->internal_reference_count = 0;
	object->external_reference_count = 0;
	object->lock_count = 0;

	node_reset(&object->external_reference_sentinel);

	// OBJECT_CREATE_FLAG_INTERNAL or OBJECT_CREATE_FLAG_EXTERNAL has to be used
	if ((create_flags & (OBJECT_CREATE_FLAG_INTERNAL | OBJECT_CREATE_FLAG_EXTERNAL)) == 0) {
		log_error("Invalid object create flags 0x%04X", create_flags);

		return API_E_INTERNAL_ERROR;
	}

	// session has to be valied if OBJECT_CREATE_FLAG_EXTERNAL is used
	if ((create_flags & OBJECT_CREATE_FLAG_EXTERNAL) != 0 && session == NULL) {
		log_error("Missing session for external reference");

		return API_E_INTERNAL_ERROR;
	}

	// OBJECT_CREATE_FLAG_LOCKED can only be used in combination with OBJECT_CREATE_FLAG_INTERNAL
	if ((create_flags & OBJECT_CREATE_FLAG_LOCKED) != 0 &&
	    (create_flags & OBJECT_CREATE_FLAG_INTERNAL) == 0) {
		log_error("Invalid object create flags 0x%04X", create_flags);

		return API_E_INTERNAL_ERROR;
	}

	if ((create_flags & OBJECT_CREATE_FLAG_INTERNAL) != 0) {
		++object->internal_reference_count;
	}

	if ((create_flags & OBJECT_CREATE_FLAG_EXTERNAL) != 0) {
		error_code = object_add_external_reference(object, session);

		if (error_code != API_E_SUCCESS) {
			return error_code;
		}
	}

	if ((create_flags & OBJECT_CREATE_FLAG_LOCKED) != 0) {
		++object->lock_count;
	}

	return inventory_add_object(object);
}

void object_destroy(Object *object) {
	ExternalReference *external_reference;
	Session *session;

	if (object->internal_reference_count != 0 || object->external_reference_count != 0) {
		log_warn("Destroying %s object (id: %u) while there are still references (internal: %d, external: %d) to it",
		         object_get_type_name(object->type), object->id,
		         object->internal_reference_count, object->external_reference_count);
	}

	while (object->external_reference_sentinel.next != &object->external_reference_sentinel) {
		external_reference = containerof(object->external_reference_sentinel.next, ExternalReference, object_node);
		session = external_reference->session;

		node_remove(&external_reference->object_node);
		node_remove(&external_reference->session_node);

		object->external_reference_count -= external_reference->count;
		session->external_reference_count -= external_reference->count;

		free(external_reference);
	}

	if (object->lock_count > 0) {
		log_warn("Destroying %s object (id: %u) while it is still locked (lock-count: %d)",
		         object_get_type_name(object->type), object->id, object->lock_count);
	}

	if (object->destroy != NULL) {
		object->destroy(object);
	}
}

void object_log_signature(Object *object) {
	char signature[OBJECT_MAX_SIGNATURE_LENGTH] = "<unknown>";

	object->signature(object, signature);

	log_object_debug("Object (id: %u, type: %s, internal-reference-count: %d, external-reference-count: %d, lock-count: %d%s%s)",
	                 object->id, object_get_type_name(object->type),
	                 object->internal_reference_count, object->external_reference_count, object->lock_count,
	                 signature != NULL ? ", " : "", signature);
}

// public API
APIE object_release(Object *object, Session *session) {
	if (object->external_reference_count == 0) {
		log_warn("Cannot remove external %s object (id: %u) reference, external reference count is already zero",
		         object_get_type_name(object->type), object->id);

		return API_E_INVALID_OPERATION;
	}

	object_remove_external_reference(object, session);

	return API_E_SUCCESS;
}

// public API
PacketE object_release_unchecked(Object *object, Session *session) {
	return object_release(object, session) == API_E_SUCCESS ? PACKET_E_SUCCESS : PACKET_E_UNKNOWN_ERROR;
}

void object_add_internal_reference(Object *object) {
	log_object_debug("Adding an internal %s object (id: %u) reference (count: %d +1)",
	                 object_get_type_name(object->type), object->id,
	                 object->internal_reference_count);

	++object->internal_reference_count;
}

void object_remove_internal_reference(Object *object) {
	if (object->internal_reference_count == 0) {
		log_error("Cannot remove internal %s object (id: %u) reference, internal reference count is already zero",
		          object_get_type_name(object->type), object->id);

		return;
	}

	log_object_debug("Removing an internal %s object (id: %u) reference (count: %d -1)",
	                 object_get_type_name(object->type), object->id,
	                 object->internal_reference_count);

	--object->internal_reference_count;

	// destroy object if last reference was removed
	if (object->internal_reference_count == 0 && object->external_reference_count == 0) {
		inventory_remove_object(object); // calls object_destroy
	}
}

APIE object_add_external_reference(Object *object, Session *session) {
	Node *external_reference_object_node = object->external_reference_sentinel.next;
	ExternalReference *external_reference;
	APIE error_code;

	// check if there is already an external reference
	while (external_reference_object_node != &object->external_reference_sentinel) {
		external_reference = containerof(external_reference_object_node, ExternalReference, object_node);

		if (external_reference->session == session) {
			if (object->id != OBJECT_ID_ZERO) {
				// only log a message if this is not the initial call from
				// object_create were the object is not fully initialized yet
				log_object_debug("Adding an external %s object (id: %u) reference (count: %d +1) to session (id: %u)",
				                 object_get_type_name(object->type), object->id,
				                 object->external_reference_count, session->id);
			}

			++external_reference->count;
			++object->external_reference_count;
			++session->external_reference_count;

			return API_E_SUCCESS;
		}

		external_reference_object_node = external_reference_object_node->next;
	}

	// create new external reference
	external_reference = calloc(1, sizeof(ExternalReference));

	if (external_reference == NULL) {
		error_code = API_E_NO_FREE_MEMORY;

		log_error("Could not allocate external reference: %s (%d)",
		          get_errno_name(ENOMEM), ENOMEM);

		return error_code;
	}

	if (object->id != OBJECT_ID_ZERO) {
		// only log a message if this is not the initial call from
		// object_create were the object is not fully initialized yet
		log_object_debug("Adding an external %s object (id: %u) reference (count: %d +1) to session (id: %u)",
		                 object_get_type_name(object->type), object->id,
		                 object->external_reference_count, session->id);
	}

	node_reset(&external_reference->object_node);
	node_insert_before(&object->external_reference_sentinel, &external_reference->object_node);

	node_reset(&external_reference->session_node);
	node_insert_before(&session->external_reference_sentinel, &external_reference->session_node);

	external_reference->object = object;
	external_reference->session = session;
	external_reference->count = 1;

	++object->external_reference_count;
	++session->external_reference_count;

	return API_E_SUCCESS;
}

void object_remove_external_reference(Object *object, Session *session) {
	Node *external_reference_object_node;
	ExternalReference *external_reference;

	if (object->external_reference_count == 0) {
		log_warn("Cannot remove external %s object (id: %u) reference, external reference count is already zero",
		         object_get_type_name(object->type), object->id);

		return;
	}

	external_reference_object_node = object->external_reference_sentinel.next;

	while (external_reference_object_node != &object->external_reference_sentinel) {
		external_reference = containerof(external_reference_object_node, ExternalReference, object_node);

		if (external_reference->session == session) {
			log_object_debug("Removing an internal %s object (id: %u) reference (count: %d -1) from session (id: %u)",
			                 object_get_type_name(object->type), object->id,
			                 object->external_reference_count, session->id);

			--external_reference->count;
			--object->external_reference_count;
			--session->external_reference_count;

			if (external_reference->count == 0) {
				node_remove(&external_reference->object_node);
				node_remove(&external_reference->session_node);

				free(external_reference);
			}

			// destroy object if last reference was removed
			if (object->internal_reference_count == 0 && object->external_reference_count == 0) {
				inventory_remove_object(object); // calls object_destroy
			}

			return;
		}

		external_reference_object_node = external_reference_object_node->next;
	}

	log_error("Could not find external %s object (id: %u) reference in session (id: %u)",
	          object_get_type_name(object->type), object->id, session->id);
}

void object_lock(Object *object) {
	log_object_debug("Locking %s object (id: %u, lock-count: %d +1)",
	                 object_get_type_name(object->type), object->id, object->lock_count);

	++object->lock_count;
}

void object_unlock(Object *object) {
	if (object->lock_count == 0) {
		log_error("Cannot unlock already unlock %s object (id: %u)",
		          object_get_type_name(object->type), object->id);

		return;
	}

	log_object_debug("Unlocking %s object (id: %u, lock-count: %d -1)",
	                 object_get_type_name(object->type), object->id, object->lock_count);

	--object->lock_count;
}

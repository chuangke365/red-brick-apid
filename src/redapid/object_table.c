/*
 * redapid
 * Copyright (C) 2014 Matthias Bolte <matthias@tinkerforge.com>
 *
 * object_table.h: Table of Objects
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
 * the RED Brick API operates with different types of objects. each object is
 * referenced by a uint16_t object ID. there is only one number space that is
 * shared between all object types. this means that there can be at most 64k
 * objects in the system and that each object ID can be in use at most once at
 * the same time. object ID 0 is reserved to indicate an error condition. this
 * results in possible valid object ID values from 1 to 65535 (inclusive).
 *
 * the system keeps track of object IDs in multiple arrays. initially the
 * objects and free_ids arrays are empty and next_id is 1. when acquiring
 * a object ID the system checks if free_ids is not empty. in this case a
 * object ID is removed from this array and returned. if free_ids is empty
 * (no object ID has been released yet) then next_id is check. if its not
 * equal to 0, then there still object IDs that have never been acquired and
 * next_id is returned and increased. if a object ID is released it is added
 * to the free_ids array to be acquired again.
 */

#include <errno.h>

#include <daemonlib/array.h>
#include <daemonlib/log.h>
#include <daemonlib/utils.h>

#include "object_table.h"

#include "api.h"

#define LOG_CATEGORY LOG_CATEGORY_API

typedef struct {
	ObjectID id;
	ObjectType type;
	void *data;
	FreeFunction function;
} Object;

static ObjectID _next_id = 1;
static Array _objects[MAX_OBJECT_TYPES];
static Array _free_ids;
static int _iteration_index[MAX_OBJECT_TYPES] = { -1, -1, -1, -1 };

static const char *object_table_get_object_type_name(ObjectType type) {
	switch (type) {
	case OBJECT_TYPE_STRING:
		return "string";

	case OBJECT_TYPE_FILE:
		return "file";

	case OBJECT_TYPE_DIRECTORY:
		return "directory";

	case OBJECT_TYPE_PROGRAM:
		return "program";

	default:
		return "<unknown>";
	}
}

static void object_destroy(Object *object) {
	ObjectID id = object->id;
	ObjectType type = object->type;

	log_debug("Destroying %s object (id: %u)",
	          object_table_get_object_type_name(type), id);

	if (object->function != NULL) {
		object->function(object->data);
	}

	log_debug("Destroyed %s object (id: %u)",
	          object_table_get_object_type_name(type), id);
}

int object_table_init(void) {
	int phase = 0;

	log_debug("Initializing Object subsystem");

	// allocate object arrays
	if (array_create(&_objects[OBJECT_TYPE_STRING], 32, sizeof(Object), 1) < 0) {
		log_error("Could not create string object array: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 1;

	if (array_create(&_objects[OBJECT_TYPE_FILE], 32, sizeof(Object), 1) < 0) {
		log_error("Could not create file object array: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 2;

	if (array_create(&_objects[OBJECT_TYPE_DIRECTORY], 32, sizeof(Object), 1) < 0) {
		log_error("Could not create directory object array: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 3;

	if (array_create(&_objects[OBJECT_TYPE_PROGRAM], 32, sizeof(Object), 1) < 0) {
		log_error("Could not create program object array: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 4;

	if (array_create(&_free_ids, 32, sizeof(ObjectID), 1) < 0) {
		log_error("Could not create free object ID array: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 5;

cleanup:
	switch (phase) { // no breaks, all cases fall through intentionally
	case 4:
		array_destroy(&_objects[OBJECT_TYPE_PROGRAM], (FreeFunction)object_destroy);

	case 3:
		array_destroy(&_objects[OBJECT_TYPE_DIRECTORY], (FreeFunction)object_destroy);

	case 2:
		array_destroy(&_objects[OBJECT_TYPE_FILE], (FreeFunction)object_destroy);

	case 1:
		array_destroy(&_objects[OBJECT_TYPE_STRING], (FreeFunction)object_destroy);

	default:
		break;
	}

	return phase == 5 ? 0 : -1;
}

void object_table_exit(void) {
	log_debug("Shutting down Object subsystem");

	// destroy all objects that could have references to string objects...
	array_destroy(&_objects[OBJECT_TYPE_PROGRAM], (FreeFunction)object_destroy);
	array_destroy(&_objects[OBJECT_TYPE_DIRECTORY], (FreeFunction)object_destroy);
	array_destroy(&_objects[OBJECT_TYPE_FILE], (FreeFunction)object_destroy);

	// ...before destroying the remaining string objects...
	array_destroy(&_objects[OBJECT_TYPE_STRING], (FreeFunction)object_destroy);

	// ...before destroying the free IDs array
	array_destroy(&_free_ids, NULL);
}

ObjectID object_table_add_object(ObjectType type, void *data, FreeFunction function) {
	Object *object;
	int last;

	if (type < OBJECT_TYPE_STRING || type > OBJECT_TYPE_PROGRAM) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object type %d", type);

		return OBJECT_ID_INVALID;
	}

	log_debug("Adding %s object",
	          object_table_get_object_type_name(type));

	if (_free_ids.count == 0 && _next_id == OBJECT_ID_INVALID) {
		// all valid object IDs are acquired
		api_set_last_error(API_ERROR_CODE_NO_FREE_OBJECT_ID);

		log_warn("All object IDs are in use");

		return OBJECT_ID_INVALID;
	}

	object = (Object *)array_append(&_objects[type]);

	if (object == NULL) {
		if (errno == ENOMEM) {
			api_set_last_error(API_ERROR_CODE_NO_FREE_MEMORY);
		} else {
			api_set_last_error(API_ERROR_CODE_UNKNOWN_ERROR);
		}

		log_error("Could not append to %s object array: %s (%d)",
		          object_table_get_object_type_name(type),
		          get_errno_name(errno), errno);

		return OBJECT_ID_INVALID;
	}

	if (_free_ids.count > 0) {
		last = _free_ids.count - 1;
		object->id = *(ObjectID *)array_get(&_free_ids, last);

		array_remove(&_free_ids, last, NULL);
	} else {
		object->id = _next_id++; // will overflow to OBJECT_ID_INVALID
	}

	object->type = type;
	object->data = data;
	object->function = function;

	log_debug("Added %s object (id: %u)",
	          object_table_get_object_type_name(type), object->id);

	return object->id;
}

int object_table_remove_object(ObjectType type, ObjectID id) {
	int i;
	Object *object;
	ObjectID *free_id;

	if (type < OBJECT_TYPE_STRING || type > OBJECT_TYPE_PROGRAM) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object type %d for object ID %u", type, id);

		return -1;
	}

	if (id == OBJECT_ID_INVALID) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object ID %u for object type %d", id, type);

		return -1;
	}

	log_debug("Removing %s object (id: %u)",
	          object_table_get_object_type_name(type), id);

	// find object ID
	for (i = 0; i < _objects[type].count; ++i) {
		object = (Object *)array_get(&_objects[type], i);

		if (object->id != id) {
			continue;
		}

		object_destroy(object);

		array_remove(&_objects[type], i, NULL);

		// adjust next-object-ID or add it to the array of free object IDs
		if (_next_id != OBJECT_ID_INVALID && id == _next_id - 1) {
			--_next_id;
		} else {
			free_id = (ObjectID *)array_append(&_free_ids);

			if (free_id == NULL) {
				api_set_last_error(API_ERROR_CODE_NO_FREE_MEMORY);

				log_error("Could not append to free object ID array: %s (%d)",
				          get_errno_name(errno), errno);

				return -1;
			}

			*free_id = id;
		}

		// adjust iteration index
		if (_iteration_index[type] > 0 && _iteration_index[type] < i) {
			--_iteration_index[type];
		}

		log_debug("Removed %s object (id: %u)",
		          object_table_get_object_type_name(type), id);

		return 0;
	}

	api_set_last_error(API_ERROR_CODE_UNKNOWN_OBJECT_ID);

	log_warn("Could not remove unknown %s object (id: %u)",
	         object_table_get_object_type_name(type), id);

	return -1;
}

void *object_table_get_object_data(ObjectType type, ObjectID id) {
	int i;
	Object *object;

	if (type < OBJECT_TYPE_STRING || type > OBJECT_TYPE_PROGRAM) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object type %d for object ID %u", type, id);

		return NULL;
	}

	if (id == OBJECT_ID_INVALID) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object ID %u for object type %d", id, type);

		return NULL;
	}

	// find object ID
	for (i = 0; i < _objects[type].count; ++i) {
		object = (Object *)array_get(&_objects[type], i);

		if (object->id == id) {
			return object->data;
		}
	}

	api_set_last_error(API_ERROR_CODE_UNKNOWN_OBJECT_ID);

	log_warn("Could not get data for unknown %s object (id: %u)",
	         object_table_get_object_type_name(type), id);

	return NULL;
}

ObjectType object_table_get_object_type(ObjectID id) {
	ObjectType type;
	int i;
	Object *object;

	if (id == OBJECT_ID_INVALID) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object ID");

		return OBJECT_TYPE_INVALID;
	}

	for (type = OBJECT_TYPE_STRING; type <= OBJECT_TYPE_PROGRAM; ++type) {
		for (i = 0; i < _objects[type].count; ++i) {
			object = (Object *)array_get(&_objects[type], i);

			if (object->id == id) {
				return type;
			}
		}
	}

	api_set_last_error(API_ERROR_CODE_UNKNOWN_OBJECT_ID);

	log_warn("Unknown object ID %u", id);

	return OBJECT_TYPE_INVALID;
}

ObjectID object_table_get_next_entry(ObjectType type) {
	Object *object;

	if (type < OBJECT_TYPE_STRING || type > OBJECT_TYPE_PROGRAM) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object type %d", type);

		return OBJECT_ID_INVALID;
	}

	if (_iteration_index[type] < 0) {
		api_set_last_error(API_ERROR_CODE_NO_REWIND);

		log_warn("Trying to get next %s object without rewinding the table first",
		         object_table_get_object_type_name(type));

		return OBJECT_ID_INVALID;
	}

	if (_iteration_index[type] >= _objects[type].count) {
		api_set_last_error(API_ERROR_CODE_NO_MORE_DATA);

		log_debug("Reached end of %s object table",
		          object_table_get_object_type_name(type));

		return OBJECT_ID_INVALID;
	}

	object = (Object *)array_get(&_objects[type], _iteration_index[type]++);

	return object->id;
}

int object_table_rewind(ObjectType type) {
	if (type < OBJECT_TYPE_STRING || type > OBJECT_TYPE_PROGRAM) {
		api_set_last_error(API_ERROR_CODE_INVALID_PARAMETER);

		log_warn("Invalid object type %d", type);

		return -1;
	}

	_iteration_index[type] = 0;

	return 0;
}
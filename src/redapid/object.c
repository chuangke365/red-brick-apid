/*
 * redapid
 * Copyright (C) 2014 Matthias Bolte <matthias@tinkerforge.com>
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

#include <errno.h>

#include <daemonlib/log.h>

#include "object.h"

#include "object_table.h"

#define LOG_CATEGORY LOG_CATEGORY_OBJECT

static void object_add_reference(Object *object, int *reference_count,
                                 const char *reference_count_name) {
	log_debug("Adding an %s %s object (id: %u) reference (count: %d +1)",
	          reference_count_name, object_get_type_name(object->type),
	          object->id, *reference_count);

	++(*reference_count);
}

static void object_remove_reference(Object *object, int *reference_count,
                                    const char *reference_count_name) {
	if (*reference_count == 0) {
		log_warn("Could not remove %s %s object (id: %u) reference, %s reference count is already zero",
		         reference_count_name, object_get_type_name(object->type),
		         object->id, reference_count_name);

		return;
	}

	log_debug("Removing an %s %s object (id: %u) reference (count: %d -1)",
	          reference_count_name, object_get_type_name(object->type),
	          object->id, *reference_count);

	--(*reference_count);

	// destroy object if last reference was removed
	if (object->internal_reference_count == 0 && object->external_reference_count == 0) {
		object_table_remove_object(object); // calls object_destroy
	}
}

const char *object_get_type_name(ObjectType type) {
	switch (type) {
	case OBJECT_TYPE_STRING:
		return "string";

	case OBJECT_TYPE_LIST:
		return "list";

	case OBJECT_TYPE_FILE:
		return "file";

	case OBJECT_TYPE_DIRECTORY:
		return "directory";

	case OBJECT_TYPE_PROCESS:
		return "process";

	case OBJECT_TYPE_PROGRAM:
		return "program";

	default:
		return "<unknown>";
	}
}

bool object_is_type_valid(ObjectType type) {
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

APIE object_create(Object *object, ObjectType type, bool with_internal_reference,
                   ObjectDestroyFunction destroy) {
	object->type = type;
	object->destroy = destroy;
	object->internal_reference_count = with_internal_reference ? 1 : 0;
	object->external_reference_count = 1;
	object->usage_count = 0;

	return object_table_add_object(object);
}

void object_destroy(Object *object) {
	if (object->internal_reference_count != 0 || object->external_reference_count != 0) {
		log_warn("Destroying %s object (id: %u) while there are still references (internal: %d, external: %d) to it",
		         object_get_type_name(object->type), object->id,
		         object->internal_reference_count, object->external_reference_count);
	}

	if (object->usage_count > 0) {
		log_warn("Destroying %s object (id: %u) while it is still in use (usage-count: %d)",
		         object_get_type_name(object->type), object->id, object->usage_count);
	}

	if (object->destroy != NULL) {
		object->destroy(object);
	}
}

void object_add_internal_reference(Object *object) {
	object_add_reference(object, &object->internal_reference_count, "internal");
}

void object_remove_internal_reference(Object *object) {
	object_remove_reference(object, &object->internal_reference_count, "internal");
}

void object_add_external_reference(Object *object) {
	object_add_reference(object, &object->external_reference_count, "external");
}

void object_remove_external_reference(Object *object) {
	object_remove_reference(object, &object->external_reference_count, "external");
}

void object_occupy(Object *object) {
	log_debug("Occupying %s object (id: %u, usage-count: %d +1)",
	          object_get_type_name(object->type), object->id, object->usage_count);

	++object->usage_count;

	object_add_internal_reference(object);
}

void object_vacate(Object *object) {
	if (object->usage_count == 0) {
		log_error("Cannot vacate already unused %s object (id: %u)",
		          object_get_type_name(object->type), object->id);

		return;
	}

	log_debug("Vacating %s object (id: %u, usage-count: %d -1)",
	          object_get_type_name(object->type), object->id, object->usage_count);

	--object->usage_count;

	object_remove_internal_reference(object);
}

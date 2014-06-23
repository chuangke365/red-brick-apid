/*
 * redapid
 * Copyright (C) 2014 Matthias Bolte <matthias@tinkerforge.com>
 *
 * string.h: String object implementation
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

#ifndef REDAPID_STRING_H
#define REDAPID_STRING_H

#include "object_table.h"

#define STRING_SET_CHUNK_BUFFER_LENGTH 58
#define STRING_GET_CHUNK_BUFFER_LENGTH 64

ObjectID string_acquire_external(uint32_t reserve);
ObjectID string_acquire_internal(char *buffer);
int string_acquire_ref(ObjectID id);
int string_release(ObjectID id);

int string_truncate(ObjectID id, uint32_t length);
int32_t string_get_length(ObjectID id);

int string_set_chunk(ObjectID id, uint32_t offset, char *buffer);
void string_get_chunk(ObjectID id, uint32_t offset, char *buffer);

int string_lock(ObjectID id);
int string_unlock(ObjectID id);

const char *string_get_null_terminated_buffer(ObjectID id);

#endif // REDAPID_STRING_H
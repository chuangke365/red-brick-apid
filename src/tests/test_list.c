#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "ip_connection.h"
#include "brick_red.h"

#define HOST "localhost"
#define PORT 4223
#define UID "3hG6BK" // Change to your UID

#include "utils.c"

RED red;

int main() {
	uint8_t ec;
	int rc;

	// Create IP connection
	IPConnection ipcon;
	ipcon_create(&ipcon);

	// Create device object
	red_create(&red, UID, &ipcon);

	// Connect to brickd
	rc = ipcon_connect(&ipcon, HOST, PORT);
	if (rc < 0) {
		printf("ipcon_connect -> rc %d\n", rc);
		return -1;
	}

	uint16_t session_id;
	if (create_session(&red, 30, &session_id) < 0) {
		return -1;
	}

	uint16_t lid;
	rc = red_allocate_list(&red, 20, session_id, &ec, &lid);
	if (rc < 0) {
		printf("red_allocate_list -> rc %d\n", rc);
		return -1;
	}
	if (ec != 0) {
		printf("red_allocate_list -> ec %u\n", ec);
		return -1;
	}
	printf("red_allocate_list -> lid %u\n", lid);

	int i;
	for (i = 0; i < 30; ++i) {
		uint16_t sid;
		if (allocate_string(&red, "A123456789B123456789C123456789D123456789", session_id, &sid) < 0) {
			goto cleanup;
		}

		rc = red_append_to_list(&red, lid, sid, &ec);
		if (rc < 0) {
			printf("red_append_to_list -> rc %d\n", rc);
		}
		if (ec != 0) {
			printf("red_append_to_list -> ec %u\n", ec);
		}

		release_object(&red, sid, session_id, "string");
	}

	uint16_t length;
	rc = red_get_list_length(&red, lid, &ec, &length);
	if (rc < 0) {
		printf("red_get_list_length -> rc %d\n", rc);
	}
	if (ec != 0) {
		printf("red_get_list_length -> ec %u\n", ec);
	}
	printf("red_get_list_length -> length %u\n", length);

	rc = red_remove_from_list(&red, lid, 5, &ec);
	if (rc < 0) {
		printf("red_remove_from_list -> rc %d\n", rc);
	}
	if (ec != 0) {
		printf("red_remove_from_list -> ec %u\n", ec);
	}

	rc = red_get_list_length(&red, lid, &ec, &length);
	if (rc < 0) {
		printf("red_get_list_length -> rc %d\n", rc);
	}
	if (ec != 0) {
		printf("red_get_list_length -> ec %u\n", ec);
	}
	printf("red_get_list_length -> length %u\n", length);

cleanup:
	release_object(&red, lid, session_id, "list");
	expire_session(&red, session_id);

	red_destroy(&red);
	ipcon_destroy(&ipcon);

	return 0;
}

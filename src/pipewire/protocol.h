/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_PROTOCOL_H
#define PIPEWIRE_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/list.h>

struct pw_protocol;

#include <pipewire/core.h>
#include <pipewire/properties.h>
#include <pipewire/utils.h>

#define PW_TYPE_INFO_Protocol		"PipeWire:Protocol"
#define PW_TYPE_INFO_PROTOCOL_BASE	PW_TYPE_INFO_Protocol ":"

struct pw_protocol_client {
	struct spa_list link;		/**< link in protocol client_list */
	struct pw_protocol *protocol;	/**< the owner protocol */

	struct pw_remote *remote;	/**< the associated remote */

	int (*connect) (struct pw_protocol_client *client,
			void (*done_callback) (void *data, int result),
			void *data);
	int (*connect_fd) (struct pw_protocol_client *client, int fd, bool close);
	int (*steal_fd) (struct pw_protocol_client *client);
	void (*disconnect) (struct pw_protocol_client *client);
	void (*destroy) (struct pw_protocol_client *client);
};

#define pw_protocol_client_connect(c,cb,d)	((c)->connect(c,cb,d))
#define pw_protocol_client_connect_fd(c,fd,cl)	((c)->connect_fd(c,fd,cl))
#define pw_protocol_client_steal_fd(c)		((c)->steal_fd(c))
#define pw_protocol_client_disconnect(c)	((c)->disconnect(c))
#define pw_protocol_client_destroy(c)		((c)->destroy(c))

struct pw_protocol_server {
	struct spa_list link;		/**< link in protocol server_list */
	struct pw_protocol *protocol;	/**< the owner protocol */

	struct spa_list client_list;	/**< list of clients of this protocol */

	void (*destroy) (struct pw_protocol_server *listen);
};

#define pw_protocol_server_destroy(l)	((l)->destroy(l))

struct pw_protocol_marshal {
        uint32_t type;			/**< interface type */
	uint32_t version;               /**< version */
	uint32_t n_methods;             /**< number of methods in the interface */
        uint32_t n_events;              /**< number of events in the interface */
	const void *method_marshal;
	const void *method_demarshal;
	const void *event_marshal;
	const void *event_demarshal;
};

struct pw_protocol_implementaton {
#define PW_VERSION_PROTOCOL_IMPLEMENTATION	0
	uint32_t version;

	struct pw_protocol_client * (*new_client) (struct pw_protocol *protocol,
						   struct pw_remote *remote,
						   struct pw_properties *properties);
	struct pw_protocol_server * (*add_server) (struct pw_protocol *protocol,
						   struct pw_core *core,
						   struct pw_properties *properties);
};

struct pw_protocol_events {
#define PW_VERSION_PROTOCOL_EVENTS		0
	uint32_t version;

	void (*destroy) (void *data);
};

#define pw_protocol_new_client(p,...)	(pw_protocol_get_implementation(p)->new_client(p,__VA_ARGS__))
#define pw_protocol_add_server(p,...)	(pw_protocol_get_implementation(p)->add_server(p,__VA_ARGS__))
#define pw_protocol_ext(p,type,method,...)	(((type*)pw_protocol_get_extension(p))->method( __VA_ARGS__))

struct pw_protocol *pw_protocol_new(struct pw_core *core, const char *name, size_t user_data_size);

void pw_protocol_destroy(struct pw_protocol *protocol);

void *pw_protocol_get_user_data(struct pw_protocol *protocol);

const struct pw_protocol_implementaton *
pw_protocol_get_implementation(struct pw_protocol *protocol);

const void *
pw_protocol_get_extension(struct pw_protocol *protocol);


void pw_protocol_add_listener(struct pw_protocol *protocol,
                              struct spa_hook *listener,
                              const struct pw_protocol_events *events,
                              void *data);

/** \class pw_protocol
 *
 * \brief Manages protocols and their implementation
 */
int pw_protocol_add_marshal(struct pw_protocol *protocol,
			    const struct pw_protocol_marshal *marshal);

const struct pw_protocol_marshal *
pw_protocol_get_marshal(struct pw_protocol *protocol, uint32_t type);

struct pw_protocol * pw_core_find_protocol(struct pw_core *core, const char *name);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_PROTOCOL_H */

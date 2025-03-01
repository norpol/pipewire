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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>

#include "spa-device.h"
#include "pipewire/device.h"
#include "pipewire/port.h"
#include "pipewire/log.h"
#include "pipewire/private.h"
#include "pipewire/pipewire.h"

struct impl {
	struct pw_device *this;

	enum pw_spa_device_flags flags;

	void *unload;
        struct spa_handle *handle;
        struct spa_device *device;

	struct spa_hook device_listener;

	void *user_data;
};

static void device_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_device *device = impl->this;

	pw_log_debug("spa-device %p: free", device);

	spa_hook_remove(&impl->device_listener);
	if (impl->handle)
		pw_unload_spa_handle(impl->handle);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.destroy = device_destroy,
};

struct pw_device *
pw_spa_device_new(struct pw_core *core,
		  enum pw_spa_device_flags flags,
		  struct spa_device *device,
		  struct spa_handle *handle,
		  struct pw_properties *properties,
		  size_t user_data_size)
{
	struct pw_device *this;
	struct impl *impl;
	int res;

	this = pw_device_new(core, properties, sizeof(struct impl) + user_data_size);
	if (this == NULL)
		return NULL;

	impl = this->user_data;
	impl->this = this;
	impl->device = device;
	impl->handle = handle;
	impl->flags = flags;

	if (user_data_size > 0)
                impl->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	pw_device_add_listener(this, &impl->device_listener, &device_events, impl);
	pw_device_set_implementation(this, impl->device);

	if (!SPA_FLAG_CHECK(impl->flags, PW_SPA_DEVICE_FLAG_NO_REGISTER)) {
		if ((res = pw_device_register(this, NULL)) < 0)
			goto error_register;
	}
	return this;

error_register:
	pw_device_destroy(this);
	errno = -res;
	return NULL;
}

void *pw_spa_device_get_user_data(struct pw_device *device)
{
	struct impl *impl = device->user_data;
	return impl->user_data;
}

struct pw_device *pw_spa_device_load(struct pw_core *core,
				 const char *factory_name,
				 enum pw_spa_device_flags flags,
				 struct pw_properties *properties,
				 size_t user_data_size)
{
	struct pw_device *this;
	struct spa_handle *handle;
	void *iface;
	int res;

	handle = pw_core_load_spa_handle(core, factory_name,
			properties ? &properties->dict : NULL);
	if (handle == NULL)
		goto error_load;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0)
		goto error_interface;

	this = pw_spa_device_new(core, flags,
			       iface, handle, properties, user_data_size);
	if (this == NULL)
		goto error_device;

	return this;

error_load:
	res = -errno;
	pw_log_error("can't load device handle: %m");
	goto error_exit;
error_interface:
	pw_log_error("can't get device interface %d", res);
	goto error_exit_unload;
error_device:
	res = -errno;
	pw_log_error("can't create device: %m");
	goto error_exit_unload;

error_exit_unload:
	pw_unload_spa_handle(handle);
error_exit:
	errno = -res;
	return NULL;
}

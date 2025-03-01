/* Simple Plugin API
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

#ifndef SPA_DEVICE_H
#define SPA_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/support/plugin.h>
#include <spa/pod/builder.h>
#include <spa/pod/event.h>

/**
 * spa_device:
 *
 * The device interface.
 */
#define SPA_VERSION_DEVICE 0
struct spa_device { struct spa_interface iface; };

struct spa_device_info {
#define SPA_VERSION_DEVICE_INFO 0
	uint32_t version;

#define SPA_DEVICE_CHANGE_MASK_FLAGS		(1u<<0)
#define SPA_DEVICE_CHANGE_MASK_PROPS		(1u<<1)
#define SPA_DEVICE_CHANGE_MASK_PARAMS		(1u<<2)
	uint64_t change_mask;
	uint64_t flags;
	const struct spa_dict *props;
	struct spa_param_info *params;
	uint32_t n_params;
};

#define SPA_DEVICE_INFO_INIT()	(struct spa_device_info){ SPA_VERSION_DEVICE_INFO, }

struct spa_device_object_info {
#define SPA_VERSION_DEVICE_OBJECT_INFO 0
	uint32_t version;

	uint32_t type;
	const char *factory_name;

#define SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS	(1u<<0)
#define SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS	(1u<<1)
	uint64_t change_mask;
	uint64_t flags;
	const struct spa_dict *props;
};

#define SPA_DEVICE_OBJECT_INFO_INIT()	(struct spa_device_object_info){ SPA_VERSION_DEVICE_OBJECT_INFO, }

/** the result of spa_device_enum_params() */
#define SPA_RESULT_TYPE_DEVICE_PARAMS	1
struct spa_result_device_params {
	uint32_t id;
	uint32_t index;
	uint32_t next;
	struct spa_pod *param;
};

#define SPA_DEVICE_EVENT_INFO		0
#define SPA_DEVICE_EVENT_RESULT		1
#define SPA_DEVICE_EVENT_EVENT		2
#define SPA_DEVICE_EVENT_OBJECT_INFO	3
#define SPA_DEVICE_EVENT_NUM		4

/**
 * spa_device_events:
 *
 * Events are always emited from the main thread
 */
struct spa_device_events {
	/** version of the structure */
#define SPA_VERSION_DEVICE_EVENTS	0
	uint32_t version;

	/** notify extra information about the device */
	void (*info) (void *data, const struct spa_device_info *info);

	/** notify a result */
	void (*result) (void *data, int seq, int res, uint32_t type, const void *result);

	/** a device event */
	void (*event) (void *data, const struct spa_event *event);

	/** info changed for an object managed by the device, info is NULL when
	 * the object is removed */
	void (*object_info) (void *data, uint32_t id,
		const struct spa_device_object_info *info);
};

#define SPA_DEVICE_METHOD_ADD_LISTENER	0
#define SPA_DEVICE_METHOD_SYNC		1
#define SPA_DEVICE_METHOD_ENUM_PARAMS	2
#define SPA_DEVICE_METHOD_SET_PARAM	3
#define SPA_DEVICE_METHOD_NUM		4

/**
 * spa_device_methods:
 */
struct spa_device_methods {
	/* the version of the methods. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_DEVICE_METHODS	0
	uint32_t version;

	/**
	 * Set events to receive asynchronous notifications from
	 * the device.
	 *
	 * Setting the events will trigger the info event and an
	 * object_info event for each managed node on the new
	 * listener.
	 *
	 * \param device a #spa_device
	 * \param listener a listener
	 * \param events a #struct spa_device_events
	 * \param data data passed as first argument in functions of \a events
	 * \return 0 on success
	 *	   < 0 errno on error
	 */
	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct spa_device_events *events,
			void *data);
	/**
	 * Perform a sync operation.
	 *
	 * This method will emit the result event with the given sequence
	 * number synchronously or with the returned async return value
	 * asynchronously.
	 *
	 * Because all methods are serialized in the device, this can be used
	 * to wait for completion of all previous method calls.
	 *
	 * \param seq a sequence number
	 * \return 0 on success
	 *         -EINVAL when node is NULL
	 *         an async result
	 */
        int (*sync) (void *object, int seq);

	/**
	 * Enumerate the parameters of a device.
	 *
	 * Parameters are identified with an \a id. Some parameters can have
	 * multiple values, see the documentation of the parameter id.
	 *
	 * Parameters can be filtered by passing a non-NULL \a filter.
	 *
	 * The result callback will be called at most \max times with a
	 * struct spa_result_device_params as the result.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param device a \ref spa_device
	 * \param seq a sequence numeber to pass to the result function
	 * \param id the param id to enumerate
	 * \param index the index of enumeration, pass 0 for the first item.
	 * \param max the maximum number of items to iterate
	 * \param filter and optional filter to use
	 * \return 0 when there are no more parameters to enumerate
	 *         -EINVAL when invalid arguments are given
	 *         -ENOENT the parameter \a id is unknown
	 *         -ENOTSUP when there are no parameters
	 *                 implemented on \a device
	 */
	int (*enum_params) (void *object, int seq,
			    uint32_t id, uint32_t index, uint32_t max,
			    const struct spa_pod *filter);

	/**
	 * Set the configurable parameter in \a device.
	 *
	 * Usually, \a param will be obtained from enum_params and then
	 * modified but it is also possible to set another spa_pod
	 * as long as its keys and types match a supported object.
	 *
	 * Objects with property keys that are not known are ignored.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param device a \ref spa_device
	 * \param id the parameter id to configure
	 * \param flags additional flags
	 * \param param the parameter to configure
	 *
	 * \return 0 on success
	 *         -EINVAL when invalid arguments are given
	 *         -ENOTSUP when there are no parameters implemented on \a device
	 *         -ENOENT the parameter is unknown
	 */
	int (*set_param) (void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
};

#define spa_device_method(o,method,version,...)				\
({									\
	int _res = -ENOTSUP;						\
	struct spa_device *_o = o;					\
	spa_interface_call_res(&_o->iface,				\
			struct spa_device_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define spa_device_add_listener(d,...)	spa_device_method(d, add_listener, 0, __VA_ARGS__)
#define spa_device_sync(d,...)		spa_device_method(d, sync, 0, __VA_ARGS__)
#define spa_device_enum_params(d,...)	spa_device_method(d, enum_params, 0, __VA_ARGS__)
#define spa_device_set_param(d,...)	spa_device_method(d, set_param, 0, __VA_ARGS__)

#define SPA_KEY_DEVICE_ENUM_API		"device.enum.api"	/**< the api used to discover this
								  *  device */
#define SPA_KEY_DEVICE_API		"device.api"		/**< the api used by the device
								  *  Ex. "udev", "alsa", "v4l2". */
#define SPA_KEY_DEVICE_NAME		"device.name"		/**< the name of the device */
#define SPA_KEY_DEVICE_ALIAS		"device.alias"		/**< altenative name of the device */
#define SPA_KEY_DEVICE_NICK		"device.nick"		/**< the device short name */
#define SPA_KEY_DEVICE_DESCRIPTION	"device.description"	/**< a device description */
#define SPA_KEY_DEVICE_ICON		"device.icon"		/**< icon for the device. A base64 blob
								  *  containing PNG image data */
#define SPA_KEY_DEVICE_ICON_NAME	"device.icon-name"	/**< an XDG icon name for the device.
								  *  Ex. "sound-card-speakers-usb" */
#define SPA_KEY_DEVICE_PLUGGED_USEC	"device.plugged.usec"	/**< when the device was plugged */

#define SPA_KEY_DEVICE_BUS_ID		"device.bus-id"		/**< the device bus-id */
#define SPA_KEY_DEVICE_BUS_PATH		"device.bus-path"	/**< bus path to the device in the OS'
								  *  format.
								  *  Ex. "pci-0000:00:14.0-usb-0:3.2:1.0" */
#define SPA_KEY_DEVICE_BUS		"device.bus"		/**< bus of the device if applicable. One of
								   *  "isa", "pci", "usb", "firewire",
								   *  "bluetooth" */
#define SPA_KEY_DEVICE_SUBSYSTEM	"device.subsystem"	/**< device subsystem */
#define SPA_KEY_DEVICE_SYSFS_PATH	"device.sysfs.path"	/**< device sysfs path */

#define SPA_KEY_DEVICE_VENDOR_ID	"device.vendor.id"	/**< vendor ID if applicable */
#define SPA_KEY_DEVICE_VENDOR_NAME	"device.vendor.name"	/**< vendor name if applicable */
#define SPA_KEY_DEVICE_PRODUCT_ID	"device.product.id"	/**< product ID if applicable */
#define SPA_KEY_DEVICE_PRODUCT_NAME	"device.product.name"	/**< product name if applicable */
#define SPA_KEY_DEVICE_SERIAL		"device.serial"		/**< Serial number if applicable */
#define SPA_KEY_DEVICE_CLASS		"device.class"		/**< device class */
#define SPA_KEY_DEVICE_CAPABILITIES	"device.capabilities"	/**< api specific device capabilities */
#define SPA_KEY_DEVICE_FORM_FACTOR	"device.form-factor"	/**< form factor if applicable. One of
								  *  "internal", "speaker", "handset", "tv",
								  *  "webcam", "microphone", "headset",
								  *  "headphone", "hands-free", "car", "hifi",
								  *  "computer", "portable" */


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_DEVICE_H */

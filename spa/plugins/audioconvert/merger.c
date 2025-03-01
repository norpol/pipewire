/* Spa
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

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/utils/names.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>
#include <spa/debug/pod.h>

#include "fmt-ops.h"

#define NAME "merger"

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2

#define MAX_SAMPLES	2048
#define MAX_BUFFERS	64
#define MAX_DATAS	32
#define MAX_PORTS	128

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1<<0)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *buf;
	void *datas[MAX_DATAS];
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[8];
	char position[16];

	bool have_format;
	struct spa_audio_info format;
	uint32_t blocks;
	uint32_t stride;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[8];

	struct spa_hook_list hooks;

	uint32_t port_count;
	uint32_t monitor_count;
	struct port in_ports[MAX_PORTS];
	struct port out_ports[MAX_PORTS + 1];

	struct convert conv;
	uint32_t cpu_flags;
	unsigned int is_passthrough:1;
	unsigned int started:1;
	unsigned int monitor:1;
	unsigned int have_profile:1;

	float empty[MAX_SAMPLES + 15];
};

#define CHECK_IN_PORT(this,d,p)		((d) == SPA_DIRECTION_INPUT && (p) < this->port_count)
#define CHECK_OUT_PORT(this,d,p)	((d) == SPA_DIRECTION_OUTPUT && (p) <= this->monitor_count)
#define CHECK_PORT(this,d,p)		(CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))
#define GET_IN_PORT(this,p)		(&this->in_ports[p])
#define GET_OUT_PORT(this,p)		(&this->out_ports[p])
#define GET_PORT(this,d,p)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define PORT_IS_DSP(d,p) (p != 0 || d != SPA_DIRECTION_OUTPUT)

static void emit_node_info(struct impl *this, bool full)
{
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		struct spa_dict_item items[3];
		uint32_t n_items = 0;

		if (PORT_IS_DSP(port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit float mono audio");
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_AUDIO_CHANNEL, port->position);
			if (port->direction == SPA_DIRECTION_OUTPUT)
				items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_MONITOR, "1");
		}
		port->info.props = &SPA_DICT_INIT(items, n_items);

		spa_node_emit_port_info(&this->hooks, port->direction, port->id, &port->info);
		port->info.change_mask = 0;
	}
}

static int init_port(struct impl *this, enum spa_direction direction, uint32_t port_id,
		uint32_t rate, uint32_t position)
{
	struct port *port = GET_PORT(this, direction, port_id);

	port->direction = direction;
	port->id = port_id;

	snprintf(port->position, 16, "%s", rindex(spa_type_audio_channel[position].name, ':')+1);

	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
		SPA_PORT_FLAG_DYNAMIC_DATA;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 5;

	port->n_buffers = 0;
	port->have_format = false;
	port->format.media_type = SPA_MEDIA_TYPE_audio;
	port->format.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	port->format.info.raw.format = SPA_AUDIO_FORMAT_F32P;
	port->format.info.raw.rate = rate;
	port->format.info.raw.channels = 1;
	port->format.info.raw.position[0] = position;
	spa_list_init(&port->queue);

	spa_log_debug(this->log, NAME " %p: add port %d:%d rate:%d position:%s",
			this, direction, port_id, rate, port->position);
	emit_port_info(this, port, true);

	return 0;
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PortConfig:
		return -ENOTSUP;
	default:
		return 0;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_PortConfig:
	{
		struct spa_audio_info info = { 0, };
		struct port *port;
		struct spa_pod *format;
		enum spa_direction direction;
		enum spa_param_port_config_mode mode;
		bool monitor = false;
		uint32_t i;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamPortConfig, NULL,
				SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(&direction),
				SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(&mode),
				SPA_PARAM_PORT_CONFIG_monitor,		SPA_POD_OPT_Bool(&monitor),
				SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(&format)) < 0)
			return -EINVAL;

		if (!SPA_POD_IS_OBJECT_TYPE(format, SPA_TYPE_OBJECT_Format))
			return -EINVAL;

		if (mode != SPA_PARAM_PORT_CONFIG_MODE_dsp)
			return -ENOTSUP;
		if (direction != SPA_DIRECTION_INPUT)
			return -EINVAL;

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		port = GET_OUT_PORT(this, 0);
		if (port->have_format && memcmp(&port->format, &info, sizeof(info)) == 0)
			return 0;

		spa_log_debug(this->log, NAME " %p: port config %d/%d %d", this,
				info.info.raw.rate, info.info.raw.channels, monitor);

		for (i = 0; i < this->port_count; i++) {
			spa_node_emit_port_info(&this->hooks,
					SPA_DIRECTION_INPUT, i, NULL);
			if (this->monitor)
				spa_node_emit_port_info(&this->hooks,
						SPA_DIRECTION_OUTPUT, i+1, NULL);
		}

		port->have_format = true;
		port->format = info;
		this->monitor = monitor;

		this->have_profile = true;
		this->port_count = info.info.raw.channels;
		this->monitor_count = this->monitor ? this->port_count : 0;
		for (i = 0; i < this->port_count; i++) {
			init_port(this, SPA_DIRECTION_INPUT, i, info.info.raw.rate,
					info.info.raw.position[i]);
			if (this->monitor)
				init_port(this, SPA_DIRECTION_OUTPUT, i+1, info.info.raw.rate,
					info.info.raw.position[i]);
		}
		return 0;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	uint32_t i;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, NAME" %p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, GET_OUT_PORT(this, 0), true);
	for (i = 0; i < this->port_count; i++) {
		emit_port_info(this, GET_IN_PORT(this, i), true);
		if (this->monitor)
			emit_port_info(this, GET_OUT_PORT(this, i+1), true);
	}

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = object;
	struct port *port = GET_PORT(this, direction, port_id);

	switch (index) {
	case 0:
		if (PORT_IS_DSP(direction, port_id) || port->have_format) {
			*param = spa_format_audio_raw_build(builder,
				SPA_PARAM_EnumFormat, &port->format.info.raw);
		}
		else {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(13,
							SPA_AUDIO_FORMAT_F32,
							SPA_AUDIO_FORMAT_F32,
							SPA_AUDIO_FORMAT_F32P,
							SPA_AUDIO_FORMAT_S32,
							SPA_AUDIO_FORMAT_S32P,
							SPA_AUDIO_FORMAT_S24_32,
							SPA_AUDIO_FORMAT_S24_32P,
							SPA_AUDIO_FORMAT_S24,
							SPA_AUDIO_FORMAT_S24P,
							SPA_AUDIO_FORMAT_S16,
							SPA_AUDIO_FORMAT_S16P,
							SPA_AUDIO_FORMAT_U8,
							SPA_AUDIO_FORMAT_U8P),
				SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(
					DEFAULT_RATE, 1, INT32_MAX),
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(
					DEFAULT_CHANNELS, 1, MAX_PORTS));
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_log_debug(this->log, "%p: enum params %d %d %u %u", this, seq, direction, port_id, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(object, direction, port_id, result.index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &port->format.info.raw);
		break;
	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								1024 * port->stride,
								16 * port->stride,
								MAX_SAMPLES * port->stride),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;
	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0) {
		spa_debug_pod(2, NULL, param);
		spa_debug_pod(2, NULL, filter);
		goto next;
	}

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int setup_convert(struct impl *this)
{
	struct port *outport;
	uint32_t src_fmt, dst_fmt;
	int res;

	outport = GET_OUT_PORT(this, 0);

	src_fmt = SPA_AUDIO_FORMAT_F32P;
	dst_fmt = outport->format.info.raw.format;

	spa_log_info(this->log, NAME " %p: %s/%d@%dx%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_fmt),
			1,
			outport->format.info.raw.rate,
			this->port_count,
			spa_debug_type_find_name(spa_type_audio_format, dst_fmt),
			outport->format.info.raw.channels,
			outport->format.info.raw.rate);

	this->conv.src_fmt = src_fmt;
	this->conv.dst_fmt = dst_fmt;
	this->conv.n_channels = outport->format.info.raw.channels;
	this->conv.cpu_flags = this->cpu_flags;

	if ((res = convert_init(&this->conv)) < 0)
		return res;

	spa_log_info(this->log, NAME " %p: got converter features %08x:%08x", this,
			this->cpu_flags, this->conv.cpu_flags);

	this->is_passthrough = src_fmt == dst_fmt;

	return 0;
}

static int calc_width(struct spa_audio_info *info)
{
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8:
		return 1;
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
		return 2;
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
		return 3;
	default:
		return 4;
	}
}

static int port_set_format(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = object;
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, NAME " %p: set format", this);

	if (format == NULL) {
		if (port->have_format) {
			if (PORT_IS_DSP(direction, port_id))
				port->have_format = false;
			else
				port->have_format = this->have_profile;
			clear_buffers(this, port);
		}
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0) {
			spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
			return res;
		}

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
			spa_log_error(this->log, "unexpected types %d/%d",
					info.media_type, info.media_subtype);
			return -EINVAL;
		}

		if ((res = spa_format_audio_raw_parse(format, &info.info.raw)) < 0) {
			spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
			return res;
		}

		if (PORT_IS_DSP(direction, port_id)) {
			if (info.info.raw.rate != port->format.info.raw.rate) {
				spa_log_error(this->log, "unexpected rate %d<->%d",
					info.info.raw.rate, port->format.info.raw.rate);
				return -EINVAL;
			}
			if (info.info.raw.format != SPA_AUDIO_FORMAT_F32P) {
				spa_log_error(this->log, "unexpected format %d<->%d",
					info.info.raw.format, SPA_AUDIO_FORMAT_F32P);
				return -EINVAL;
			}
			if (info.info.raw.channels != 1) {
				spa_log_error(this->log, "unexpected channels %d<->1",
					info.info.raw.channels);
				return -EINVAL;
			}
		}
		else {
			if (info.info.raw.channels != this->port_count) {
				spa_log_error(this->log, "unexpected channels %d<->%d",
					info.info.raw.channels, this->port_count);
				return -EINVAL;
			}
		}

		port->format = info;
		port->stride = calc_width(&info);
		if (SPA_AUDIO_FORMAT_IS_PLANAR(info.info.raw.format)) {
			port->blocks = info.info.raw.channels;
		}
		else {
			port->stride *= info.info.raw.channels;
			port->blocks = 1;
		}
		spa_log_debug(this->log, NAME " %p: %d %d %d", this,
				port_id, port->stride, port->blocks);

		if (!PORT_IS_DSP(direction, port_id))
			if ((res = setup_convert(this)) < 0)
				return res;

		port->have_format = true;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}


static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
}

static void queue_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	spa_log_trace_fp(this->log, NAME " %p: queue buffer %d on port %d %d",
			this, id, port->id, b->flags);
	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_QUEUED))
		return;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
}

static struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace_fp(this->log, NAME " %p: dequeue buffer %d on port %d %u",
			this, b->id, port->id, b->flags);

	return b;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i, j;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_debug(this->log, NAME " %p: use buffers %d on port %d:%d",
			this, n_buffers, direction, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		uint32_t n_datas = buffers[i]->n_datas;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->buf = buffers[i];

		if (n_datas != port->blocks) {
			spa_log_error(this->log, NAME " %p: invalid blocks %d on buffer %d",
					this, n_datas, i);
			return -EINVAL;
		}

		for (j = 0; j < n_datas; j++) {
			if (!((d[j].type == SPA_DATA_MemPtr ||
			       d[j].type == SPA_DATA_MemFd ||
			       d[j].type == SPA_DATA_DmaBuf) && d[j].data != NULL)) {
				spa_log_error(this->log, NAME " %p: invalid memory %d on buffer %d %d %p",
						this, j, i, d[j].type, d[j].data);
				return -EINVAL;
			}
			if (!SPA_IS_ALIGNED(d[j].data, 16)) {
				spa_log_warn(this->log, NAME " %p: memory %d on buffer %d not aligned",
						this, j, i);
			}
			b->datas[j] = d[j].data;
			if (direction == SPA_DIRECTION_OUTPUT &&
			    !SPA_FLAG_CHECK(d[j].flags, SPA_DATA_FLAG_DYNAMIC))
				this->is_passthrough = false;
		}

		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, i);
	}
	port->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, NAME " %p: set io %d on port %d:%d %p",
			this, id, direction, port_id, data);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_OUT_PORT(this, port_id);
	queue_buffer(this, port, buffer_id);

	return 0;
}

static inline int get_in_buffer(struct impl *this, struct port *port, struct buffer **buf)
{
	struct spa_io_buffers *io;

	if ((io = port->io) == NULL) {
		spa_log_trace_fp(this->log, NAME " %p: no io on port %d",
				this, port->id);
		return -EIO;
	}
	if (io->status != SPA_STATUS_HAVE_DATA ||
	    io->buffer_id >= port->n_buffers) {
		spa_log_trace_fp(this->log, NAME " %p: empty port %d %p %d %d %d",
				this, port->id, io, io->status, io->buffer_id,
				port->n_buffers);
		return -EPIPE;
	}

	*buf = &port->buffers[io->buffer_id];
	io->status = SPA_STATUS_NEED_DATA;

	return 0;
}

static inline int get_out_buffer(struct impl *this, struct port *port, struct buffer **buf)
{
	struct spa_io_buffers *io;

	if ((io = port->io) == NULL ||
	    io->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	if (io->buffer_id < port->n_buffers)
		queue_buffer(this, port, io->buffer_id);

	if ((*buf = dequeue_buffer(this, port)) == NULL)
		return -EPIPE;

	io->status = SPA_STATUS_HAVE_DATA;
	io->buffer_id = (*buf)->id;

	return 0;
}

static inline int handle_monitor(struct impl *this, const void *data, int n_samples, struct port *outport)
{
	struct buffer *dbuf;
        struct spa_data *dd;
	int res, size;

	if ((res = get_out_buffer(this, outport, &dbuf)) != 0)
		return res;

	dd = &dbuf->buf->datas[0];
	size = SPA_MIN(dd->maxsize, n_samples * outport->stride);
	dd->chunk->offset = 0;
	dd->chunk->size = size;

	spa_log_trace(this->log, "%p: io %p %08x", this, outport->io, dd->flags);

	if (SPA_FLAG_CHECK(dd->flags, SPA_DATA_FLAG_DYNAMIC))
		dd->data = (void*)data;
	else
		memcpy(dd->data, data, size);

	return res;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *outport;
	struct spa_io_buffers *outio;
	uint32_t i, maxsize, n_samples;
	struct spa_data *sd, *dd;
	struct buffer *sbuf, *dbuf;
	uint32_t n_src_datas, n_dst_datas;
	const void **src_datas;
	void **dst_datas;
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	outport = GET_OUT_PORT(this, 0);
	outio = outport->io;
	spa_return_val_if_fail(outio != NULL, -EIO);
	spa_return_val_if_fail(this->conv.process != NULL, -EIO);

	spa_log_trace_fp(this->log, NAME " %p: status %p %d %d", this,
			outio, outio->status, outio->buffer_id);

	if ((res = get_out_buffer(this, outport, &dbuf)) != 0)
		return res;

	dd = &dbuf->buf->datas[0];

	maxsize = dd->maxsize;
	n_samples = maxsize / outport->stride;

	src_datas = alloca(sizeof(void*) * this->port_count);

	n_dst_datas = dbuf->buf->n_datas;
	dst_datas = alloca(sizeof(void*) * n_dst_datas);

	/* produce more output if possible */
	n_src_datas = 0;
	for (i = 0; i < this->port_count; i++) {
		struct port *inport = GET_IN_PORT(this, i);

		if (get_in_buffer(this, inport, &sbuf) < 0) {
			src_datas[n_src_datas++] = SPA_PTR_ALIGN(this->empty, 16, void);
			continue;
		}

		sd = &sbuf->buf->datas[0];

		src_datas[n_src_datas++] = SPA_MEMBER(sd->data, sd->chunk->offset, void);

		n_samples = SPA_MIN(n_samples, sd->chunk->size / inport->stride);

		spa_log_trace_fp(this->log, NAME " %p: %d %d %d %p", this,
				sd->chunk->size, maxsize, n_samples, src_datas[i]);

		SPA_FLAG_SET(res, SPA_STATUS_NEED_DATA);
	}

	for (i = 0; i < this->monitor_count; i++)
		handle_monitor(this, src_datas[i], n_samples, GET_OUT_PORT(this, i + 1));

	for (i = 0; i < n_dst_datas; i++) {
		dst_datas[i] = this->is_passthrough ? (void*)src_datas[i] : dbuf->datas[i];
		dbuf->buf->datas[i].data = dst_datas[i];
		dbuf->buf->datas[i].chunk->offset = 0;
		dbuf->buf->datas[i].chunk->size = n_samples * outport->stride;
		spa_log_trace_fp(this->log, NAME " %p %p %d", this, dst_datas[i],
				n_samples * outport->stride);
	}
	if (!this->is_passthrough)
		convert_process(&this->conv, dst_datas, src_datas, n_samples);

	return res | SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		switch (support[i].type) {
		case SPA_TYPE_INTERFACE_Log:
			this->log = support[i].data;
			break;
		case SPA_TYPE_INTERFACE_CPU:
			this->cpu = support[i].data;
			break;
		}
	}

	if (this->cpu)
		this->cpu_flags = spa_cpu_get_flags(this->cpu);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = MAX_PORTS+1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_WRITE);
	this->info.params = this->params;
	this->info.n_params = 1;

	port = GET_OUT_PORT(this, 0);
	port->direction = SPA_DIRECTION_OUTPUT;
	port->id = 0;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_DYNAMIC_DATA;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 5;
	spa_list_init(&port->queue);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_merger_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AUDIO_PROCESS_INTERLEAVE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

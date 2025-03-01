/* Spa SCO Sink
 *
 * Copyright © 2019 Collabora Ltd.
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

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/monitor/device.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include "defs.h"

struct props {
	uint32_t min_latency;
	uint32_t max_latency;
};

#define FILL_FRAMES 2
#define MAX_BUFFERS 32

struct buffer {
	uint32_t id;
	unsigned int outstanding:1;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct port {
	struct spa_audio_info current_format;
	int frame_size;
	unsigned int have_format:1;

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_param_info params[8];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list free;
	struct spa_list ready;

	unsigned int need_data:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	/* Support */
	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	/* Hooks and callbacks */
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	/* Info */
	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[8];
	struct props props;

	/* Transport */
	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;
	int sock_fd;

	/* Port */
	struct port port;

	/* Flags */
	unsigned int started:1;
	unsigned int slaved:1;

	/* Sources */
	struct spa_source source;
	struct spa_source flush_source;

	/* Timer */
	int timerfd;
	struct timespec now;
	struct spa_io_clock *clock;
	struct spa_io_position *position;
	int threshold;

	/* Times */
	uint64_t start_time;

	/* Counts */
	uint64_t sample_count;
};

#define NAME "sco-sink"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_INPUT && (p) == 0)

static const uint32_t default_min_latency = 128;
static const uint32_t default_max_latency = 1024;

static void reset_props(struct props *props)
{
	props->min_latency = default_min_latency;
	props->max_latency = default_max_latency;
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
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_minLatency),
				SPA_PROP_INFO_name, SPA_POD_String("The minimum latency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->min_latency, 1, INT32_MAX));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_maxLatency),
				SPA_PROP_INFO_name, SPA_POD_String("The maximum latency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->max_latency, 1, INT32_MAX));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_minLatency, SPA_POD_Int(p->min_latency),
				SPA_PROP_maxLatency, SPA_POD_Int(p->max_latency));
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static void set_timeout(struct impl *this, time_t sec, long nsec)
{
	struct itimerspec ts;

	ts.it_value.tv_sec = sec;
	ts.it_value.tv_nsec = nsec;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;

	spa_system_timerfd_settime(this->data_system, this->timerfd, 0, &ts, NULL);
	this->source.mask = SPA_IO_IN;
	spa_loop_update_source(this->data_loop, &this->source);
}

static void reset_timeout(struct impl *this)
{
	set_timeout(this, 0, this->slaved ? 0 : 1);
}


static void set_next_timeout(struct impl *this, uint64_t now_time)
{
	struct port *port = &this->port;

	/* Set the next timeout if not slaved, otherwise reset values */
	if (!this->slaved) {
		/* Get the elapsed time */
		uint64_t elapsed_time = 0;
		if (now_time > this->start_time)
			elapsed_time = now_time - this->start_time;

		/* Get the elapsed samples */
		const uint64_t elapsed_samples = elapsed_time * port->current_format.info.raw.rate / SPA_NSEC_PER_SEC;

		/* Get the queued samples (processed - elapsed) */
		const uint64_t queued_samples = (this->sample_count - elapsed_samples);

		/* Get the queued time */
		const uint64_t queued_time = (queued_samples * SPA_NSEC_PER_SEC) / port->current_format.info.raw.rate;

		/* Set the next timeout */
		set_timeout (this, queued_time / SPA_NSEC_PER_SEC, queued_time % SPA_NSEC_PER_SEC);

	} else {
		this->start_time = now_time;
		this->sample_count = 0;
	}
}

static int do_reslave(struct spa_loop *loop,
			bool async,
			uint32_t seq,
			const void *data,
			size_t size,
			void *user_data)
{
	struct impl *this = user_data;
	reset_timeout(this);
	return 0;
}

static inline bool is_slaved(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	bool slaved;

	spa_return_val_if_fail(object != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		this->clock = data;
		break;
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOENT;
	}

	slaved = is_slaved(this);
	if (this->started && slaved != this->slaved) {
		spa_log_debug(this->log, "sco-sink %p: reslave %d->%d", this, this->slaved, slaved);
		this->slaved = slaved;
		spa_loop_invoke(this->data_loop, do_reslave, 0, NULL, 0, true, this);
	}
	return 0;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_minLatency, SPA_POD_OPT_Int(&p->min_latency),
			SPA_PROP_maxLatency, SPA_POD_OPT_Int(&p->max_latency));
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static bool write_data(struct impl *this, const uint8_t *data, uint32_t size, uint32_t *total_written)
{
	uint32_t local_total_written = 0;
	const uint32_t mtu_size = this->transport->write_mtu;

	while (local_total_written <= (size - mtu_size)) {
		const int bytes_written = write(this->sock_fd, data, mtu_size);
		if (bytes_written < 0) {
			spa_log_warn(this->log, "error writting data: %s", strerror(errno));
			return false;
		}

		data += bytes_written;
		local_total_written += bytes_written;
	}

	/* TODO: For now we assume the size is always a mutliple of mtu_size */
	if (local_total_written != size)
		spa_log_warn(this->log, "dropping some audio as buffer size is not multiple of mtu");

	if (total_written)
		*total_written = local_total_written;
	return true;
}

static int render_buffers(struct impl *this, uint64_t now_time)
{
	struct port *port = &this->port;

	/* Render the buffer */
	while (!spa_list_is_empty(&port->ready)) {
		uint8_t *src;
		struct buffer *b;
		struct spa_data *d;
		uint32_t offset, size;
		uint32_t total_written = 0;

		/* Get the buffer and datas */
		b = spa_list_first(&port->ready, struct buffer, link);
		d = b->buf->datas;

		/* Get the data, offset and size */
		src = d[0].data;
		offset = d[0].chunk->offset;
		size = d[0].chunk->size;

		/* Write data */
		if (!write_data(this, src + offset, size, &total_written)) {
			port->need_data = true;
			spa_list_remove(&b->link);
			b->outstanding = true;
			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
			break;
		}

		/* Update the sample count */
		this->sample_count += total_written / port->frame_size;

		/* Remove the buffer and mark it as reusable */
		spa_list_remove(&b->link);
		b->outstanding = true;
		spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
	}

	/* Set next timeout */
	set_next_timeout(this, now_time);

	return 0;
}

static void fill_socket (struct impl *this)
{
  struct port *port = &this->port;
  static const uint8_t zero_buffer[1024 * 4] = { 0, };
  uint32_t fill_size = this->transport->write_mtu;
  uint32_t fills = 0;
  uint32_t total_written = 0;

  /* Fill the socked */
  while (fills < FILL_FRAMES) {
          uint32_t written = 0;

          /* Write the data */
          if (!write_data(this, zero_buffer, fill_size, &written))
                  break;

          total_written += written;
          fills++;
  }

  /* Update the sample count */
  this->sample_count += total_written / port->frame_size;
}

static void sco_on_flush(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t now_time;

	spa_log_trace(this->log, NAME" %p: flushing", this);

	if ((source->rmask & SPA_IO_OUT) == 0) {
		spa_log_warn(this->log, "error %d", source->rmask);
		if (this->flush_source.loop)
			spa_loop_remove_source(this->data_loop, &this->flush_source);
		this->source.mask = 0;
		spa_loop_update_source(this->data_loop, &this->source);
		return;
	}

	/* Get the current time */
	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &this->now);
	now_time = SPA_TIMESPEC_TO_NSEC(&this->now);

	/* Render buffers */
	render_buffers(this, now_time);
}

static void sco_on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	struct port *port = &this->port;
	uint64_t exp, now_time;
	struct spa_io_buffers *io = port->io;

	/* Read the timerfd */
	if (this->started && spa_system_timerfd_read(this->data_system, this->timerfd, &exp) < 0)
		spa_log_warn(this->log, "error reading timerfd: %s", strerror(errno));

	/* Get the current time */
	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &this->now);
	now_time = SPA_TIMESPEC_TO_NSEC(&this->now);

	/* If this is the first timeout, set the start time and fill the socked */
	if (this->start_time == 0) {
		fill_socket (this);
		this->start_time = now_time;
	}

	/* Notify we need a new buffer if we have processed all of them */
	if (spa_list_is_empty(&port->ready) || port->need_data) {
		io->status = SPA_STATUS_NEED_DATA;
		spa_node_call_ready(&this->callbacks, SPA_STATUS_NEED_DATA);
	}

	/* Render the buffers */
	render_buffers(this, now_time);
}

static int do_start(struct impl *this)
{
	int val;
	bool do_accept;

	/* Dont do anything if the node has already started */
	if (this->started)
		return 0;

	/* Make sure the transport is valid */
	spa_return_val_if_fail (this->transport != NULL, -EIO);

	/* Set the slaved flag */
	this->slaved = is_slaved(this);

	/* Do accept if Gateway; otherwise do connect for Head Unit */
	do_accept = this->transport->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY;

	/* acquire the socked fd (false -> connect | true -> accept) */
	this->sock_fd = spa_bt_transport_acquire(this->transport, do_accept);
	if (this->sock_fd < 0)
		return -1;

	/* Set the write MTU */
	val = FILL_FRAMES * this->transport->write_mtu;
	if (setsockopt(this->sock_fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "sco-sink %p: SO_SNDBUF %m", this);

	/* Set the read MTU */
	val = FILL_FRAMES * this->transport->read_mtu;
	if (setsockopt(this->sock_fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "sco-sink %p: SO_RCVBUF %m", this);

	/* Set the priority */
	val = 6;
	if (setsockopt(this->sock_fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "SO_PRIORITY failed: %m");

	/* Add the timeout callback */
	this->source.data = this;
	this->source.fd = this->timerfd;
	this->source.func = sco_on_timeout;
	this->source.mask = SPA_IO_IN;
	this->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->source);

	/* Add the flush callback */
	this->flush_source.data = this;
	this->flush_source.fd = this->sock_fd;
	this->flush_source.func = sco_on_flush;
	this->flush_source.mask = 0;
	this->flush_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->flush_source);

	/* Reset timeout to start processing */
	reset_timeout(this);

	/* Set the started flag */
	this->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;

	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);
	reset_timeout (this);
	if (this->flush_source.loop)
		spa_loop_remove_source(this->data_loop, &this->flush_source);

	return 0;
}

static int do_stop(struct impl *this)
{
	int res = 0;

	if (!this->started)
		return 0;

	spa_log_trace(this->log, "sco-sink %p: stop", this);

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	this->started = false;

	if (this->transport) {
		/* Release the transport */
		res = spa_bt_transport_release(this->transport);

		/* Shutdown and close the socket */
		shutdown(this->sock_fd, SHUT_RDWR);
		close(this->sock_fd);
		this->sock_fd = -1;
	}

	return res;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	port = &this->port;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;
		if ((res = do_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
		if ((res = do_stop(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct spa_dict_item node_info_items[] = {
	{ SPA_KEY_DEVICE_API, "bluez5" },
	{ SPA_KEY_MEDIA_CLASS, "Audio/Sink" },
	{ SPA_KEY_NODE_DRIVER, "true" },
};

static void emit_node_info(struct impl *this, bool full)
{
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
				SPA_DIRECTION_INPUT, 0, &port->info);
		port->info.change_mask = 0;
	}
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->port, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_node_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
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

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port;

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (result.index > 0)
			return 0;

		/* set the info structure */
		struct spa_audio_info_raw info = { 0, };
		info.format = SPA_AUDIO_FORMAT_S16;
		info.channels = 1;
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;

		/* TODO: For now we only handle HSP profiles which has always CVSD format,
		 * but we eventually need to support HFP that can have both CVSD and MSBC formats */

		 /* CVSD format has a rate of 8kHz
		  * MSBC format has a rate of 16kHz */
		info.rate = 8000;

		/* build the param */
		param = spa_format_audio_raw_build(&b, id, &info);

		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &port->current_format.info.raw);
		break;

	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							this->props.min_latency * port->frame_size,
							this->props.min_latency * port->frame_size,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->frame_size),
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

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	do_stop(this);
	if (port->n_buffers > 0) {
		spa_list_init(&port->ready);
		port->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct impl *this, struct port *port,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	int err;

	if (format == NULL) {
		spa_log_info(this->log, "clear format");
		clear_buffers(this, port);
		port->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		port->frame_size = info.info.raw.channels * 2;
		port->current_format = info;
		port->have_format = true;
		this->threshold = this->props.min_latency;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
		port->info.flags = SPA_PORT_FLAG_LIVE;
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
		port->info.rate = SPA_FRACTION(1, port->current_format.info.raw.rate);
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
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);
	port = &this->port;

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(this, port, flags, param);
		break;
	default:
		res = -ENOENT;
		break;
	}
	return res;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port;

	spa_log_info(this->log, "use buffers %d", n_buffers);

	if (!port->have_format)
		return -EIO;

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		uint32_t type;

		b->buf = buffers[i];
		b->id = i;
		b->outstanding = true;

		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		type = buffers[i]->datas[0].type;
		if ((type == SPA_DATA_MemFd ||
		     type == SPA_DATA_DmaBuf ||
		     type == SPA_DATA_MemPtr) && buffers[i]->datas[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return -EINVAL;
		}
		this->threshold = buffers[i]->datas[0].maxsize / port->frame_size;
	}
	port->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);
	port = &this->port;

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
	return -ENOTSUP;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *port;
	struct spa_io_buffers *io;
	uint64_t now_time;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	port = &this->port;
	io = port->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &this->now);
	now_time = SPA_TIMESPEC_TO_NSEC(&this->now);

	if (!spa_list_is_empty(&port->ready))
		render_buffers(this, now_time);

	if (io->status == SPA_STATUS_HAVE_DATA && io->buffer_id < port->n_buffers) {
		struct buffer *b = &port->buffers[io->buffer_id];

		if (!b->outstanding) {
			spa_log_warn(this->log, NAME " %p: buffer %u in use", this, io->buffer_id);
			io->status = -EINVAL;
			return -EINVAL;
		}

		spa_log_trace(this->log, NAME " %p: queue buffer %u", this, io->buffer_id);

		spa_list_append(&port->ready, &b->link);
		b->outstanding = false;
		port->need_data = false;

		this->threshold = SPA_MIN(b->buf->datas[0].chunk->size / port->frame_size,
				this->props.max_latency);

		render_buffers(this, now_time);

		io->status = SPA_STATUS_OK;
	}

	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
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

static void transport_destroy(void *data)
{
	struct impl *this = data;
	spa_log_debug(this->log, "transport %p destroy", this->transport);
	this->transport = NULL;
}

static const struct spa_bt_transport_events transport_events = {
	SPA_VERSION_BT_TRANSPORT_EVENTS,
        .destroy = transport_destroy,
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
	struct impl *this = (struct impl *) handle;
	spa_system_close(this->data_system, this->timerfd);
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
		case SPA_TYPE_INTERFACE_DataLoop:
			this->data_loop = support[i].data;
			break;
		case SPA_TYPE_INTERFACE_DataSystem:
			this->data_system = support[i].data;
			break;
		}
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data system is needed");
		return -EINVAL;
	}

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS |
			SPA_NODE_CHANGE_MASK_PROPS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.max_output_ports = 0;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = 2;

	port = &this->port;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = 0;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 5;
	spa_list_init(&port->ready);

	for (i = 0; info && i < info->n_items; i++) {
		if (strcmp(info->items[i].key, SPA_KEY_API_BLUEZ5_TRANSPORT) == 0)
			sscanf(info->items[i].value, "pointer:%p", &this->transport);
	}
	if (this->transport == NULL) {
		spa_log_error(this->log, "a transport is needed");
		return -EINVAL;
	}
	spa_bt_transport_add_listener(this->transport,
			&this->transport_listener, &transport_events, this);
	this->sock_fd = -1;

	this->timerfd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info, uint32_t *index)
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

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Collabora Ltd. <contact@collabora.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Play bluetooth audio with hsp/hfp" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_TRANSPORT"=<transport>" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

struct spa_handle_factory spa_sco_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_SCO_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

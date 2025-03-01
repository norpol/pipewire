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

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/names.h>

#define M_PI_M2 ( M_PI + M_PI )

static SPA_LOG_IMPL(default_log);

#define spa_debug(f,...) spa_log_trace(&default_log.log, f, __VA_ARGS__)

#include <spa/graph/graph.h>

#include <spa/debug/pod.h>

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[1];
	struct spa_chunk chunks[1];
};

struct data {
	struct spa_log *log;
	struct spa_loop data_loop;

	struct spa_support support[4];
	uint32_t n_support;

	struct spa_graph graph;
	struct spa_graph_state graph_state;
	struct spa_graph_node source_node;
	struct spa_graph_state source_state;
	struct spa_graph_port source_out;
	struct spa_graph_port sink_in;
	struct spa_graph_node sink_node;
	struct spa_graph_state sink_state;

	struct spa_node *sink;

	struct spa_node *source;
	struct spa_io_buffers source_sink_io[1];
	struct spa_buffer *source_buffers[1];
	struct buffer source_buffer[1];

	uint8_t ctrl[1024];
	double freq_accum;
	double volume_accum;

	bool running;
	pthread_t thread;

	struct spa_source sources[16];
	unsigned int n_sources;

	bool rebuild_fds;
	struct pollfd fds[16];
	unsigned int n_fds;
};

#define MIN_LATENCY     1024

#define BUFFER_SIZE    MIN_LATENCY

static void
init_buffer(struct data *data, struct spa_buffer **bufs, struct buffer *ba, int n_buffers,
	    size_t size)
{
	int i;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &ba[i];
		bufs[i] = &b->buffer;

		b->buffer.metas = b->metas;
		b->buffer.n_metas = 1;
		b->buffer.datas = b->datas;
		b->buffer.n_datas = 1;

		b->header.flags = 0;
		b->header.seq = 0;
		b->header.pts = 0;
		b->header.dts_offset = 0;
		b->metas[0].type = SPA_META_Header;
		b->metas[0].data = &b->header;
		b->metas[0].size = sizeof(b->header);

		b->datas[0].type = SPA_DATA_MemPtr;
		b->datas[0].flags = 0;
		b->datas[0].fd = -1;
		b->datas[0].mapoffset = 0;
		b->datas[0].maxsize = size;
		b->datas[0].data = malloc(size);
		b->datas[0].chunk = &b->chunks[0];
		b->datas[0].chunk->offset = 0;
		b->datas[0].chunk->size = 0;
		b->datas[0].chunk->stride = 0;
	}
}

static int make_node(struct data *data, struct spa_node **node, const char *lib, const char *name)
{
	struct spa_handle *handle;
	int res;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;

	if ((hnd = dlopen(lib, RTLD_NOW)) == NULL) {
		printf("can't load %s: %s\n", lib, dlerror());
		return -errno;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		return -errno;
	}

	for (i = 0;;) {
		const struct spa_handle_factory *factory;
		void *iface;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (strcmp(factory->name, name))
			continue;

		handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
		if ((res =
		     spa_handle_factory_init(factory, handle, NULL, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node, &iface)) < 0) {
			printf("can't get interface %d\n", res);
			return res;
		}
		*node = iface;
		return 0;
	}
	return -EBADF;
}

static void update_props(struct data *data)
{
	struct spa_pod_builder b;
	struct spa_pod *pod;
	struct spa_pod_frame f[2];

	spa_pod_builder_init(&b, data->ctrl, sizeof(data->ctrl));

#if 0
	spa_pod_builder_push_sequence(&b, &f[0], 0);
	spa_pod_builder_control(&b, 0, SPA_CONTROL_Properties);
	spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, 0);
	spa_pod_builder_prop(&b, SPA_PROP_frequency, 0);
	spa_pod_builder_float(&b, ((sin(data->freq_accum) + 1.0) * 200.0) + 440.0);
	spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
	spa_pod_builder_float(&b, (sin(data->volume_accum) / 2.0) + 0.5);
	spa_pod_builder_pop(&b, &f[1]);
	pod = spa_pod_builder_pop(&b, &f[0]);
#else
	spa_pod_builder_push_sequence(&b, &f[0], 0);
	spa_pod_builder_control(&b, 0, SPA_CONTROL_Properties);
	spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Props, 0,
		SPA_PROP_frequency, SPA_POD_Float(((sin(data->freq_accum) + 1.0) * 200.0) + 440.0),
		SPA_PROP_volume,    SPA_POD_Float((sin(data->volume_accum) / 2.0) + 0.5));
	pod = spa_pod_builder_pop(&b, &f[0]);
#endif

	spa_debug_pod(0, NULL, pod);

	data->freq_accum += M_PI_M2 / 880.0;
	if (data->freq_accum >= M_PI_M2)
		data->freq_accum -= M_PI_M2;

	data->volume_accum += M_PI_M2 / 2000.0;
	if (data->volume_accum >= M_PI_M2)
		data->volume_accum -= M_PI_M2;
}

static int on_sink_ready(void *_data, int status)
{
	struct data *data = _data;

	update_props(data);

	spa_graph_node_process(&data->source_node);
	spa_graph_node_process(&data->sink_node);
	return 0;
}

static int
on_sink_reuse_buffer(void *_data, uint32_t port_id, uint32_t buffer_id)
{
	struct data *data = _data;

	data->source_sink_io[0].buffer_id = buffer_id;
	return 0;
}

static const struct spa_node_callbacks sink_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = on_sink_ready,
	.reuse_buffer = on_sink_reuse_buffer
};

static int do_add_source(void *object, struct spa_source *source)
{
	struct data *data = object;

	data->sources[data->n_sources] = *source;
	data->n_sources++;
	data->rebuild_fds = true;

	return 0;
}

static int
do_invoke(void *object,
	  spa_invoke_func_t func, uint32_t seq, const void *data, size_t size, bool block, void *user_data)
{
	return func(object, false, seq, data, size, user_data);
}

static const struct spa_loop_methods impl_loop = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = do_add_source,
	.invoke = do_invoke,
};

static int make_nodes(struct data *data, const char *device)
{
	int res;
	struct spa_pod *props;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[512];
	//uint32_t idx;

	if ((res = make_node(data, &data->sink,
			     "build/spa/plugins/alsa/libspa-alsa.so",
			     SPA_NAME_API_ALSA_PCM_SINK)) < 0) {
		printf("can't create alsa-sink: %d\n", res);
		return res;
	}
	spa_node_set_callbacks(data->sink, &sink_callbacks, data);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	props = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Props, 0,
		SPA_PROP_device,     SPA_POD_String(device ? device : "hw:0"),
		SPA_PROP_minLatency, SPA_POD_Int(MIN_LATENCY));

	spa_debug_pod(0, NULL, props);

	if ((res = spa_node_set_param(data->sink, SPA_PARAM_Props, 0, props)) < 0)
		printf("got set_props error %d\n", res);

	if ((res = make_node(data, &data->source,
			     "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
			     "audiotestsrc")) < 0) {
		printf("can't create audiotestsrc: %d\n", res);
		return res;
	}

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	props = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Props, 0,
		SPA_PROP_frequency, SPA_POD_Float(600.0),
		SPA_PROP_volume,    SPA_POD_Float(0.5),
		SPA_PROP_live,      SPA_POD_Bool(false));

	if ((res = spa_node_set_param(data->source, SPA_PARAM_Props, 0, props)) < 0)
		printf("got set_props error %d\n", res);

	if ((res = spa_node_port_set_io(data->source,
				     SPA_DIRECTION_OUTPUT, 0,
				     SPA_IO_Control,
				     &data->ctrl, sizeof(data->ctrl))) < 0) {
		printf("can't set_io freq: %d\n", res);
		return res;
	}

	data->source_sink_io[0] = SPA_IO_BUFFERS_INIT;

	spa_node_port_set_io(data->source,
			     SPA_DIRECTION_OUTPUT, 0,
			     SPA_IO_Buffers,
			     &data->source_sink_io[0], sizeof(data->source_sink_io[0]));
	spa_node_port_set_io(data->sink,
			     SPA_DIRECTION_INPUT, 0,
			     SPA_IO_Buffers,
			     &data->source_sink_io[0], sizeof(data->source_sink_io[0]));

	spa_graph_node_init(&data->source_node, &data->source_state);
	spa_graph_node_set_callbacks(&data->source_node, &spa_graph_node_impl_default, data->source);
	spa_graph_node_add(&data->graph, &data->source_node);
	spa_graph_port_init(&data->source_out, SPA_DIRECTION_OUTPUT, 0, 0);
	spa_graph_port_add(&data->source_node, &data->source_out);

	spa_graph_node_init(&data->sink_node, &data->sink_state);
	spa_graph_node_set_callbacks(&data->sink_node, &spa_graph_node_impl_default, data->sink);
	spa_graph_node_add(&data->graph, &data->sink_node);
	spa_graph_port_init(&data->sink_in, SPA_DIRECTION_INPUT, 0, 0);
	spa_graph_port_add(&data->sink_node, &data->sink_in);

	spa_graph_port_link(&data->source_out, &data->sink_in);

	return res;
}

static int negotiate_formats(struct data *data)
{
	int res;
	struct spa_pod *format, *filter;
	uint32_t state = 0;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	filter = spa_format_audio_raw_build(&b, 0,
			&SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_S16,
				.rate = 44100,
				.channels = 2 ));

	spa_debug_pod(0, NULL, filter);

	spa_log_debug(&default_log.log, "enum_params");
	if ((res = spa_node_port_enum_params_sync(data->sink,
					     SPA_DIRECTION_INPUT, 0,
					     SPA_PARAM_EnumFormat, &state,
					     filter, &format, &b)) != 1)
		return -EBADF;

	spa_log_debug(&default_log.log, "sink set_param");
	spa_debug_pod(0, NULL, format);
	spa_pod_fixate(format);

	if ((res = spa_node_port_set_param(data->sink,
					   SPA_DIRECTION_INPUT, 0,
					   SPA_PARAM_Format, 0, format)) < 0)
		return res;

	if ((res = spa_node_port_set_param(data->source,
					   SPA_DIRECTION_OUTPUT, 0,
					   SPA_PARAM_Format, 0, format)) < 0)
		return res;

	init_buffer(data, data->source_buffers, data->source_buffer, 1, BUFFER_SIZE);
	if ((res =
	     spa_node_port_use_buffers(data->sink,
		     SPA_DIRECTION_INPUT, 0, 0,
		     data->source_buffers, 1)) < 0)
		return res;
	if ((res =
	     spa_node_port_use_buffers(data->source,
		     SPA_DIRECTION_OUTPUT, 0, 0,
		     data->source_buffers, 1)) < 0)
		return res;

	return 0;
}

static void *loop(void *user_data)
{
	struct data *data = user_data;

	printf("enter thread %d\n", data->n_sources);
	while (data->running) {
		int r;
		unsigned int i;

		/* rebuild */
		if (data->rebuild_fds) {
			for (i = 0; i < data->n_sources; i++) {
				struct spa_source *p = &data->sources[i];
				data->fds[i].fd = p->fd;
				data->fds[i].events = p->mask;
			}
			data->n_fds = data->n_sources;
			data->rebuild_fds = false;
		}

		r = poll(data->fds, data->n_fds, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			fprintf(stderr, "select timeout");
			break;
		}

		/* after */
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			p->rmask = data->fds[i].revents;
		}
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			if (p->rmask)
				p->func(p);
		}
	}
	printf("leave thread\n");

	return NULL;
}

static void run_async_sink(struct data *data)
{
	int res;
	int err;

	{
		struct spa_command cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
		if ((res = spa_node_send_command(data->source, &cmd)) < 0)
			printf("got source error %d\n", res);
		if ((res = spa_node_send_command(data->sink, &cmd)) < 0)
			printf("got sink error %d\n", res);
	}

	data->running = true;
	if ((err = pthread_create(&data->thread, NULL, loop, data)) != 0) {
		printf("can't create thread: %d %s", err, strerror(err));
		data->running = false;
	}

	printf("sleeping for 1000 seconds\n");
	sleep(1000);

	if (data->running) {
		data->running = false;
		pthread_join(data->thread, NULL);
	}

	{
		struct spa_command cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
		if ((res = spa_node_send_command(data->sink, &cmd)) < 0)
			printf("got error %d\n", res);
		if ((res = spa_node_send_command(data->source, &cmd)) < 0)
			printf("got source error %d\n", res);
	}
}

int main(int argc, char *argv[])
{
	struct data data = { NULL };
	int res;
	const char *str;

	spa_graph_init(&data.graph, &data.graph_state);

	data.log = &default_log.log;
	data.data_loop.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Loop, SPA_VERSION_LOOP, &impl_loop, &data);

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	data.support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, data.log);
	data.support[1] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Loop, &data.data_loop);
	data.support[2] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, &data.data_loop);
	data.n_support = 3;

	if ((res = make_nodes(&data, argc > 1 ? argv[1] : NULL)) < 0) {
		printf("can't make nodes: %d (%s)\n", res, spa_strerror(res));
		return -1;
	}
	if ((res = negotiate_formats(&data)) < 0) {
		printf("can't negotiate nodes: %d (%s)\n", res, spa_strerror(res));
		return -1;
	}

	run_async_sink(&data);
}

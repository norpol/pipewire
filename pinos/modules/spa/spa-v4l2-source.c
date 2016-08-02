/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <gio/gio.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/video/format.h>

#include "spa-v4l2-source.h"

#define PINOS_SPA_V4L2_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_V4L2_SOURCE, PinosSpaV4l2SourcePrivate))

typedef struct {
  PinosSpaV4l2Source *source;

  guint id;
  gboolean have_format;
  PinosPort *port;
} SourcePortData;

struct _PinosSpaV4l2SourcePrivate
{
  SpaPollFd fds[16];
  unsigned int n_fds;
  SpaPollItem poll;

  gboolean running;
  pthread_t thread;

  GBytes *format;

  GList *ports;
};

enum {
  PROP_0,
};

G_DEFINE_TYPE (PinosSpaV4l2Source, pinos_spa_v4l2_source, PINOS_TYPE_NODE);

static SpaResult
make_node (SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
  SpaResult res;
  void *hnd, *state = NULL;
  SpaEnumHandleFactoryFunc enum_func;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    g_error ("can't load %s: %s", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    g_error ("can't find enum function");
    return SPA_RESULT_ERROR;
  }

  while (true) {
    const SpaHandleFactory *factory;
    void *iface;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_error ("can't enumerate factories: %d", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    handle = calloc (1, factory->size);
    if ((res = factory->init (factory, handle)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = handle->get_interface (handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void *
loop (void *user_data)
{
  PinosSpaV4l2Source *this = user_data;
  PinosSpaV4l2SourcePrivate *priv = this->priv;
  int r;

  g_debug ("spa-v4l2-source %p: enter thread", this);
  while (priv->running) {
    SpaPollNotifyData ndata;

    r = poll ((struct pollfd *) priv->fds, priv->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      g_debug ("spa-v4l2-source %p: select timeout", this);
      break;
    }
    if (priv->poll.after_cb) {
      ndata.fds = priv->poll.fds;
      ndata.n_fds = priv->poll.n_fds;
      ndata.user_data = priv->poll.user_data;
      priv->poll.after_cb (&ndata);
    }
  }
  g_debug ("spa-v4l2-source %p: leave thread", this);
  return NULL;
}

static void
on_source_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  PinosSpaV4l2Source *this = user_data;
  PinosSpaV4l2SourcePrivate *priv = this->priv;

  switch (event->type) {
    case SPA_EVENT_TYPE_CAN_PULL_OUTPUT:
    {
      SpaOutputInfo info[1] = { 0, };
      SpaResult res;
      GList *walk;

      if ((res = spa_node_port_pull_output (node, 1, info)) < 0)
        g_debug ("spa-v4l2-source %p: got pull error %d, %d", this, res, info[0].status);

      for (walk = priv->ports; walk; walk = g_list_next (walk)) {
        SourcePortData *data = walk->data;
        GError *error = NULL;

        if (!pinos_port_send_buffer (data->port, info[0].buffer_id, &error)) {
          g_debug ("send failed: %s", error->message);
          g_clear_error (&error);
        }
      }
      break;
    }

    case SPA_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *poll = event->data;
      int err;

      priv->poll = *poll;
      priv->fds[0] = poll->fds[0];
      priv->n_fds = 1;
      priv->poll.fds = priv->fds;

      if (!priv->running) {
        priv->running = true;
        if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
          g_debug ("spa-v4l2-source %p: can't create thread: %s", this, strerror (err));
          priv->running = false;
        }
      }
      break;
    }
    case SPA_EVENT_TYPE_REMOVE_POLL:
    {
      if (priv->running) {
        priv->running = false;
        pthread_join (priv->thread, NULL);
      }
      break;
    }
    default:
      g_debug ("got event %d", event->type);
      break;
  }
}

static void
setup_node (PinosSpaV4l2Source *this)
{
  PinosNode *node = PINOS_NODE (this);
  PinosSpaV4l2SourcePrivate *priv = this->priv;
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  spa_node_set_event_callback (node->node, on_source_event, this);

  if ((res = spa_node_get_props (node->node, &props)) < 0)
    g_debug ("got get_props error %d", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.value = "/dev/video1";
  value.size = strlen (value.value)+1;
  spa_props_set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (node->node, props)) < 0)
    g_debug ("got set_props error %d", res);
}

#if 0
static SpaResult
negotiate_formats (PinosSpaV4l2Source *this)
{
  PinosNode *node = PINOS_NODE (this);
  PinosSpaV4l2SourcePrivate *priv = this->priv;
  SpaResult res;
  SpaFormat *format;
  SpaProps *props;
  uint32_t val;
  SpaPropValue value;
  void *state = NULL;
  SpaFraction frac;
  SpaRectangle rect;
  const gchar *str;

  if ((res = spa_node_port_enum_formats (node->node, 0, &format, NULL, &state)) < 0)
    return res;

  props = &format->props;

  value.type = SPA_PROP_TYPE_UINT32;
  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_VIDEO_FORMAT_YUY2;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_FORMAT), &value)) < 0)
    return res;

  value.type = SPA_PROP_TYPE_RECTANGLE;
  value.size = sizeof (SpaRectangle);
  value.value = &rect;
  rect.width = 320;
  rect.height = 240;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_SIZE), &value)) < 0)
    return res;

  value.type = SPA_PROP_TYPE_FRACTION;
  value.size = sizeof (SpaFraction);
  value.value = &frac;
  frac.num = 25;
  frac.denom = 1;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_FRAMERATE), &value)) < 0)
    return res;

  if ((res = spa_node_port_set_format (node->node, 0, 0, format)) < 0)
    return res;

  str = "video/x-raw,"
        " format=(string)YUY2,"
        " width=(int)320,"
        " height=(int)240,"
        " framerate=(fraction)30/1";
  priv->format = g_bytes_new_static (str, strlen (str)+1);

  priv->n_buffers = 16;
  if ((res = spa_node_port_alloc_buffers (node->node, 0, NULL, 0, priv->buffers, &priv->n_buffers)) < 0)
    return res;

  return SPA_RESULT_OK;
}
#endif

static void
start_pipeline (PinosSpaV4l2Source *this)
{
#if 0
  PinosSpaV4l2SourcePrivate *priv = this->priv;
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaCommand cmd;
  int err;

  g_debug ("spa-v4l2-source %p: starting pipeline", this);
  negotiate_formats (this);

  cmd.type = SPA_COMMAND_START;
  if ((res = spa_node_send_command (node->node, &cmd)) < 0)
    g_debug ("got error %d", res);

  priv->running = true;
  if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
    g_debug ("spa-v4l2-source %p: can't create thread", strerror (err));
    priv->running = false;
  }
#endif
}

static void
stop_pipeline (PinosSpaV4l2Source *this)
{
  PinosSpaV4l2SourcePrivate *priv = this->priv;
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaCommand cmd;

  g_debug ("spa-v4l2-source %p: stopping pipeline", this);

  if (priv->running) {
    priv->running = false;
    pthread_join (priv->thread, NULL);
  }

  cmd.type = SPA_COMMAND_STOP;
  if ((res = spa_node_send_command (node->node, &cmd)) < 0)
    g_debug ("got error %d", res);
}

static void
destroy_pipeline (PinosSpaV4l2Source *this)
{
  g_debug ("spa-v4l2-source %p: destroy pipeline", this);
}

static gboolean
set_state (PinosNode      *node,
           PinosNodeState  state)
{
  PinosSpaV4l2Source *this = PINOS_SPA_V4L2_SOURCE (node);

  g_debug ("spa-source %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_SUSPENDED:
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      break;

    case PINOS_NODE_STATE_IDLE:
      stop_pipeline (this);
      break;

    case PINOS_NODE_STATE_RUNNING:
      start_pipeline (this);
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  pinos_node_update_state (node, state);
  return TRUE;
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
on_activate (PinosPort *port, gpointer user_data)
{
  SourcePortData *data = user_data;
  PinosSpaV4l2Source *source = data->source;

  pinos_node_report_busy (PINOS_NODE (source));

  return TRUE;
}

static void
on_deactivate (PinosPort *port, gpointer user_data)
{
  SourcePortData *data = user_data;
  PinosSpaV4l2Source *source = data->source;

  pinos_node_report_idle (PINOS_NODE (source));
}

static void
free_source_port_data (SourcePortData *data)
{
  g_slice_free (SourcePortData, data);
}

static gboolean
remove_port (PinosNode       *node,
             guint            id)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (node);
  PinosSpaV4l2SourcePrivate *priv = source->priv;
  GList *walk;

  for (walk = priv->ports; walk; walk = g_list_next (walk)) {
    SourcePortData *data = walk->data;

    if (data->id == id) {
      free_source_port_data (data);
      priv->ports = g_list_delete_link (priv->ports, walk);
      break;
    }
  }
  if (priv->ports == NULL)
    pinos_node_report_idle (node);

  return TRUE;
}

static void
source_constructed (GObject * object)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (object);

  G_OBJECT_CLASS (pinos_spa_v4l2_source_parent_class)->constructed (object);

  setup_node (source);
}

static void
source_finalize (GObject * object)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (object);

  g_debug ("spa-source %p: dispose", source);
  destroy_pipeline (source);

  G_OBJECT_CLASS (pinos_spa_v4l2_source_parent_class)->finalize (object);
}

static gboolean
on_received_buffer (PinosPort *port, uint32_t buffer_id, GError **error, gpointer user_data)
{
  return FALSE;
}

static gboolean
on_received_event (PinosPort *port, SpaEvent *event, GError **error, gpointer user_data)
{
  PinosNode *node = user_data;
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (node);
  PinosSpaV4l2SourcePrivate *priv = source->priv;
  SpaResult res;

  switch (event->type) {
    case SPA_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaEventReuseBuffer *rb = event->data;

      if ((res = spa_node_port_reuse_buffer (node->node,
                                             event->port_id,
                                             rb->buffer_id,
                                             rb->offset,
                                             rb->size)) < 0)
        g_warning ("client-node %p: error reuse buffer: %d", node, res);
      break;
    }
    default:
      if ((res = spa_node_port_push_event (node->node, port->id, event)) < 0)
        g_warning ("client-node %p: error pushing event: %d", node, res);
      break;
  }
  return TRUE;
}


static PinosPort *
add_port (PinosNode       *node,
          PinosDirection   direction,
          guint            id,
          GError         **error)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (node);
  PinosSpaV4l2SourcePrivate *priv = source->priv;
  SourcePortData *data;

  data = g_slice_new0 (SourcePortData);
  data->source = source;
  data->id = id;
  data->have_format = FALSE;

  data->port = PINOS_NODE_CLASS (pinos_spa_v4l2_source_parent_class)
                    ->add_port (node, direction, id, error);

  pinos_port_set_received_cb (data->port, on_received_buffer, on_received_event, node, NULL);

  g_debug ("connecting signals");
  g_signal_connect (data->port, "activate", (GCallback) on_activate, data);
  g_signal_connect (data->port, "deactivate", (GCallback) on_deactivate, data);

  priv->ports = g_list_append (priv->ports, data);

  return data->port;
}

static void
pinos_spa_v4l2_source_class_init (PinosSpaV4l2SourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaV4l2SourcePrivate));

  gobject_class->constructed = source_constructed;
  gobject_class->finalize = source_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;

  node_class->set_state = set_state;
  node_class->add_port = add_port;
  node_class->remove_port = remove_port;
}

static void
pinos_spa_v4l2_source_init (PinosSpaV4l2Source * source)
{
  source->priv = PINOS_SPA_V4L2_SOURCE_GET_PRIVATE (source);
}

PinosNode *
pinos_spa_v4l2_source_new (PinosDaemon *daemon,
                           const gchar *name,
                           PinosProperties *properties)
{
  PinosNode *node;
  SpaNode *n;
  SpaResult res;

  if ((res = make_node (&n,
                        "spa/build/plugins/v4l2/libspa-v4l2.so",
                        "v4l2-source")) < 0) {
    g_error ("can't create v4l2-source: %d", res);
    return NULL;
  }

  node = g_object_new (PINOS_TYPE_SPA_V4L2_SOURCE,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);

  return node;
}

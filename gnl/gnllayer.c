/* GStreamer
 * Copyright (C) 2001 Wim Taymans <wim.taymans@chello.be>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */



#include <gnl/gnllayer.h>

static void 		gnl_layer_class_init 		(GnlLayerClass *klass);
static void 		gnl_layer_init 			(GnlLayer *layer);

static GnlLayerClass *parent_class = NULL;

GType
gnl_layer_get_type (void)
{
  static GType layer_type = 0;

  if (!layer_type) {
    static const GTypeInfo layer_info = {
      sizeof (GnlLayerClass),
      NULL,
      NULL,
      (GClassInitFunc) gnl_layer_class_init,
      NULL,
      NULL,
      sizeof (GnlLayer),
      32,
      (GInstanceInitFunc) gnl_layer_init,
    };
    layer_type = g_type_register_static (G_TYPE_OBJECT, "GnlLayer", &layer_info, 0);
  }
  return layer_type;
}

static void
gnl_layer_class_init (GnlLayerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class =       (GObjectClass*)klass;

  parent_class = g_type_class_ref (GST_TYPE_BIN);
}


static void
gnl_layer_init (GnlLayer *layer)
{
}


GnlLayer*
gnl_layer_new (const gchar *name)
{
  return NULL;
}

void
gnl_layer_add_source (GnlLayer *layer, GnlSource *source, guint64 start)
{
  g_return_if_fail (layer != NULL);
  g_return_if_fail (GNL_IS_LAYER (layer));
  g_return_if_fail (source != NULL);
  g_return_if_fail (GNL_IS_SOURCE (source));

}



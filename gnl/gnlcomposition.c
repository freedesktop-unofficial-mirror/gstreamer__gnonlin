/* GStreamer
 * Copyright (C) 2001 Wim Taymans <wim.taymans@chello.be>
 *               2004 Edward Hervey <edward@fluendo.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gnl.h"

GST_BOILERPLATE (GnlComposition, gnl_composition, GnlObject, GNL_TYPE_OBJECT);

static GstElementDetails gnl_composition_details = GST_ELEMENT_DETAILS (
   "GNonLin Composition",
   "Filter/Editor",
   "Combines GNL objects",
   "Wim Taymans <wim.taymans@chello.be>, Edward Hervey <bilboed@bilboed.com>"
   );

GST_DEBUG_CATEGORY_STATIC (gnlcomposition);
#define GST_CAT_DEFAULT gnlcomposition

struct _GnlCompositionPrivate {
  gboolean	dispose_has_run;
  
  /* 
     Sorted List of GnlObjects , ThreadSafe 
     objects_start : sorted by start-time then priority
     objects_stop : sorted by stop-time then priority
     objects_hash : contains signal handlers id for controlled objects
     objects_lock : mutex to acces/modify any of those lists/hashtable
  */
  GList			*objects_start;
  GList			*objects_stop;
  GHashTable		*objects_hash;
  GMutex		*objects_lock;  
  
  /* source ghostpad */
  GstPad		*ghostpad;

  /* current stack */
  GList			*current;

  /*
    current segment seek start/stop time. 
    Reconstruct pipeline ONLY if seeking outside of those values
  */
  GstClockTime		segment_start;
  GstClockTime		segment_stop;
  
  /* Seek segment handler */
  GstSegment		*segment;
  
  /*
    OUR sync_handler on the child_bus 
    We are called before gnl_object_sync_handler
  */
  GstBusSyncHandler	 sync_handler;
  gpointer		 sync_handler_data;

  GstPadEventFunction	gnl_event_pad_func;
};

static void
gnl_composition_dispose		(GObject *object);
static void
gnl_composition_finalize 	(GObject *object);
static void
gnl_composition_reset		(GnlComposition *comp);

static GstBusSyncReply
gnl_composition_sync_handler	(GstBus *bus, GstMessage *message,
				 GnlComposition *comp);
static gboolean
gnl_composition_add_object	(GstBin *bin, GstElement *element);

static gboolean
gnl_composition_remove_object	(GstBin *bin, GstElement *element);

static GstStateChangeReturn
gnl_composition_change_state	(GstElement *element, 
				 GstStateChange transition);


static gboolean
update_pipeline		(GnlComposition *comp, GstClockTime currenttime,
			 gboolean initial);

#define COMP_REAL_START(comp) \
  (MAX (comp->private->segment->start, GNL_OBJECT (comp)->start))

#define COMP_REAL_STOP(comp) \
  ((GST_CLOCK_TIME_IS_VALID (comp->private->segment->stop) \
    ? (MIN (comp->private->segment->stop, GNL_OBJECT (comp)->stop))) \
   : (GNL_OBJECT (comp)->stop))

#define COMP_ENTRY(comp, object) \
  (g_hash_table_lookup (comp->private->objects_hash, (gconstpointer) object))

#define COMP_OBJECTS_LOCK(comp) (g_mutex_lock (comp->private->objects_lock))
#define COMP_OBJECTS_UNLOCK(comp) (g_mutex_unlock (comp->private->objects_lock))

static gboolean
gnl_composition_prepare		(GnlObject *object);


typedef struct _GnlCompositionEntry GnlCompositionEntry;

struct _GnlCompositionEntry
{
  GnlObject	*object;
  
  /* handler ids for property notifications */
  gulong	starthandler;
  gulong	stophandler;
  gulong	priorityhandler;
  gulong	activehandler;

  /* handler id for 'no-more-pads' signal */
  gulong	nomorepadshandler;
};

static void
gnl_composition_base_init (gpointer g_class)
{
  GstElementClass *gstclass = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details (gstclass, &gnl_composition_details);
}

static void
gnl_composition_class_init (GnlCompositionClass *klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBinClass *gstbin_class;
  GnlObjectClass *gnlobject_class;

  gobject_class    = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;
  gstbin_class     = (GstBinClass*)klass;
  gnlobject_class  = (GnlObjectClass*)klass;

  GST_DEBUG_CATEGORY_INIT (gnlcomposition, "gnlcomposition", 0, "GNonLin Composition");

  gobject_class->dispose	= GST_DEBUG_FUNCPTR (gnl_composition_dispose);
  gobject_class->finalize 	= GST_DEBUG_FUNCPTR (gnl_composition_finalize);

  gstelement_class->change_state = gnl_composition_change_state;
/*   gstelement_class->query	 = gnl_composition_query; */

  gstbin_class->add_element      = 
    GST_DEBUG_FUNCPTR (gnl_composition_add_object);
  gstbin_class->remove_element   = 
    GST_DEBUG_FUNCPTR (gnl_composition_remove_object);

  gnlobject_class->prepare       = 
    GST_DEBUG_FUNCPTR (gnl_composition_prepare);
/*   gnlobject_class->covers	 = gnl_composition_covers_func; */

/*   klass->nearest_cover	 	 = gnl_composition_nearest_cover_func; */
}

static void
gnl_composition_put_sync_handler (GnlComposition *comp,
				  GstBusSyncHandler handler,
				  gpointer data)
{
  GstBus	*bus;

  bus = GST_BIN (comp)->child_bus;

  comp->private->sync_handler = bus->sync_handler;
  comp->private->sync_handler_data = bus->sync_handler_data;
  
  bus->sync_handler = handler;
  bus->sync_handler_data = data;
}

static void
hash_value_destroy	(GnlCompositionEntry *entry)
{
  g_signal_handler_disconnect (entry->object, entry->starthandler);
  g_signal_handler_disconnect (entry->object, entry->stophandler);
  g_signal_handler_disconnect (entry->object, entry->priorityhandler);
  g_signal_handler_disconnect (entry->object, entry->activehandler);

  if (entry->nomorepadshandler)
    g_signal_handler_disconnect (entry->object, entry->nomorepadshandler);
  g_free(entry);
}

static void
gnl_composition_init (GnlComposition *comp, GnlCompositionClass *klass)
{
  GST_OBJECT_FLAG_SET (comp, GNL_OBJECT_SOURCE);


  comp->private = g_new0 (GnlCompositionPrivate, 1);
  comp->private->objects_lock = g_mutex_new();
  comp->private->objects_start = NULL;
  comp->private->objects_stop = NULL;

  comp->private->segment = gst_segment_new();

  comp->private->objects_hash = g_hash_table_new_full
    (g_direct_hash,
     g_direct_equal,
     NULL,
     (GDestroyNotify) hash_value_destroy);

  gnl_composition_put_sync_handler 
    (comp, (GstBusSyncHandler) gnl_composition_sync_handler, comp);
  
  gnl_composition_reset (comp);
}

static void
gnl_composition_dispose (GObject *object)
{
  GnlComposition	*comp = GNL_COMPOSITION (object);

  if (comp->private->dispose_has_run)
    return;

  comp->private->dispose_has_run = TRUE;

  if (comp->private->ghostpad)
    gst_element_remove_pad (GST_ELEMENT (object),
			    comp->private->ghostpad);

  /* FIXME: all of a sudden I can't remember what we have to do here... */
}

static void
gnl_composition_finalize (GObject *object)
{
  GnlComposition *comp = GNL_COMPOSITION (object);

  GST_INFO("finalize");

  COMP_OBJECTS_LOCK (comp);
  g_list_free (comp->private->objects_start);
  g_list_free (comp->private->objects_stop);
  g_hash_table_destroy (comp->private->objects_hash);
  COMP_OBJECTS_UNLOCK (comp);

  g_free (comp->private);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gnl_composition_reset	(GnlComposition *comp)
{
  comp->private->segment_start = GST_CLOCK_TIME_NONE;
  comp->private->segment_stop = GST_CLOCK_TIME_NONE;

  gst_segment_init (comp->private->segment,
		    GST_FORMAT_TIME);

  if (comp->private->current)
    g_list_free (comp->private->current);
  comp->private->current = NULL;

  if (comp->private->ghostpad) {
    gst_element_remove_pad (GST_ELEMENT (comp),
			    comp->private->ghostpad);
    gst_object_unref (comp->private->ghostpad);
    comp->private->ghostpad = NULL;
  }
  /* FIXME: uncomment the following when setting a NULL target is allowed */
/*   gnl_object_ghost_pad_set_target (GNL_OBJECT (comp), */
/* 				   comp->private->ghostpad, */
/* 				   NULL); */
}

static GstBusSyncReply
gnl_composition_sync_handler	(GstBus *bus, GstMessage *message,
				 GnlComposition *comp)
{
  GstBusSyncReply	reply = GST_BUS_DROP;
  gboolean		dropit = FALSE;

  GST_DEBUG_OBJECT (comp, "bus:%s message:%s",
		    GST_OBJECT_NAME (bus),
		    gst_message_type_get_name(GST_MESSAGE_TYPE (message)));  

  switch (GST_MESSAGE_TYPE (message)) {
  case GST_MESSAGE_SEGMENT_DONE: {
    gint64	pos;
    GstFormat	format;
    /* reorganize pipeline */
    
    gst_message_parse_segment_done (message, &format, &pos);
    if (format != GST_FORMAT_TIME) {
      /* this can happen when filesrc emits a segment-done in BYTES */
      GST_WARNING_OBJECT (comp, "Got a SEGMENT_DONE_MESSAGE with a format different from GST_FORMAT_TIME");
    }
    GST_DEBUG_OBJECT (comp, "Save a SEGMENT_DONE message, updating pipeline");
    update_pipeline (comp, (GstClockTime) comp->private->segment_stop, FALSE);
    
    if (!(comp->private->current)) {
      GST_DEBUG_OBJECT (comp, "Nothing else to play");
      if (gst_pad_is_linked (comp->private->ghostpad)) {
	GstPad	*peerpad = gst_pad_get_peer (comp->private->ghostpad);
	gst_pad_event_default (peerpad,
			       gst_event_new_eos());
	gst_object_unref (peerpad);
      }
    }
    break;
  }
  default:
    break;
  }

  if (dropit)
    gst_message_unref (message);
  else
    if (comp->private->sync_handler)
      reply = comp->private->sync_handler 
	(bus, message, comp->private->sync_handler_data);
  
  return reply;  
}

static gint
priority_comp (GnlObject *a, GnlObject *b)
{
  return a->priority - b->priority;
}

static gboolean
have_to_update_pipeline (GnlComposition *comp)
{
  GST_DEBUG_OBJECT (comp,
		    "segment[%"GST_TIME_FORMAT"--%"GST_TIME_FORMAT"] current[%"GST_TIME_FORMAT"--%"GST_TIME_FORMAT"]",
		    GST_TIME_ARGS (comp->private->segment->start),
		    GST_TIME_ARGS (comp->private->segment->stop),
		    GST_TIME_ARGS (comp->private->segment_start),
		    GST_TIME_ARGS (comp->private->segment_stop));

  if (comp->private->segment->start < comp->private->segment_start)
    return TRUE;
  if (comp->private->segment->start >= comp->private->segment_stop)
    return TRUE;
  return FALSE;
}

/**
 * get_new_seek_event:
 *
 * Returns a seek event for the currently configured segment
 * and start/stop values
 *
 * The GstSegment and segment_start|stop must have been configured
 * before calling this function.
 */
static GstEvent*
get_new_seek_event (GnlComposition *comp, gboolean initial)
{
  GstSeekFlags	flags;

  GST_DEBUG_OBJECT (comp, "initial:%d", initial);
  /* remove the seek flag */
  if (!(initial))
    flags = comp->private->segment->flags;
  else
    flags = GST_SEEK_FLAG_SEGMENT;

  GST_DEBUG_OBJECT (comp, "Using flags:%d");
  return gst_event_new_seek (comp->private->segment->rate,
			     comp->private->segment->format,
			     flags,
			     GST_SEEK_TYPE_SET,
			     MAX (comp->private->segment->start, comp->private->segment_start),
			     GST_SEEK_TYPE_SET,
			     GST_CLOCK_TIME_IS_VALID (comp->private->segment->stop)
			     ? MIN (comp->private->segment->stop, comp->private->segment_stop)
			     : comp->private->segment_stop);
}

static void
handle_seek_event (GnlComposition *comp, GstEvent *event)
{
  gboolean	update;
  gdouble	rate;
  GstFormat	format;
  GstSeekFlags flags;
  GstSeekType	cur_type, stop_type;
  gint64	cur, stop;
  
  gst_event_parse_seek (event, &rate, &format, &flags,
			&cur_type, &cur,
			&stop_type, &stop);
  
  GST_DEBUG_OBJECT (comp, "start:%"GST_TIME_FORMAT" -- stop:%"GST_TIME_FORMAT"  flags:%d",
		    GST_TIME_ARGS (cur), GST_TIME_ARGS (stop),
		    flags);

  gst_segment_set_seek (comp->private->segment,
			rate, format, flags,
			cur_type, cur, stop_type, stop,
			&update);
  
  GST_DEBUG_OBJECT (comp, "Segment now has flags:%d",
		    comp->private->segment->flags);

  /* crop the segment start/stop values */
  comp->private->segment->start = MAX (comp->private->segment->start,
				       GNL_OBJECT(comp)->start);
  comp->private->segment->stop = MIN (comp->private->segment->stop,
				      GNL_OBJECT(comp)->stop);

  /* Check if we need to update the pipeline */
  if (have_to_update_pipeline (comp)) {
    update_pipeline (comp, comp->private->segment->start, FALSE);
  };
}

static gboolean
gnl_composition_event_handler	(GstPad *ghostpad, GstEvent *event)
{
  GnlComposition	*comp = GNL_COMPOSITION (gst_pad_get_parent (ghostpad));
  gboolean		res = TRUE;


  GST_DEBUG_OBJECT (comp, "event type:%s",
		    GST_EVENT_TYPE_NAME (event));
  switch (GST_EVENT_TYPE (event)) {
  case GST_EVENT_SEEK: {
    handle_seek_event (comp, event);
    break;
  }
  default:
    break;
  }
  
  if (res)
    res = comp->private->gnl_event_pad_func (ghostpad, event);

  gst_object_unref (comp);
  return res;
}

static void
gnl_composition_ghost_pad_set_target	(GnlComposition *comp,
					 GstPad	*target)
{
  gboolean	hadghost = (comp->private->ghostpad) ? TRUE : FALSE;

  GST_DEBUG_OBJECT (comp, "%s:%s , hadghost:%d",
		    GST_DEBUG_PAD_NAME (target),
		    hadghost);

  if (!(hadghost)) {
    comp->private->ghostpad = gnl_object_ghost_pad_no_target (GNL_OBJECT (comp),
							      "src",
							      GST_PAD_SRC);
  } else {
    GstPad	*ptarget = gst_ghost_pad_get_target (GST_GHOST_PAD (comp->private->ghostpad));

    GST_DEBUG_OBJECT (comp, "Previous target was %s:%s, blocking that pad",
		      GST_DEBUG_PAD_NAME (ptarget));
/*     if (!gst_pad_is_blocked (ptarget) && !(gst_pad_set_blocked (ptarget, TRUE))) */
/*       GST_WARNING_OBJECT (comp, "Couldn't block pad %s:%s", */
/* 			  GST_DEBUG_PAD_NAME (ptarget)); */
    gst_object_unref (ptarget);
  }

/*   if (gst_pad_is_blocked (target)) */
/*      if (!(gst_pad_set_blocked (target, FALSE))) */
/*      GST_WARNING_OBJECT (comp, "Couldn't unblock pad %s:%s", */
/* 			 GST_DEBUG_PAD_NAME (target)); */

  gnl_object_ghost_pad_set_target (GNL_OBJECT (comp),
				   comp->private->ghostpad,
				   target);
  comp->private->gnl_event_pad_func = GST_PAD_EVENTFUNC (comp->private->ghostpad);
  gst_pad_set_event_function (comp->private->ghostpad,
			      GST_DEBUG_FUNCPTR (gnl_composition_event_handler));
  if (!(hadghost)) {
    if (!(gst_element_add_pad (GST_ELEMENT (comp),
			       comp->private->ghostpad)))
      GST_WARNING ("couldn't add the ghostpad");
    else
      gst_element_no_more_pads (GST_ELEMENT (comp));
  }
  GST_DEBUG_OBJECT (comp, "END");
}

/**
 * get_stack_list:
 * @comp: The #GnlComposition
 * @timestamp: The #GstClockTime to look at
 * @priority: The priority level to start looking from
 * @activeonly: Only look for active elements if TRUE
 *
 * Not MT-safe, you should take the objects lock before calling it.
 * Returns: A #GList of #GnlObject sorted in priority order, corresponding
 * to the given search arguments. The list can be empty.
 */

static GList *
get_stack_list	(GnlComposition *comp, GstClockTime timestamp,
		 guint priority, gboolean activeonly)
{
  GList	*tmp = comp->private->objects_start;
  GList	*stack = NULL;

  GST_DEBUG_OBJECT (comp, "timestamp:%lld, priority:%d, activeonly:%d",
		    timestamp, priority, activeonly);

  /*
    Iterate the list with a stack:
    Either :
      _ ignore
      _ add to the stack (object.start <= timestamp < object.stop)
        _ add in priority order
      _ stop iteration (object.start > timestamp)
  */

  for ( ; tmp ; tmp = g_list_next (tmp)) {
    GnlObject	*object = GNL_OBJECT (tmp->data);

    GST_LOG_OBJECT (object, "start: %lld , stop:%lld , duration:%lld, priority:%d",
		    object->start, object->stop, object->duration, object->priority);

    if (object->start <= timestamp)
      {
	if (object->stop <= timestamp) 
	  {
	    if (stack) {
	      GST_LOG_OBJECT (comp, "too far, stopping iteration");
	      break;
	    }
	  }
	else 
	  if ( (object->priority >= priority) 
	       && ((!activeonly) || (object->active)) ) {
	    GST_LOG_OBJECT (comp, "adding %s: sorted to the stack", 
			    GST_OBJECT_NAME (object));
	    stack = g_list_insert_sorted (stack, object, 
					  (GCompareFunc) priority_comp);
	  }
      }
  }
  
  return stack;
}

/**
 * get_clean_toplevel_stack:
 * @comp: The #GnlComposition
 * @timestamp: The #GstClockTime to look at
 * @stop_time: Pointer to a #GstClockTime for min stop time of returned stack
 *
 * Returns: The new current stack for the given #GnlComposition
 */

static GList *
get_clean_toplevel_stack (GnlComposition *comp, GstClockTime timestamp,
			  GstClockTime *stop_time)
{
  GList	*stack, *tmp;
  GList	*ret = NULL;
  gint	size = 1;
  GstClockTime	stop = G_MAXUINT64;

  GST_DEBUG_OBJECT (comp, "timestamp:%lld", timestamp);

  stack = get_stack_list (comp, timestamp, 0, TRUE);
  for (tmp = stack; tmp && size; tmp = g_list_next(tmp), size--) {
    GnlObject	*object = tmp->data;

    ret = g_list_append (ret, object);
    if (stop > object->stop)
      stop = object->stop;
    /* FIXME : ADD CASE FOR OPERATION */
    /* size += number of input for operation */
  }
  
  g_list_free(stack);

  if (stop_time) {
    if (ret)
      *stop_time = stop;
    else
      *stop_time = 0;
  }
  
  return ret;
}


/*
 *
 * UTILITY FUNCTIONS
 *
 */

/**
 * get_src_pad:
 * #element: a #GstElement
 *
 * Returns: The src pad for the given element. A reference was added to the
 * returned pad, remove it when you don't need that pad anymore.
 * Returns NULL if there's no source pad.
 */

static GstPad *
get_src_pad (GstElement *element)
{
  GstIterator		*it;
  GstIteratorResult	itres;
  GstPad		*srcpad;

  it = gst_element_iterate_src_pads (element);
  itres = gst_iterator_next (it, (gpointer) &srcpad);
  if (itres != GST_ITERATOR_OK) {
    GST_WARNING ("%s doesn't have a src pad !",
		 GST_ELEMENT_NAME (element));
    srcpad = NULL;
  }
  gst_iterator_free (it);
  return srcpad;
}


/*
 *
 * END OF UTILITY FUNCTIONS
 *
 */

static gboolean
gnl_composition_prepare	(GnlObject *object)
{
  gboolean		ret = TRUE;

  return ret;
}

static GstStateChangeReturn
gnl_composition_change_state (GstElement *element, GstStateChange transition)
{
  GnlComposition *comp = GNL_COMPOSITION (element);
  GstStateChangeReturn	ret;

  switch (transition) {
  case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    /* state-lock elements not used */
/*     update_pipeline (comp, COMP_REAL_START (comp)); */
    break;
  default:
    break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
  case GST_STATE_CHANGE_READY_TO_PAUSED:

    /* set ghostpad target */
    update_pipeline (comp,
		     COMP_REAL_START(comp), TRUE);
    
    break;
  case GST_STATE_CHANGE_PAUSED_TO_READY:
    gnl_composition_reset (comp);
    break;
  default:
    break;
  }

  return ret;
}

static gint
objects_start_compare	(GnlObject *a, GnlObject *b)
{
  if (a->start == b->start)
    return a->priority - b->priority;
  return b->start - a->start;
}

static gint
objects_stop_compare	(GnlObject *a, GnlObject *b)
{
  if (a->stop == b->stop)
    return a->priority - b->priority;
  return a->stop - b->stop;
}

static void
update_start_stop_duration	(GnlComposition *comp)
{
  GnlObject	*obj;
  GnlObject	*cobj = GNL_OBJECT (comp);

  GST_DEBUG_OBJECT (comp, "...");
  if (!(comp->private->objects_start)) {
    GST_LOG ("no objects, resetting everything to 0");
    if (cobj->start) {
      cobj->start = 0;
      g_object_notify (G_OBJECT (cobj), "start");
    }
    if (cobj->duration) {
      cobj->duration = 0;
      g_object_notify (G_OBJECT (cobj), "duration");
    }
    if (cobj->stop) {
      cobj->stop = 0;
      g_object_notify (G_OBJECT (cobj), "stop");
    }
    return;
  }

  obj = GNL_OBJECT (comp->private->objects_start->data);
  if (obj->start != cobj->start) {
    GST_LOG_OBJECT (obj, "setting start from %s to %" GST_TIME_FORMAT,
		    GST_OBJECT_NAME (obj),
		    GST_TIME_ARGS (obj->start));
    cobj->start = obj->start;
    g_object_notify (G_OBJECT (cobj), "start");
  }

  obj = GNL_OBJECT (comp->private->objects_stop->data);
  if (obj->stop != cobj->stop) {
    GST_LOG_OBJECT (obj, "setting stop from %s to %" GST_TIME_FORMAT,
		    GST_OBJECT_NAME (obj),
		    GST_TIME_ARGS (obj->stop));
    cobj->stop = obj->stop;
    g_object_notify (G_OBJECT (cobj), "stop");
  }

  if ((cobj->stop - cobj->start) != cobj->duration) {
    cobj->duration = cobj->stop - cobj->start;
    g_object_notify (G_OBJECT (cobj), "duration");
  }

  GST_LOG_OBJECT (comp, 
		  "start:%"GST_TIME_FORMAT
		  " stop:%"GST_TIME_FORMAT
		  " duration:%"GST_TIME_FORMAT,
		  GST_TIME_ARGS (cobj->start),
		  GST_TIME_ARGS (cobj->stop),
		  GST_TIME_ARGS (cobj->duration));
}

static void
no_more_pads_object_cb	(GstElement *element, GnlComposition *comp)
{
  GnlObject	*object = GNL_OBJECT (element);
  GList		*tmp, *prev;
  GstPad	*pad = NULL;

  GST_LOG_OBJECT (element, "no more pads");
  if (!(pad = get_src_pad(element)))
    return;

  /* FIXME : ADD CASE FOR OPERATIONS */

  COMP_OBJECTS_LOCK (comp);
  
  for (tmp = comp->private->current, prev = NULL;
       tmp;
       prev = tmp, tmp = g_list_next(tmp)) {
    GnlObject	*curobj = GNL_OBJECT (tmp->data);
    
    if (curobj == object ) {
      GnlCompositionEntry	*entry = COMP_ENTRY(comp, object);
      
      if (prev) {
	/* link that object to the previous */
	gst_element_link (element, GST_ELEMENT (prev->data));
	break;
      } else {
	/* toplevel element */
	gnl_composition_ghost_pad_set_target (comp, pad);
	gst_pad_send_event (pad, get_new_seek_event(comp, FALSE));
      }
      /* remove signal handler */
      g_signal_handler_disconnect (object, entry->nomorepadshandler);
      entry->nomorepadshandler = 0;
    }
  }

  COMP_OBJECTS_UNLOCK (comp);

  gst_object_unref (pad);
  
  GST_DEBUG_OBJECT (comp, "end");
}

/**
 * compare_relink_stack:
 * @comp: The #GnlComposition
 * @stack: The new stack
 *
 * Compares the given stack to the current one and relinks it if needed
 *
 * Returns: The #GList of #GnlObject no longer used
 */

static GList *
compare_relink_stack	(GnlComposition *comp, GList *stack)
{
  GList	*deactivate = NULL;
  GnlObject	*oldobj = NULL;
  GnlObject	*newobj = NULL;
  GnlObject	*pold = NULL; 
  GnlObject	*pnew = NULL;
  GList	*curo = comp->private->current;
  GList	*curn = stack;

  for (; curo && curn; ) 
    {
      oldobj = GNL_OBJECT (curo->data);
      newobj = GNL_OBJECT (curn->data);
      
      GST_DEBUG ("Comparing old:%s new:%s",
		 GST_ELEMENT_NAME (oldobj),
		 GST_ELEMENT_NAME (newobj));

      if ((oldobj == newobj) && GNL_OBJECT_IS_OPERATION (oldobj))
	{
	  /* guint	size = GNL_OPERATION_SINKS (oldobj); */
	  /* FIXME : check objects the operation works on */
	  /*
	    while (size) {
	    
	    size--;
	    }
	  */
	}
      else if (oldobj != newobj)
	{
	  GstPad	*srcpad;
	  GnlCompositionEntry	*entry;
	  
	  deactivate = g_list_append (deactivate, oldobj);

	  /* unlink from it's previous source */
	  if (pold)
	    {
	      /* unlink old object from previous old obj */
	      gst_element_unlink (GST_ELEMENT (pold), GST_ELEMENT (oldobj));
	    }
	  
	  /* link to it's previous source */
	  if (pnew)
	    {
	      GST_DEBUG_OBJECT (comp, "linking to previous source"); 
	      /* link new object to previous new obj */
	      entry = COMP_ENTRY(comp, newobj);
	      srcpad = get_src_pad (GST_ELEMENT (newobj));
	      
	      if (!(srcpad))
		{
		  GST_LOG ("Adding callback on 'no-more-pads' for %s",
			   GST_ELEMENT_NAME (entry->object));
		  /* special case for elements who don't have their srcpad yet */
		  if (entry->nomorepadshandler)
		    g_signal_handler_disconnect (entry->object, 
						 entry->nomorepadshandler);
		  entry->nomorepadshandler = g_signal_connect
		    (G_OBJECT (newobj),
		     "no-more-pads",
		     G_CALLBACK(no_more_pads_object_cb),
		     comp);
		} 
	      else 
		{
		  GST_LOG ("Linking %s to %s the standard way",
			   GST_ELEMENT_NAME (pnew),
			   GST_ELEMENT_NAME (newobj));
		  gst_element_link (GST_ELEMENT (pnew), GST_ELEMENT (newobj));
		  gst_object_unref (srcpad);
		}
	    }
	  else
	    {
	      /* There's no previous new, which means it's the toplevel element */
	      GST_DEBUG_OBJECT (comp, "top-level element"); 

	      srcpad = get_src_pad (GST_ELEMENT (newobj));

	      if (!(srcpad)) {
		entry = COMP_ENTRY(comp, newobj);
		if (entry->nomorepadshandler)
		  g_signal_handler_disconnect (entry->object,
					       entry->nomorepadshandler);
		entry->nomorepadshandler = g_signal_connect 
		  (G_OBJECT (newobj),
		   "no-more-pads",
		   G_CALLBACK (no_more_pads_object_cb),
		   comp);

	      } else
		gst_object_unref (srcpad);
	    }
	}
      
      curo = g_list_next (curo);
      curn = g_list_next (curn);
      pold = oldobj;
      pnew = newobj;
    }
  
  GST_DEBUG_OBJECT (comp, "curo:%p, curn:%p",
		    curo, curn);

  for (; curn; curn = g_list_next (curn)) {
    GST_DEBUG_OBJECT (curn->data, "...");
    newobj = GNL_OBJECT (curn->data);

    if (pnew)
      gst_element_link (GST_ELEMENT (pnew), GST_ELEMENT (newobj));
    else {
      GstPad	*srcpad = get_src_pad (GST_ELEMENT (newobj));
      GnlCompositionEntry	*entry;

      if (srcpad) {
	gnl_composition_ghost_pad_set_target (comp, srcpad);
	gst_object_unref (srcpad);
      } else {
	entry = COMP_ENTRY(comp, newobj);
	if (entry->nomorepadshandler)
	  g_signal_handler_disconnect (entry->object,
				       entry->nomorepadshandler);
	entry->nomorepadshandler = g_signal_connect
	  (G_OBJECT (newobj),
	   "no-more-pads",
	   G_CALLBACK (no_more_pads_object_cb),
	   comp);
      }
    }

    pnew = newobj;
  }

  for (; curo; curo = g_list_next (curo)) {
    oldobj = GNL_OBJECT (curo->data);

    deactivate = g_list_append (deactivate, oldobj);
    if (pold)
      gst_element_unlink (GST_ELEMENT (pold), GST_ELEMENT (oldobj));

    pold = oldobj;
  }

  /* 
     we need to remove from the deactivate lists, item which have changed priority
     but are still in the new stack.
     FIXME: This is damn ugly !
  */
  for (curo = comp->private->current; curo; curo = g_list_next (curo))
    for (curn = stack; curn; curn = g_list_next (curn))
      if (curo->data == curn->data)
	deactivate = g_list_remove (deactivate, curo->data);

  return deactivate;
}

/**
 * update_pipeline:
 * @comp: The #GnlComposition
 * @currenttime: The #GstClockTime to update at, can be GST_CLOCK_TIME_NONE.
 * @initial: TRUE if this is the first setup
 *
 * Updates the internal pipeline and properties. If @currenttime is 
 * GST_CLOCK_TIME_NONE, it will not modify the current pipeline
 *
 * Returns: TRUE if the pipeline was modified.
 */

static gboolean
update_pipeline	(GnlComposition *comp, GstClockTime currenttime, gboolean initial)
{
  gboolean	ret = FALSE;

  GST_DEBUG_OBJECT (comp, "currenttime:%lld", currenttime);

  COMP_OBJECTS_LOCK (comp);

  update_start_stop_duration (comp);

  /* only modify the pipeline if we're in state != PLAYING */
  if ( (GST_CLOCK_TIME_IS_VALID (currenttime)) ) {
    GstState	state = (GST_STATE_NEXT (comp) == GST_STATE_VOID_PENDING) ? GST_STATE (comp) : GST_STATE_NEXT (comp);
    GList	*stack = NULL;
    GList	*deactivate;
    GstPad	*pad = NULL;
    GstClockTime	new_stop;

    GST_DEBUG_OBJECT (comp, "now really updating the pipeline");

    /* rebuild the stack and relink new elements */
    stack = get_clean_toplevel_stack (comp, currenttime, &new_stop);    
    deactivate = compare_relink_stack (comp, stack);
    if (deactivate)
      ret = TRUE;

    /* set new segment_start/stop */
    comp->private->segment_start = currenttime;
    comp->private->segment_stop = new_stop;
    
    COMP_OBJECTS_UNLOCK (comp);

    GST_DEBUG_OBJECT (comp, "De-activating objects no longer used");
    /* state-lock elements no more used */
    while (deactivate) {
      gst_element_set_locked_state (GST_ELEMENT (deactivate->data), TRUE);
      deactivate = g_list_next (deactivate);
    }

    GST_DEBUG_OBJECT (comp, "Finished de-activating objects no longer used");

    /* if toplevel element has changed, redirect ghostpad to it */
    if ((stack) && ((!(comp->private->current)) || (comp->private->current->data != stack->data))) {
      GST_DEBUG_OBJECT (comp, "Top stack object has changed, switching pad");
      pad = get_src_pad (GST_ELEMENT (stack->data));
      if (pad) {
	gnl_composition_ghost_pad_set_target (comp, pad);
	gst_object_unref (pad);
      } else {
	/* The pad might be created dynamically */
      }
    }

    GST_DEBUG_OBJECT (comp, "activating objects in new stack to %s",
		      gst_element_state_get_name (state));

    /* activate new stack */
    comp->private->current = stack;
    while (stack) {
      gst_element_set_locked_state (GST_ELEMENT (stack->data), FALSE);
      gst_element_set_state (GST_ELEMENT (stack->data), state);
      stack = g_list_next (stack);
    }

    GST_DEBUG_OBJECT (comp, "Finished activating objects in new stack");

    if (comp->private->current) {
      pad = get_src_pad (GST_ELEMENT (comp->private->current->data));
      if (pad) {
	GstEvent	*event;
	
	event = get_new_seek_event (comp, initial);
	gst_pad_send_event (pad, event);
	gst_object_unref (pad);
      }
    }
  } else {
    COMP_OBJECTS_UNLOCK (comp);
  }
  return ret;
}

static void
object_start_changed	(GnlObject *object, GParamSpec *arg,
			 GnlComposition *comp)
{
  GST_DEBUG_OBJECT (object, "...");

  comp->private->objects_start = g_list_sort 
    (comp->private->objects_start, 
     (GCompareFunc) objects_start_compare);

  comp->private->objects_stop = g_list_sort 
    (comp->private->objects_stop, 
     (GCompareFunc) objects_stop_compare);
  
  update_pipeline (comp, GST_CLOCK_TIME_NONE, FALSE);
}

static void
object_stop_changed	(GnlObject *object, GParamSpec *arg,
			 GnlComposition *comp)
{
  GST_DEBUG_OBJECT (object, "...");
  
  comp->private->objects_stop = g_list_sort 
    (comp->private->objects_stop, 
     (GCompareFunc) objects_stop_compare);
  
  comp->private->objects_start = g_list_sort 
    (comp->private->objects_start, 
     (GCompareFunc) objects_start_compare);

  update_pipeline (comp, GST_CLOCK_TIME_NONE, FALSE);
}

static void
object_priority_changed	(GnlObject *object, GParamSpec *arg, 
			 GnlComposition *comp)
{
  GST_DEBUG_OBJECT (object, "...");

  comp->private->objects_start = g_list_sort
    (comp->private->objects_start, 
     (GCompareFunc) objects_start_compare);
  
  comp->private->objects_stop = g_list_sort
    (comp->private->objects_stop, 
     (GCompareFunc) objects_stop_compare);
  
  update_pipeline (comp, GST_CLOCK_TIME_NONE, FALSE);
}

static void
object_active_changed	(GnlObject *object, GParamSpec *arg,
			 GnlComposition *comp)
{
  GST_DEBUG_OBJECT (object, "...");

  update_pipeline (comp, GST_CLOCK_TIME_NONE, FALSE);
}

static gboolean
gnl_composition_add_object	(GstBin *bin, GstElement *element)
{
  gboolean	ret;
  GnlCompositionEntry	*entry;
  GnlComposition	*comp = GNL_COMPOSITION (bin);

  GST_DEBUG_OBJECT (bin, "element %s", GST_OBJECT_NAME (element));
  /* we only accept GnlObject */
  g_return_val_if_fail (GNL_IS_OBJECT (element), FALSE);

  COMP_OBJECTS_LOCK (comp);

  gst_object_ref (element);

  gst_element_set_locked_state (element, TRUE);

  ret = GST_BIN_CLASS (parent_class)->add_element (bin, element);
  if (!ret)
    goto beach;

  /* wrap it and add it to the hash table */
  entry = g_new0 (GnlCompositionEntry, 1);
  entry->object = GNL_OBJECT (element);
  entry->starthandler = g_signal_connect (G_OBJECT (element),
					  "notify::start",
					  G_CALLBACK (object_start_changed),
					  comp);
  entry->stophandler = g_signal_connect (G_OBJECT (element),
					  "notify::stop",
					  G_CALLBACK (object_stop_changed),
					  comp);
  entry->priorityhandler = g_signal_connect (G_OBJECT (element),
					  "notify::priority",
					  G_CALLBACK (object_priority_changed),
					  comp);
  entry->activehandler = g_signal_connect (G_OBJECT (element),
					  "notify::active",
					  G_CALLBACK (object_active_changed),
					  comp);
  g_hash_table_insert (comp->private->objects_hash, element, entry);

  /* add it sorted to the objects list */
  comp->private->objects_start = g_list_append
    (comp->private->objects_start,
     element);
  comp->private->objects_start = g_list_sort
    (comp->private->objects_start,
     (GCompareFunc) objects_start_compare);

  if (comp->private->objects_start)
    GST_LOG_OBJECT (comp, "Head of objects_start is now %s",
		    GST_OBJECT_NAME (comp->private->objects_start->data));

  comp->private->objects_stop = g_list_append
    (comp->private->objects_stop,
     element);
  comp->private->objects_stop = g_list_sort
    (comp->private->objects_stop,
     (GCompareFunc) objects_stop_compare);
  
  if (comp->private->objects_stop)
    GST_LOG_OBJECT (comp, "Head of objects_stop is now %s",
		    GST_OBJECT_NAME (comp->private->objects_stop->data));

  /* update pipeline */
  COMP_OBJECTS_UNLOCK (comp);
  update_pipeline (comp, GST_CLOCK_TIME_NONE, FALSE);
  COMP_OBJECTS_LOCK (comp);

 beach:
  gst_object_unref (element);
  COMP_OBJECTS_UNLOCK (comp);
  return ret;
}


static gboolean
gnl_composition_remove_object	(GstBin *bin, GstElement *element)
{
  gboolean	ret;
  GnlComposition	*comp = GNL_COMPOSITION (bin);

  GST_DEBUG_OBJECT (bin, "element %s", GST_OBJECT_NAME (element));
  /* we only accept GnlObject */
  g_return_val_if_fail (GNL_IS_OBJECT (element), FALSE);

  COMP_OBJECTS_LOCK (comp);

  gst_object_ref (element);

  gst_element_set_locked_state (element, FALSE);

  ret = GST_BIN_CLASS (parent_class)->remove_element (bin, element);
  if (!ret)
    goto beach;

  if (!(g_hash_table_remove (comp->private->objects_hash, element)))
    goto beach;

  /* remove it from the objects list and resort the lists */
  comp->private->objects_start = g_list_remove 
    (comp->private->objects_start, element);
  comp->private->objects_start = g_list_sort
    (comp->private->objects_start, 
     (GCompareFunc) objects_start_compare);
  
  comp->private->objects_stop = g_list_remove
    (comp->private->objects_stop, element);
  comp->private->objects_stop = g_list_sort
    (comp->private->objects_stop, 
     (GCompareFunc) objects_stop_compare);

  COMP_OBJECTS_UNLOCK (comp);
  update_pipeline (comp, GST_CLOCK_TIME_NONE, FALSE);
  COMP_OBJECTS_LOCK (comp);

 beach:
  gst_object_unref (element);
  COMP_OBJECTS_UNLOCK (comp);
  return ret;
}



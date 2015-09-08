/* GStreamer
 * Copyright (C) <2005> Julien Moutte <julien@moutte.net>
 *               <2009>,<2010> Stefan Kost <stefan.kost@nokia.com>
 * Copyright (C) 2012, 2013 Samsung Electronics Co., Ltd.
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
 *
 * * Modifications by Samsung Electronics Co., Ltd.
 * 1. Add display related properties
 * 2. Support samsung extension format to improve performance
 * 3. Support video texture overlay of OSP layer
 */

/**
 * SECTION:element-xvimagesink
 *
 * XvImageSink renders video frames to a drawable (XWindow) on a local display
 * using the XVideo extension. Rendering to a remote display is theoretically
 * possible but i doubt that the XVideo extension is actually available when
 * connecting to a remote display. This element can receive a Window ID from the
 * application through the XOverlay interface and will then render video frames
 * in this drawable. If no Window ID was provided by the application, the
 * element will create its own internal window and render into it.
 *
 * <refsect2>
 * <title>Scaling</title>
 * <para>
 * The XVideo extension, when it's available, handles hardware accelerated
 * scaling of video frames. This means that the element will just accept
 * incoming video frames no matter their geometry and will then put them to the
 * drawable scaling them on the fly. Using the #GstXvImageSink:force-aspect-ratio
 * property it is possible to enforce scaling with a constant aspect ratio,
 * which means drawing black borders around the video frame.
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Events</title>
 * <para>
 * XvImageSink creates a thread to handle events coming from the drawable. There
 * are several kind of events that can be grouped in 2 big categories: input
 * events and window state related events. Input events will be translated to
 * navigation events and pushed upstream for other elements to react on them.
 * This includes events such as pointer moves, key press/release, clicks etc...
 * Other events are used to handle the drawable appearance even when the data
 * is not flowing (GST_STATE_PAUSED). That means that even when the element is
 * paused, it will receive expose events from the drawable and draw the latest
 * frame with correct borders/aspect-ratio.
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Pixel aspect ratio</title>
 * <para>
 * When changing state to GST_STATE_READY, XvImageSink will open a connection to
 * the display specified in the #GstXvImageSink:display property or the
 * default display if nothing specified. Once this connection is open it will
 * inspect the display configuration including the physical display geometry and
 * then calculate the pixel aspect ratio. When receiving video frames with a
 * different pixel aspect ratio, XvImageSink will use hardware scaling to
 * display the video frames correctly on display's pixel aspect ratio.
 * Sometimes the calculated pixel aspect ratio can be wrong, it is
 * then possible to enforce a specific pixel aspect ratio using the
 * #GstXvImageSink:pixel-aspect-ratio property.
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch -v videotestsrc ! xvimagesink
 * ]| A pipeline to test hardware scaling.
 * When the test video signal appears you can resize the window and see that
 * video frames are scaled through hardware (no extra CPU cost).
 * |[
 * gst-launch -v videotestsrc ! xvimagesink force-aspect-ratio=true
 * ]| Same pipeline with #GstXvImageSink:force-aspect-ratio property set to true
 * You can observe the borders drawn around the scaled image respecting aspect
 * ratio.
 * |[
 * gst-launch -v videotestsrc ! navigationtest ! xvimagesink
 * ]| A pipeline to test navigation events.
 * While moving the mouse pointer over the test signal you will see a black box
 * following the mouse pointer. If you press the mouse button somewhere on the
 * video and release it somewhere else a green box will appear where you pressed
 * the button and a red one where you released it. (The navigationtest element
 * is part of gst-plugins-good.) You can observe here that even if the images
 * are scaled through hardware the pointer coordinates are converted back to the
 * original video frame geometry so that the box can be drawn to the correct
 * position. This also handles borders correctly, limiting coordinates to the
 * image area
 * |[
 * gst-launch -v videotestsrc ! video/x-raw-yuv, pixel-aspect-ratio=(fraction)4/3 ! xvimagesink
 * ]| This is faking a 4/3 pixel aspect ratio caps on video frames produced by
 * videotestsrc, in most cases the pixel aspect ratio of the display will be
 * 1/1. This means that XvImageSink will have to do the scaling to convert
 * incoming frames to a size that will match the display pixel aspect ratio
 * (from 320x240 to 320x180 in this case). Note that you might have to escape
 * some characters for your shell like '\(fraction\)'.
 * |[
 * gst-launch -v videotestsrc ! xvimagesink hue=100 saturation=-100 brightness=100
 * ]| Demonstrates how to use the colorbalance interface.
 * </refsect2>
 */

/* for developers: there are two useful tools : xvinfo and xvattr */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Our interfaces */
#include <gst/interfaces/navigation.h>
#include <gst/interfaces/xoverlay.h>
#include <gst/interfaces/colorbalance.h>
#include <gst/interfaces/propertyprobe.h>
/* Helper functions */
#include <gst/video/video.h>

/* Object header */
#include "xvimagesink.h"

#ifdef GST_EXT_XV_ENHANCEMENT
/* Samsung extension headers */
/* For xv extension header for buffer transfer (output) */
#include "xv_types.h"

/* headers for drm */
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <X11/Xmd.h>
#include <dri2/dri2.h>
#include <tbm_bufmgr.h>

/* for setting vconf about xv state */
#include <vconf.h>
#include <vconf-internal-player-keys.h>


enum {
    SECURE_PATH_INIT = -1,
    SECURE_PATH_OFF = 0,
    SECURE_PATH_ON = 1
};

enum {
    DRM_LEVEL_0 = 0,
    DRM_LEVEL_1
 };

enum
{
  DISPLAY_STATUS_NULL = 0,
  DISPLAY_STATUS_HDMI_ACTIVE,
  DISPLAY_STATUS_UNKNOWN_ACTIVE,
};

enum
{
  XV_STATUS_NULL = 0,
  XV_STATUS_READY,
  XV_STATUS_PAUSED,
  XV_STATUS_PLAYING,
  XV_STATUS_SEEK,
};

typedef enum {
        BUF_SHARE_METHOD_PADDR = 0,
        BUF_SHARE_METHOD_FD,
        BUF_SHARE_METHOD_TIZEN_BUFFER,
        BUF_SHARE_METHOD_FLUSH_BUFFER
} buf_share_method_t;

#define _CHECK_DISPLAYED_BUFFER_COUNT   15
#define _EVENT_THREAD_CHECK_INTERVAL    15000   /* us */

/* max channel count *********************************************************/
#define SCMN_IMGB_MAX_PLANE         (4)

/* image buffer definition ***************************************************

    +------------------------------------------+ ---
    |                                          |  ^
    |     a[], p[]                             |  |
    |     +---------------------------+ ---    |  |
    |     |                           |  ^     |  |
    |     |<---------- w[] ---------->|  |     |  |
    |     |                           |  |     |  |
    |     |                           |        |
    |     |                           |  h[]   |  e[]
    |     |                           |        |
    |     |                           |  |     |  |
    |     |                           |  |     |  |
    |     |                           |  v     |  |
    |     +---------------------------+ ---    |  |
    |                                          |  v
    +------------------------------------------+ ---

    |<----------------- s[] ------------------>|
*/

typedef struct
{
	/* width of each image plane */
	int w[SCMN_IMGB_MAX_PLANE];
	/* height of each image plane */
	int h[SCMN_IMGB_MAX_PLANE];
	/* stride of each image plane */
	int s[SCMN_IMGB_MAX_PLANE];
	/* elevation of each image plane */
	int e[SCMN_IMGB_MAX_PLANE];
	/* user space address of each image plane */
	void *a[SCMN_IMGB_MAX_PLANE];
	/* physical address of each image plane, if needs */
	void *p[SCMN_IMGB_MAX_PLANE];
	/* color space type of image */
	int cs;
	/* left postion, if needs */
	int x;
	/* top position, if needs */
	int y;
	/* to align memory */
	int __dummy2;
	/* arbitrary data */
	int data[16];
	/* dma buf fd */
	int dmabuf_fd[SCMN_IMGB_MAX_PLANE];
	/* buffer share method */
	int buf_share_method;
	/* Y plane size in case of ST12 */
	int y_size;
	/* UV plane size in case of ST12 */
	int uv_size;
	/* Tizen buffer object */
	void *bo[SCMN_IMGB_MAX_PLANE];
	/* JPEG data */
	void *jpeg_data;
	/* JPEG size */
	int jpeg_size;
	/* TZ memory buffer */
	int tz_enable;
} SCMN_IMGB;

static void _release_flush_buffer(GstXvImageSink *xvimagesink);
static void _remove_last_buffer(GstXvImageSink *xvimagesink);
static gboolean gst_xvimagesink_make_flush_buffer(GstXvImageSink *xvimagesink);
#endif /* GST_EXT_XV_ENHANCEMENT */

/* Debugging category */
#include <gst/gstinfo.h>

#include "gst/glib-compat-private.h"

GST_DEBUG_CATEGORY_STATIC (gst_debug_xvimagesink);
#define GST_CAT_DEFAULT gst_debug_xvimagesink
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

#ifdef GST_EXT_XV_ENHANCEMENT
#define GST_TYPE_XVIMAGESINK_DISPLAY_MODE (gst_xvimagesink_display_mode_get_type())
#define GST_TYPE_XVIMAGESINK_CSC_RANGE (gst_xvimagesink_csc_range_get_type())

static GType
gst_xvimagesink_display_mode_get_type(void)
{
	static GType xvimagesink_display_mode_type = 0;
	static const GEnumValue display_mode_type[] = {
		{ 0, "Default mode", "DEFAULT"},
		{ 1, "Primary video ON and Secondary video FULL SCREEN mode", "PRI_VIDEO_ON_AND_SEC_VIDEO_FULL_SCREEN"},
		{ 2, "Primary video OFF and Secondary video FULL SCREEN mode", "PRI_VIDEO_OFF_AND_SEC_VIDEO_FULL_SCREEN"},
		{ 3, NULL, NULL},
	};

	if (!xvimagesink_display_mode_type) {
		xvimagesink_display_mode_type = g_enum_register_static("GstXVImageSinkDisplayModeType", display_mode_type);
	}

	return xvimagesink_display_mode_type;
}

static GType
gst_xvimagesink_csc_range_get_type(void)
{
	static GType xvimagesink_csc_range_type = 0;
	static const GEnumValue csc_range_type[] = {
		{ 0, "Narrow range", "NARROW"},
		{ 1, "Wide range", "WIDE"},
		{ 2, NULL, NULL},
	};

	if (!xvimagesink_csc_range_type) {
		xvimagesink_csc_range_type = g_enum_register_static("GstXVImageSinkCSCRangeType", csc_range_type);
	}

	return xvimagesink_csc_range_type;
}

enum {
    DEGREE_0,
    DEGREE_90,
    DEGREE_180,
    DEGREE_270,
    DEGREE_NUM,
};

#define GST_TYPE_XVIMAGESINK_ROTATE_ANGLE (gst_xvimagesink_rotate_angle_get_type())

static GType
gst_xvimagesink_rotate_angle_get_type(void)
{
	static GType xvimagesink_rotate_angle_type = 0;
	static const GEnumValue rotate_angle_type[] = {
		{ 0, "No rotate", "DEGREE_0"},
		{ 1, "Rotate 90 degree", "DEGREE_90"},
		{ 2, "Rotate 180 degree", "DEGREE_180"},
		{ 3, "Rotate 270 degree", "DEGREE_270"},
		{ 4, NULL, NULL},
	};

	if (!xvimagesink_rotate_angle_type) {
		xvimagesink_rotate_angle_type = g_enum_register_static("GstXVImageSinkRotateAngleType", rotate_angle_type);
	}

	return xvimagesink_rotate_angle_type;
}

enum {
    DISP_GEO_METHOD_LETTER_BOX = 0,
    DISP_GEO_METHOD_ORIGIN_SIZE,
    DISP_GEO_METHOD_FULL_SCREEN,
    DISP_GEO_METHOD_CROPPED_FULL_SCREEN,
    DISP_GEO_METHOD_ORIGIN_SIZE_OR_LETTER_BOX,
    DISP_GEO_METHOD_CUSTOM_DST_ROI,
    DISP_GEO_METHOD_NUM,
};

enum {
    ROI_DISP_GEO_METHOD_FULL_SCREEN = 0,
    ROI_DISP_GEO_METHOD_LETTER_BOX,
    ROI_DISP_GEO_METHOD_NUM,
};

#define DEF_DISPLAY_GEOMETRY_METHOD         DISP_GEO_METHOD_LETTER_BOX
#define DEF_ROI_DISPLAY_GEOMETRY_METHOD     ROI_DISP_GEO_METHOD_FULL_SCREEN

enum {
    FLIP_NONE = 0,
    FLIP_HORIZONTAL,
    FLIP_VERTICAL,
    FLIP_BOTH,
    FLIP_NUM,
};
#define DEF_DISPLAY_FLIP            FLIP_NONE

#define GST_TYPE_XVIMAGESINK_DISPLAY_GEOMETRY_METHOD (gst_xvimagesink_display_geometry_method_get_type())
#define GST_TYPE_XVIMAGESINK_ROI_DISPLAY_GEOMETRY_METHOD (gst_xvimagesink_roi_display_geometry_method_get_type())
#define GST_TYPE_XVIMAGESINK_FLIP                    (gst_xvimagesink_flip_get_type())

static GType
gst_xvimagesink_display_geometry_method_get_type(void)
{
	static GType xvimagesink_display_geometry_method_type = 0;
	static const GEnumValue display_geometry_method_type[] = {
		{ 0, "Letter box", "LETTER_BOX"},
		{ 1, "Origin size", "ORIGIN_SIZE"},
		{ 2, "Full-screen", "FULL_SCREEN"},
		{ 3, "Cropped full-screen", "CROPPED_FULL_SCREEN"},
		{ 4, "Origin size(if screen size is larger than video size(width/height)) or Letter box(if video size(width/height) is larger than screen size)", "ORIGIN_SIZE_OR_LETTER_BOX"},
		{ 5, "Explicitly described destination ROI", "CUSTOM_DST_ROI"},
		{ 6, NULL, NULL},
	};

	if (!xvimagesink_display_geometry_method_type) {
		xvimagesink_display_geometry_method_type = g_enum_register_static("GstXVImageSinkDisplayGeometryMethodType", display_geometry_method_type);
	}

	return xvimagesink_display_geometry_method_type;
}

static GType
gst_xvimagesink_roi_display_geometry_method_get_type(void)
{
	static GType xvimagesink_roi_display_geometry_method_type = 0;
	static const GEnumValue roi_display_geometry_method_type[] = {
		{ 0, "ROI-Full-screen", "FULL_SCREEN"},
		{ 1, "ROI-Letter box", "LETTER_BOX"},
		{ 2, NULL, NULL},
	};

	if (!xvimagesink_roi_display_geometry_method_type) {
		xvimagesink_roi_display_geometry_method_type = g_enum_register_static("GstXVImageSinkROIDisplayGeometryMethodType", roi_display_geometry_method_type);
	}

	return xvimagesink_roi_display_geometry_method_type;
}

static GType
gst_xvimagesink_flip_get_type(void)
{
	static GType xvimagesink_flip_type = 0;
	static const GEnumValue flip_type[] = {
		{ FLIP_NONE,       "Flip NONE", "FLIP_NONE"},
		{ FLIP_HORIZONTAL, "Flip HORIZONTAL", "FLIP_HORIZONTAL"},
		{ FLIP_VERTICAL,   "Flip VERTICAL", "FLIP_VERTICAL"},
		{ FLIP_BOTH,       "Flip BOTH", "FLIP_BOTH"},
		{ FLIP_NUM, NULL, NULL},
	};

	if (!xvimagesink_flip_type) {
		xvimagesink_flip_type = g_enum_register_static("GstXVImageSinkFlipType", flip_type);
	}

	return xvimagesink_flip_type;
}

#define g_marshal_value_peek_pointer(v)  (v)->data[0].v_pointer
void
gst_xvimagesink_BOOLEAN__POINTER (GClosure         *closure,
                                     GValue         *return_value G_GNUC_UNUSED,
                                     guint          n_param_values,
                                     const GValue   *param_values,
                                     gpointer       invocation_hint G_GNUC_UNUSED,
                                     gpointer       marshal_data)
{
  typedef gboolean (*GMarshalFunc_BOOLEAN__POINTER) (gpointer     data1,
                                                     gpointer     arg_1,
                                                     gpointer     data2);
  register GMarshalFunc_BOOLEAN__POINTER callback;
  register GCClosure *cc = (GCClosure*) closure;
  register gpointer data1, data2;

  gboolean v_return;

  g_return_if_fail (return_value != NULL);
  g_return_if_fail (n_param_values == 2);

  if (G_CCLOSURE_SWAP_DATA (closure)) {
    data1 = closure->data;
    data2 = g_value_peek_pointer (param_values + 0);
  } else {
    data1 = g_value_peek_pointer (param_values + 0);
    data2 = closure->data;
  }
  callback = (GMarshalFunc_BOOLEAN__POINTER) (marshal_data ? marshal_data : cc->callback);

  v_return = callback (data1,
                      g_marshal_value_peek_pointer (param_values + 1),
                      data2);

  g_value_set_boolean (return_value, v_return);
}

enum
{
    SIGNAL_FRAME_RENDER_ERROR,
    SIGNAL_DISPLAY_STATUS,
    SIGNAL_EXTERNAL_RESOLUTION,
    SIGNAL_WINDOW_STATUS,
    SIGNAL_QUICKPANEL_STATUS,
    SIGNAL_MULTIWINDOW_STATUS,
    LAST_SIGNAL
};
static guint gst_xvimagesink_signals[LAST_SIGNAL] = { 0 };

#endif /* GST_EXT_XV_ENHANCEMENT */

typedef struct
{
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
}
MotifWmHints, MwmHints;

#define MWM_HINTS_DECORATIONS   (1L << 1)

#ifdef GST_EXT_XV_ENHANCEMENT
#define MAX_DISPLAY_IN_XVIMAGESINK  20
static Display *g_display_id[MAX_DISPLAY_IN_XVIMAGESINK] = {0};
#endif

static void gst_xvimagesink_reset (GstXvImageSink * xvimagesink);

static GstBufferClass *xvimage_buffer_parent_class = NULL;
static void gst_xvimage_buffer_finalize (GstXvImageBuffer * xvimage);

static void gst_xvimagesink_xwindow_update_geometry (GstXvImageSink *
    xvimagesink);
static gint gst_xvimagesink_get_format_from_caps (GstXvImageSink * xvimagesink,
    GstCaps * caps);
static void gst_xvimagesink_expose (GstXOverlay * overlay);

#ifdef GST_EXT_XV_ENHANCEMENT
static XImage *make_transparent_image(Display *d, Window win, int w, int h);
static gboolean set_display_mode(GstXContext *xcontext, int set_mode);
static gboolean set_csc_range(GstXContext *xcontext, int set_range);
static void gst_xvimagesink_set_pixmap_handle(GstXOverlay *overlay, guintptr id);
static unsigned int drm_convert_dmabuf_gemname(GstXvImageSink *xvimagesink, unsigned int dmabuf_fd, unsigned int *gem_handle);
static void drm_close_gem(GstXvImageSink *xvimagesink, unsigned int *gem_handle);
static void _add_displaying_buffer(GstXvImageSink *xvimagesink, XV_DATA_PTR img_data, GstBuffer *buffer);
static void _remove_displaying_buffer(GstXvImageSink *xvimagesink, unsigned int *gem_name);
static int _is_connected_to_external_display(GstXvImageSink *xvimagesink);
static void check_hdmi_connected(GstXvImageSink *xvimagesink);
static int get_window_prop_card32_property (Display* dpy, Window win, Atom atom, Atom type, unsigned int *val, unsigned int len);
static gboolean check_supportable_port_attr(GstXvImageSink * xvimagesink, gchar* attr_name);
#endif /* GST_EXT_XV_ENHANCEMENT */

/* Default template - initiated with class struct to allow gst-register to work
   without X running */
static GstStaticPadTemplate gst_xvimagesink_sink_template_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw-rgb, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 1, MAX ], "
        "height = (int) [ 1, MAX ]; "
        "video/x-raw-yuv, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]")
    );

enum
{
  PROP_0,
  PROP_CONTRAST,
  PROP_BRIGHTNESS,
  PROP_HUE,
  PROP_SATURATION,
  PROP_DISPLAY,
  PROP_SYNCHRONOUS,
  PROP_PIXEL_ASPECT_RATIO,
  PROP_FORCE_ASPECT_RATIO,
  PROP_HANDLE_EVENTS,
  PROP_DEVICE,
  PROP_DEVICE_NAME,
  PROP_HANDLE_EXPOSE,
  PROP_DOUBLE_BUFFER,
  PROP_AUTOPAINT_COLORKEY,
  PROP_COLORKEY,
  PROP_DRAW_BORDERS,
  PROP_WINDOW_WIDTH,
  PROP_WINDOW_HEIGHT,
#ifdef GST_EXT_XV_ENHANCEMENT
  PROP_DISPLAY_MODE,
  PROP_CSC_RANGE,
  PROP_ROTATE_ANGLE,
  PROP_FLIP,
  PROP_DISPLAY_GEOMETRY_METHOD,
  PROP_VISIBLE,
  PROP_ZOOM,
  PROP_ZOOM_POS_X,
  PROP_ZOOM_POS_Y,
  PROP_ORIENTATION,
  PROP_DST_ROI_MODE,
  PROP_DST_ROI_X,
  PROP_DST_ROI_Y,
  PROP_DST_ROI_W,
  PROP_DST_ROI_H,
  PROP_SRC_CROP_X,
  PROP_SRC_CROP_Y,
  PROP_SRC_CROP_W,
  PROP_SRC_CROP_H,
  PROP_STOP_VIDEO,
  PROP_PIXMAP_CB,
  PROP_PIXMAP_CB_USER_DATA,
  PROP_SUBPICTURE,
  PROP_EXTERNAL_WIDTH,
  PROP_EXTERNAL_HEIGHT,
  PROP_ENABLE_FLUSH_BUFFER,
  PROP_PIXMAP,
  PROP_HIDED_WINDOW,
  PROP_QUICKPANEL_ON,
  PROP_MULTIWINDOW_ACTIVE,
  PROP_KEEP_EXTERNAL_FULLSCREEN_POST,
  PROP_KEEP_EXTERNAL_FULLSCREEN_PREV,
#endif /* GST_EXT_XV_ENHANCEMENT */
};

static void gst_xvimagesink_init_interfaces (GType type);

GST_BOILERPLATE_FULL (GstXvImageSink, gst_xvimagesink, GstVideoSink,
    GST_TYPE_VIDEO_SINK, gst_xvimagesink_init_interfaces);


/* ============================================================= */
/*                                                               */
/*                       Private Methods                         */
/*                                                               */
/* ============================================================= */

/* xvimage buffers */

#define GST_TYPE_XVIMAGE_BUFFER (gst_xvimage_buffer_get_type())

#define GST_IS_XVIMAGE_BUFFER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_XVIMAGE_BUFFER))
#define GST_XVIMAGE_BUFFER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_XVIMAGE_BUFFER, GstXvImageBuffer))
#define GST_XVIMAGE_BUFFER_CAST(obj) ((GstXvImageBuffer *)(obj))

/* This function destroys a GstXvImage handling XShm availability */
static void
gst_xvimage_buffer_destroy (GstXvImageBuffer * xvimage)
{
  GstXvImageSink *xvimagesink;

  GST_DEBUG_OBJECT (xvimage, "Destroying buffer");

  xvimagesink = xvimage->xvimagesink;
  if (G_UNLIKELY (xvimagesink == NULL))
    goto no_sink;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  GST_OBJECT_LOCK (xvimagesink);

  /* If the destroyed image is the current one we destroy our reference too */
  if (xvimagesink->cur_image == xvimage)
    xvimagesink->cur_image = NULL;

  /* We might have some buffers destroyed after changing state to NULL */
  if (xvimagesink->xcontext == NULL) {
    GST_DEBUG_OBJECT (xvimagesink, "Destroying XvImage after Xcontext");
#ifdef HAVE_XSHM
    /* Need to free the shared memory segment even if the x context
     * was already cleaned up */
    if (xvimage->SHMInfo.shmaddr != ((void *) -1)) {
      shmdt (xvimage->SHMInfo.shmaddr);
    }
#endif
    goto beach;
  }

  g_mutex_lock (xvimagesink->x_lock);

#ifdef HAVE_XSHM
  if (xvimagesink->xcontext->use_xshm) {
    if (xvimage->SHMInfo.shmaddr != ((void *) -1)) {
      GST_DEBUG_OBJECT (xvimagesink, "XServer ShmDetaching from 0x%x id 0x%lx",
          xvimage->SHMInfo.shmid, xvimage->SHMInfo.shmseg);
      XShmDetach (xvimagesink->xcontext->disp, &xvimage->SHMInfo);
      XSync (xvimagesink->xcontext->disp, FALSE);

      shmdt (xvimage->SHMInfo.shmaddr);
    }
    if (xvimage->xvimage)
      XFree (xvimage->xvimage);
  } else
#endif /* HAVE_XSHM */
  {
    if (xvimage->xvimage) {
      if (xvimage->xvimage->data) {
        g_free (xvimage->xvimage->data);
      }
      XFree (xvimage->xvimage);
    }
  }

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

beach:
  GST_OBJECT_UNLOCK (xvimagesink);
  xvimage->xvimagesink = NULL;
  gst_object_unref (xvimagesink);

  GST_MINI_OBJECT_CLASS (xvimage_buffer_parent_class)->finalize (GST_MINI_OBJECT
      (xvimage));

  return;

no_sink:
  {
    GST_WARNING ("no sink found");
    return;
  }
}

static void
gst_xvimage_buffer_finalize (GstXvImageBuffer * xvimage)
{
  GstXvImageSink *xvimagesink;
  gboolean running;

  xvimagesink = xvimage->xvimagesink;
  if (G_UNLIKELY (xvimagesink == NULL))
    goto no_sink;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  GST_OBJECT_LOCK (xvimagesink);
  running = xvimagesink->running;
  GST_OBJECT_UNLOCK (xvimagesink);

  /* If our geometry changed we can't reuse that image. */
  if (running == FALSE) {
    GST_LOG_OBJECT (xvimage, "destroy image as sink is shutting down");
    gst_xvimage_buffer_destroy (xvimage);
  } else if ((xvimage->width != xvimagesink->video_width) ||
      (xvimage->height != xvimagesink->video_height)) {
    GST_LOG_OBJECT (xvimage,
        "destroy image as its size changed %dx%d vs current %dx%d",
        xvimage->width, xvimage->height,
        xvimagesink->video_width, xvimagesink->video_height);
    gst_xvimage_buffer_destroy (xvimage);
  } else {
    /* In that case we can reuse the image and add it to our image pool. */
    GST_LOG_OBJECT (xvimage, "recycling image in pool");
    /* need to increment the refcount again to recycle */
    gst_buffer_ref (GST_BUFFER_CAST (xvimage));
    g_mutex_lock (xvimagesink->pool_lock);
    xvimagesink->image_pool = g_slist_prepend (xvimagesink->image_pool,
        xvimage);
    g_mutex_unlock (xvimagesink->pool_lock);
  }
  return;

no_sink:
  {
    GST_WARNING ("no sink found");
    return;
  }
}

static void
gst_xvimage_buffer_free (GstXvImageBuffer * xvimage)
{
  /* make sure it is not recycled */
  xvimage->width = -1;
  xvimage->height = -1;
  gst_buffer_unref (GST_BUFFER (xvimage));
}

static void
gst_xvimage_buffer_init (GstXvImageBuffer * xvimage, gpointer g_class)
{
#ifdef HAVE_XSHM
  xvimage->SHMInfo.shmaddr = ((void *) -1);
  xvimage->SHMInfo.shmid = -1;
#endif
}

static void
gst_xvimage_buffer_class_init (gpointer g_class, gpointer class_data)
{
  GstMiniObjectClass *mini_object_class = GST_MINI_OBJECT_CLASS (g_class);

  xvimage_buffer_parent_class = g_type_class_peek_parent (g_class);

  mini_object_class->finalize = (GstMiniObjectFinalizeFunction)
      gst_xvimage_buffer_finalize;
}

static GType
gst_xvimage_buffer_get_type (void)
{
  static GType _gst_xvimage_buffer_type;

  if (G_UNLIKELY (_gst_xvimage_buffer_type == 0)) {
    static const GTypeInfo xvimage_buffer_info = {
      sizeof (GstBufferClass),
      NULL,
      NULL,
      gst_xvimage_buffer_class_init,
      NULL,
      NULL,
      sizeof (GstXvImageBuffer),
      0,
      (GInstanceInitFunc) gst_xvimage_buffer_init,
      NULL
    };
    _gst_xvimage_buffer_type = g_type_register_static (GST_TYPE_BUFFER,
        "GstXvImageBuffer", &xvimage_buffer_info, 0);
  }
  return _gst_xvimage_buffer_type;
}

/* X11 stuff */

static gboolean error_caught = FALSE;

static int
gst_xvimagesink_handle_xerror (Display * display, XErrorEvent * xevent)
{
  char error_msg[1024];

  XGetErrorText (display, xevent->error_code, error_msg, 1024);
#ifndef GST_EXT_XV_ENHANCEMENT
  GST_DEBUG ("xvimagesink triggered an XError. error: %s, display(%p)", error_msg, display);
#else
  {
    int i = 0;
    for (i = 0; i < MAX_DISPLAY_IN_XVIMAGESINK; i++) {
      if (g_display_id[i] == display) {
        GST_ERROR ("xvimagesink triggered an XError. error: %s, display(%p), xevent->request_code: %d", error_msg, display, xevent->request_code);
        error_caught = TRUE;
        break;
      }
    }
    if (i == MAX_DISPLAY_IN_XVIMAGESINK) {
      GST_ERROR ("xvimagesink triggered an XError. error: %s, xevent->request_code: %d, but the display(%p) was not created in xvimagesink, skip it...", error_msg, xevent->request_code, display);
    }
  }
#endif
  return 0;
}

#ifdef HAVE_XSHM
/* This function checks that it is actually really possible to create an image
   using XShm */
static gboolean
gst_xvimagesink_check_xshm_calls (GstXContext * xcontext)
{
  XvImage *xvimage;
  XShmSegmentInfo SHMInfo;
  gint size;
  int (*handler) (Display *, XErrorEvent *);
  gboolean result = FALSE;
  gboolean did_attach = FALSE;

  g_return_val_if_fail (xcontext != NULL, FALSE);

  /* Sync to ensure any older errors are already processed */
  XSync (xcontext->disp, FALSE);

  /* Set defaults so we don't free these later unnecessarily */
  SHMInfo.shmaddr = ((void *) -1);
  SHMInfo.shmid = -1;

  /* Setting an error handler to catch failure */
  error_caught = FALSE;
  handler = XSetErrorHandler (gst_xvimagesink_handle_xerror);

  /* Trying to create a 1x1 picture */
  GST_DEBUG ("XvShmCreateImage of 1x1");
  xvimage = XvShmCreateImage (xcontext->disp, xcontext->xv_port_id,
      xcontext->im_format, NULL, 1, 1, &SHMInfo);

  /* Might cause an error, sync to ensure it is noticed */
  XSync (xcontext->disp, FALSE);
  if (!xvimage || error_caught) {
    GST_WARNING ("could not XvShmCreateImage a 1x1 image");
    goto beach;
  }
  size = xvimage->data_size;

  SHMInfo.shmid = shmget (IPC_PRIVATE, size, IPC_CREAT | 0777);
  if (SHMInfo.shmid == -1) {
    GST_WARNING ("could not get shared memory of %d bytes", size);
    goto beach;
  }

  SHMInfo.shmaddr = shmat (SHMInfo.shmid, NULL, 0);
  if (SHMInfo.shmaddr == ((void *) -1)) {
    GST_WARNING ("Failed to shmat: %s", g_strerror (errno));
    /* Clean up the shared memory segment */
    shmctl (SHMInfo.shmid, IPC_RMID, NULL);
    goto beach;
  }

  xvimage->data = SHMInfo.shmaddr;
  SHMInfo.readOnly = FALSE;

  if (XShmAttach (xcontext->disp, &SHMInfo) == 0) {
    GST_WARNING ("Failed to XShmAttach");
    /* Clean up the shared memory segment */
    shmctl (SHMInfo.shmid, IPC_RMID, NULL);
    goto beach;
  }

  /* Sync to ensure we see any errors we caused */
  XSync (xcontext->disp, FALSE);

  /* Delete the shared memory segment as soon as everyone is attached.
   * This way, it will be deleted as soon as we detach later, and not
   * leaked if we crash. */
  shmctl (SHMInfo.shmid, IPC_RMID, NULL);

  if (!error_caught) {
    GST_DEBUG ("XServer ShmAttached to 0x%x, id 0x%lx", SHMInfo.shmid,
        SHMInfo.shmseg);

    did_attach = TRUE;
    /* store whether we succeeded in result */
    result = TRUE;
  } else {
    GST_WARNING ("MIT-SHM extension check failed at XShmAttach. "
        "Not using shared memory.");
  }

beach:
  /* Sync to ensure we swallow any errors we caused and reset error_caught */
  XSync (xcontext->disp, FALSE);

  error_caught = FALSE;
  XSetErrorHandler (handler);

  if (did_attach) {
    GST_DEBUG ("XServer ShmDetaching from 0x%x id 0x%lx",
        SHMInfo.shmid, SHMInfo.shmseg);
    XShmDetach (xcontext->disp, &SHMInfo);
    XSync (xcontext->disp, FALSE);
  }
  if (SHMInfo.shmaddr != ((void *) -1))
    shmdt (SHMInfo.shmaddr);
  if (xvimage)
    XFree (xvimage);
  return result;
}
#endif /* HAVE_XSHM */

/* This function handles GstXvImage creation depending on XShm availability */
static GstXvImageBuffer *
gst_xvimagesink_xvimage_new (GstXvImageSink * xvimagesink, GstCaps * caps)
{
  GstXvImageBuffer *xvimage = NULL;
  GstStructure *structure = NULL;
  gboolean succeeded = FALSE;
  int (*handler) (Display *, XErrorEvent *);

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  if (caps == NULL)
    return NULL;

  xvimage = (GstXvImageBuffer *) gst_mini_object_new (GST_TYPE_XVIMAGE_BUFFER);
  GST_DEBUG_OBJECT (xvimage, "Creating new XvImageBuffer");

  structure = gst_caps_get_structure (caps, 0);

  if (!gst_structure_get_int (structure, "width", &xvimage->width) ||
      !gst_structure_get_int (structure, "height", &xvimage->height)) {
    GST_WARNING ("failed getting geometry from caps %" GST_PTR_FORMAT, caps);
  }

  GST_LOG_OBJECT (xvimagesink, "creating %dx%d", xvimage->width,
      xvimage->height);
#ifdef GST_EXT_XV_ENHANCEMENT
  GST_LOG_OBJECT(xvimagesink, "aligned size %dx%d",
                               xvimagesink->aligned_width, xvimagesink->aligned_height);
  if (xvimagesink->aligned_width == 0 || xvimagesink->aligned_height == 0) {
    GST_INFO_OBJECT(xvimagesink, "aligned size is zero. set size of caps.");
    xvimagesink->aligned_width = xvimage->width;
    xvimagesink->aligned_height = xvimage->height;
  }
  if (xvimagesink->subpicture) {
    GST_LOG("because of subpicture's format, pass xvimage_new");
    return xvimage;
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  xvimage->im_format = gst_xvimagesink_get_format_from_caps (xvimagesink, caps);
  if (xvimage->im_format == -1) {
    GST_WARNING_OBJECT (xvimagesink, "failed to get format from caps %"
        GST_PTR_FORMAT, caps);
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
        ("Failed to create output image buffer of %dx%d pixels",
            xvimage->width, xvimage->height), ("Invalid input caps"));
    goto beach_unlocked;
  }
  xvimage->xvimagesink = gst_object_ref (xvimagesink);

  g_mutex_lock (xvimagesink->x_lock);

#ifdef GST_EXT_XV_ENHANCEMENT
  XSync (xvimagesink->xcontext->disp, FALSE);
#endif /* GST_EXT_XV_ENHANCEMENT */
  /* Setting an error handler to catch failure */
  error_caught = FALSE;
  handler = XSetErrorHandler (gst_xvimagesink_handle_xerror);

#ifdef HAVE_XSHM
  if (xvimagesink->xcontext->use_xshm) {
    int expected_size;

    xvimage->xvimage = XvShmCreateImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimage->im_format, NULL,
#ifdef GST_EXT_XV_ENHANCEMENT
        xvimagesink->aligned_width, xvimagesink->aligned_height, &xvimage->SHMInfo);
#else /* GST_EXT_XV_ENHANCEMENT */
        xvimage->width, xvimage->height, &xvimage->SHMInfo);
#endif /* GST_EXT_XV_ENHANCEMENT */
    if (!xvimage->xvimage || error_caught) {
      g_mutex_unlock (xvimagesink->x_lock);

      /* Reset error flag */
      error_caught = FALSE;

      /* Push a warning */
      GST_ELEMENT_WARNING (xvimagesink, RESOURCE, WRITE,
          ("Failed to create output image buffer of %dx%d pixels",
              xvimage->width, xvimage->height),
          ("could not XvShmCreateImage a %dx%d image",
              xvimage->width, xvimage->height));

#ifdef GST_EXT_XV_ENHANCEMENT
      /* must not change "use_xshm",
         because it causes memory curruption when buffer created by XvShmCreateImage is destroyed */
      goto beach_unlocked;
#else /* GST_EXT_XV_ENHANCEMENT */
      /* Retry without XShm */
      xvimagesink->xcontext->use_xshm = FALSE;

      /* Hold X mutex again to try without XShm */
      g_mutex_lock (xvimagesink->x_lock);
      goto no_xshm;
#endif /* GST_EXT_XV_ENHANCEMENT */
    }

    /* we have to use the returned data_size for our shm size */
    xvimage->size = xvimage->xvimage->data_size;
    GST_LOG_OBJECT (xvimagesink, "XShm image size is %" G_GSIZE_FORMAT,
        xvimage->size);

    /* calculate the expected size.  This is only for sanity checking the
     * number we get from X. */
    switch (xvimage->im_format) {
      case GST_MAKE_FOURCC ('I', '4', '2', '0'):
      case GST_MAKE_FOURCC ('Y', 'V', '1', '2'):
      {
        gint pitches[3];
        gint offsets[3];
        guint plane;

        offsets[0] = 0;
        pitches[0] = GST_ROUND_UP_4 (xvimage->width);
        offsets[1] = offsets[0] + pitches[0] * GST_ROUND_UP_2 (xvimage->height);
        pitches[1] = GST_ROUND_UP_8 (xvimage->width) / 2;
        offsets[2] =
            offsets[1] + pitches[1] * GST_ROUND_UP_2 (xvimage->height) / 2;
        pitches[2] = GST_ROUND_UP_8 (pitches[0]) / 2;

        expected_size =
            offsets[2] + pitches[2] * GST_ROUND_UP_2 (xvimage->height) / 2;

        for (plane = 0; plane < xvimage->xvimage->num_planes; plane++) {
          GST_DEBUG_OBJECT (xvimagesink,
              "Plane %u has a expected pitch of %d bytes, " "offset of %d",
              plane, pitches[plane], offsets[plane]);
        }
        break;
      }
      case GST_MAKE_FOURCC ('Y', 'U', 'Y', '2'):
      case GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'):
        expected_size = xvimage->height * GST_ROUND_UP_4 (xvimage->width * 2);
        break;

#ifdef GST_EXT_XV_ENHANCEMENT
      case GST_MAKE_FOURCC ('S', 'T', '1', '2'):
      case GST_MAKE_FOURCC ('S', 'N', '1', '2'):
      case GST_MAKE_FOURCC ('S', 'N', '2', '1'):
      case GST_MAKE_FOURCC ('S', 'U', 'Y', 'V'):
      case GST_MAKE_FOURCC ('S', 'U', 'Y', '2'):
      case GST_MAKE_FOURCC ('S', '4', '2', '0'):
      case GST_MAKE_FOURCC ('S', 'Y', 'V', 'Y'):
        expected_size = sizeof(SCMN_IMGB);
        break;
#endif /* GST_EXT_XV_ENHANCEMENT */
      default:
        expected_size = 0;
        break;
    }
    if (expected_size != 0 && xvimage->size != expected_size) {
      GST_WARNING_OBJECT (xvimagesink,
          "unexpected XShm image size (got %" G_GSIZE_FORMAT ", expected %d)",
          xvimage->size, expected_size);
    }

    /* Be verbose about our XvImage stride */
    {
      guint plane;

      for (plane = 0; plane < xvimage->xvimage->num_planes; plane++) {
        GST_DEBUG_OBJECT (xvimagesink, "Plane %u has a pitch of %d bytes, "
            "offset of %d", plane, xvimage->xvimage->pitches[plane],
            xvimage->xvimage->offsets[plane]);
      }
    }

    xvimage->SHMInfo.shmid = shmget (IPC_PRIVATE, xvimage->size,
        IPC_CREAT | 0777);
    if (xvimage->SHMInfo.shmid == -1) {
      g_mutex_unlock (xvimagesink->x_lock);
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
          ("Failed to create output image buffer of %dx%d pixels",
              xvimage->width, xvimage->height),
          ("could not get shared memory of %" G_GSIZE_FORMAT " bytes",
              xvimage->size));
      goto beach_unlocked;
    }

    xvimage->SHMInfo.shmaddr = shmat (xvimage->SHMInfo.shmid, NULL, 0);
    if (xvimage->SHMInfo.shmaddr == ((void *) -1)) {
      g_mutex_unlock (xvimagesink->x_lock);
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
          ("Failed to create output image buffer of %dx%d pixels",
              xvimage->width, xvimage->height),
          ("Failed to shmat: %s", g_strerror (errno)));
      /* Clean up the shared memory segment */
      shmctl (xvimage->SHMInfo.shmid, IPC_RMID, NULL);
      goto beach_unlocked;
    }

    xvimage->xvimage->data = xvimage->SHMInfo.shmaddr;
    xvimage->SHMInfo.readOnly = FALSE;

    if (XShmAttach (xvimagesink->xcontext->disp, &xvimage->SHMInfo) == 0) {
      /* Clean up the shared memory segment */
      shmctl (xvimage->SHMInfo.shmid, IPC_RMID, NULL);

      g_mutex_unlock (xvimagesink->x_lock);
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
          ("Failed to create output image buffer of %dx%d pixels",
              xvimage->width, xvimage->height), ("Failed to XShmAttach"));
      goto beach_unlocked;
    }

    XSync (xvimagesink->xcontext->disp, FALSE);

    /* Delete the shared memory segment as soon as we everyone is attached.
     * This way, it will be deleted as soon as we detach later, and not
     * leaked if we crash. */
    shmctl (xvimage->SHMInfo.shmid, IPC_RMID, NULL);

    GST_DEBUG_OBJECT (xvimagesink, "XServer ShmAttached to 0x%x, id 0x%lx",
        xvimage->SHMInfo.shmid, xvimage->SHMInfo.shmseg);
  } else
#ifndef GST_EXT_XV_ENHANCEMENT
  no_xshm:
#endif /* GST_EXT_XV_ENHANCEMENT */
#endif /* HAVE_XSHM */
  {
    xvimage->xvimage = XvCreateImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
#ifdef GST_EXT_XV_ENHANCEMENT
        xvimage->im_format, NULL, xvimagesink->aligned_width, xvimagesink->aligned_height);
#else /* GST_EXT_XV_ENHANCEMENT */
        xvimage->im_format, NULL, xvimage->width, xvimage->height);
#endif /* GST_EXT_XV_ENHANCEMENT */
    if (!xvimage->xvimage || error_caught) {
      g_mutex_unlock (xvimagesink->x_lock);
      /* Reset error handler */
      error_caught = FALSE;
      XSetErrorHandler (handler);
      /* Push an error */
      GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
          ("Failed to create outputimage buffer of %dx%d pixels",
              xvimage->width, xvimage->height),
          ("could not XvCreateImage a %dx%d image",
              xvimage->width, xvimage->height));
      goto beach_unlocked;
    }

    /* we have to use the returned data_size for our image size */
    xvimage->size = xvimage->xvimage->data_size;
    xvimage->xvimage->data = g_malloc (xvimage->size);

    XSync (xvimagesink->xcontext->disp, FALSE);
  }

  /* Reset error handler */
  error_caught = FALSE;
  XSetErrorHandler (handler);

  succeeded = TRUE;

  GST_BUFFER_DATA (xvimage) = (guchar *) xvimage->xvimage->data;
  GST_BUFFER_SIZE (xvimage) = xvimage->size;

  g_mutex_unlock (xvimagesink->x_lock);

beach_unlocked:
  if (!succeeded) {
    gst_xvimage_buffer_free (xvimage);
    xvimage = NULL;
  }

  return xvimage;
}

/* We are called with the x_lock taken */
static void
gst_xvimagesink_xwindow_draw_borders (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow, GstVideoRectangle rect)
{
  gint t1, t2;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));
  g_return_if_fail (xwindow != NULL);

  XSetForeground (xvimagesink->xcontext->disp, xwindow->gc,
      xvimagesink->xcontext->black);

  /* Left border */
  if (rect.x > xvimagesink->render_rect.x) {
    XFillRectangle (xvimagesink->xcontext->disp, xwindow->win, xwindow->gc,
        xvimagesink->render_rect.x, xvimagesink->render_rect.y,
        rect.x - xvimagesink->render_rect.x, xvimagesink->render_rect.h);
  }

  /* Right border */
  t1 = rect.x + rect.w;
  t2 = xvimagesink->render_rect.x + xvimagesink->render_rect.w;
  if (t1 < t2) {
    XFillRectangle (xvimagesink->xcontext->disp, xwindow->win, xwindow->gc,
        t1, xvimagesink->render_rect.y, t2 - t1, xvimagesink->render_rect.h);
  }

  /* Top border */
  if (rect.y > xvimagesink->render_rect.y) {
    XFillRectangle (xvimagesink->xcontext->disp, xwindow->win, xwindow->gc,
        xvimagesink->render_rect.x, xvimagesink->render_rect.y,
        xvimagesink->render_rect.w, rect.y - xvimagesink->render_rect.y);
  }

  /* Bottom border */
  t1 = rect.y + rect.h;
  t2 = xvimagesink->render_rect.y + xvimagesink->render_rect.h;
  if (t1 < t2) {
    XFillRectangle (xvimagesink->xcontext->disp, xwindow->win, xwindow->gc,
        xvimagesink->render_rect.x, t1, xvimagesink->render_rect.w, t2 - t1);
  }
}

/* This function puts a GstXvImage on a GstXvImageSink's window. Returns FALSE
 * if no window was available  */
static gboolean
gst_xvimagesink_xvimage_put (GstXvImageSink * xvimagesink,
    GstXvImageBuffer * xvimage)
{
  GstVideoRectangle result;
  gboolean draw_border = FALSE;

#ifdef GST_EXT_XV_ENHANCEMENT
  static Atom atom_rotation = None;
  static Atom atom_hflip = None;
  static Atom atom_vflip = None;
  static Atom atom_contents_rotation = None;
  gboolean set_hflip = FALSE;
  gboolean set_vflip = FALSE;
  XWindowAttributes map_attr;

  GstVideoRectangle src_origin = { 0, 0, 0, 0};
  GstVideoRectangle src_input  = { 0, 0, 0, 0};
  GstVideoRectangle src = { 0, 0, 0, 0};
  GstVideoRectangle dst = { 0, 0, 0, 0};

  gint res_rotate_angle = 0;
  int rotate        = 0;
  int orientation = 0;
  int ret           = 0;
  int idx           = 0;
  int (*handler) (Display *, XErrorEvent *) = NULL;
  gboolean res = FALSE;
  XV_DATA_PTR img_data = NULL;

  if (xvimagesink->is_subpicture_format)
    return TRUE;
#endif /* GST_EXT_XV_ENHANCEMENT */

  /* We take the flow_lock. If expose is in there we don't want to run
     concurrently from the data flow thread */
  g_mutex_lock (xvimagesink->flow_lock);

#ifdef GST_EXT_XV_ENHANCEMENT
  if (xvimagesink->xid_updated) {
    if (xvimage && xvimagesink->xvimage == NULL) {
      GST_WARNING_OBJECT (xvimagesink, "set xvimage to NULL, new xid was set right after creation of new xvimage");
      xvimage = NULL;
    }
    xvimagesink->xid_updated = FALSE;
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  if (G_UNLIKELY (xvimagesink->xwindow == NULL)) {
#ifdef GST_EXT_XV_ENHANCEMENT
    if (xvimagesink->get_pixmap_cb) {
      GST_INFO_OBJECT( xvimagesink, "xwindow is NULL, but it has get_pixmap_cb(0x%x), keep going..",xvimagesink->get_pixmap_cb );
    } else {
      GST_INFO_OBJECT( xvimagesink, "xwindow is NULL. Skip xvimage_put." );
      g_mutex_unlock(xvimagesink->flow_lock);
      return FALSE;
    }
#else /* GST_EXT_XV_ENHANCEMENT */
    g_mutex_unlock (xvimagesink->flow_lock);
    return FALSE;
#endif /* GST_EXT_XV_ENHANCEMENT */
  }

#ifdef GST_EXT_XV_ENHANCEMENT
  /* check map status of window */
  if(!xvimagesink->is_pixmap) {
    if (xvimagesink->xcontext && xvimagesink->xwindow) {
      if (XGetWindowAttributes(xvimagesink->xcontext->disp, xvimagesink->xwindow->win, &map_attr)) {
        if (map_attr.map_state != IsViewable) {
          GST_WARNING_OBJECT(xvimagesink, "window is unmapped, skip putimage");
          g_mutex_unlock(xvimagesink->flow_lock);
          return TRUE;
        }
      } else {
      GST_WARNING_OBJECT(xvimagesink, "XGetWindowAttributes failed");
      }
    }
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  /* Draw borders when displaying the first frame. After this
     draw borders only on expose event or after a size change. */
  if (!xvimagesink->cur_image || xvimagesink->redraw_border) {
    draw_border = TRUE;
  }

  /* Store a reference to the last image we put, lose the previous one */
  if (xvimage && xvimagesink->cur_image != xvimage) {
    if (xvimagesink->cur_image) {
      GST_LOG_OBJECT (xvimagesink, "unreffing %p", xvimagesink->cur_image);
      gst_buffer_unref (GST_BUFFER_CAST (xvimagesink->cur_image));
    }
    GST_LOG_OBJECT (xvimagesink, "reffing %p as our current image", xvimage);
    xvimagesink->cur_image =
        GST_XVIMAGE_BUFFER_CAST (gst_buffer_ref (GST_BUFFER_CAST (xvimage)));
  }

  /* Expose sends a NULL image, we take the latest frame */
  if (!xvimage) {
#ifdef GST_EXT_XV_ENHANCEMENT
    g_mutex_lock(xvimagesink->display_buffer_lock);
    if (xvimagesink->is_zero_copy_format  &&
        xvimagesink->displaying_buffer_count < 1) {
      g_mutex_unlock(xvimagesink->display_buffer_lock);
      g_mutex_unlock (xvimagesink->flow_lock);
      GST_WARNING_OBJECT(xvimagesink, "no buffer to display. skip putimage");
      return TRUE;
    }
    g_mutex_unlock(xvimagesink->display_buffer_lock);
#endif /* GST_EXT_XV_ENHANCEMENT */
    if (xvimagesink->cur_image) {
      draw_border = TRUE;
      xvimage = xvimagesink->cur_image;
    } else {
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING_OBJECT(xvimagesink, "cur_image is NULL. Skip xvimage_put.");
      /* no need to release gem handle */
#endif /* GST_EXT_XV_ENHANCEMENT */
      g_mutex_unlock (xvimagesink->flow_lock);
      return TRUE;
    }
  }

#ifdef GST_EXT_XV_ENHANCEMENT
  gboolean enable_last_buffer;
  g_object_get(G_OBJECT(xvimagesink), "enable-last-buffer", &enable_last_buffer, NULL);
  if(enable_last_buffer) {
    if (xvimagesink->cur_image && xvimagesink->is_zero_copy_format) {
      GstBuffer *last_buffer= NULL;
      g_object_get(G_OBJECT(xvimagesink), "last-buffer", &last_buffer, NULL);
      if (last_buffer) {
        if(last_buffer != xvimagesink->cur_image)
        {
          GST_LOG("curimage : %p , last_buffer : %p", xvimagesink->cur_image, last_buffer);
        }
      } else {
        GST_WARNING_OBJECT(xvimagesink, "zero copy format and last buffer is NULL, so skip this putimage");
        g_mutex_unlock (xvimagesink->flow_lock);
        return TRUE;
      }
      gst_buffer_unref(last_buffer);
      last_buffer = NULL;
    }
  }

  if (xvimagesink->visible == FALSE ||
      (xvimagesink->is_hided && GST_STATE(xvimagesink) != GST_STATE_PLAYING)) {
    GST_INFO("visible[%d] or is_hided[%d]. Skip xvimage_put.",
             xvimagesink->visible, xvimagesink->is_hided);
    g_mutex_unlock(xvimagesink->flow_lock);
    return TRUE;
  }

  if (!xvimagesink->get_pixmap_cb) {
    gst_xvimagesink_xwindow_update_geometry( xvimagesink );
    if (!xvimagesink->subpicture && !xvimagesink->is_during_seek) {
      if(xvimagesink->is_multi_window && (GST_STATE(xvimagesink) != GST_STATE_PLAYING)) {
        set_display_mode(xvimagesink->xcontext, DISPLAY_MODE_PRI_VIDEO_ON_AND_SEC_VIDEO_CLONE);
      } else {
        set_display_mode(xvimagesink->xcontext, xvimagesink->display_mode);
      }
    }
  } else {
    /* for multi-pixmap usage for the video texture */
    gst_xvimagesink_set_pixmap_handle ((GstXOverlay *)xvimagesink, xvimagesink->get_pixmap_cb(xvimagesink->get_pixmap_cb_user_data));
    idx = xvimagesink->current_pixmap_idx;
    if (idx == -1) {
      g_mutex_unlock (xvimagesink->flow_lock);
      return FALSE;
    } else if (idx == -2) {
      GST_WARNING_OBJECT(xvimagesink, "Skip putImage()");
      g_mutex_unlock (xvimagesink->flow_lock);
      return TRUE;
    }
  }

  res_rotate_angle = xvimagesink->rotate_angle;

  src.x = src.y = 0;
  src_origin.x = src_origin.y = src_input.x = src_input.y = 0;

  src_input.w = src_origin.w = xvimagesink->video_width;
  src_input.h = src_origin.h = xvimagesink->video_height;

  if ((xvimagesink->display_geometry_method == DISP_GEO_METHOD_LETTER_BOX ||
      xvimagesink->display_geometry_method == DISP_GEO_METHOD_FULL_SCREEN ||
      xvimagesink->display_geometry_method == DISP_GEO_METHOD_CUSTOM_DST_ROI) &&
      xvimagesink->src_crop.w && xvimagesink->src_crop.h) {
      GST_LOG_OBJECT(xvimagesink, "set src crop %d,%d,%dx%d -> %d,%d,%dx%d",
                                  src_input.x, src_input.y, src_input.w, src_input.h,
                                  xvimagesink->src_crop.x, xvimagesink->src_crop.y, xvimagesink->src_crop.w, xvimagesink->src_crop.h);
      src_input.x = src_origin.w = xvimagesink->src_crop.x;
      src_input.y = src_origin.y = xvimagesink->src_crop.y;
      src_input.w = src_origin.w = xvimagesink->src_crop.w;
      src_input.h = src_origin.h = xvimagesink->src_crop.h;
  }

  if (xvimagesink->rotate_angle == DEGREE_0 ||
      xvimagesink->rotate_angle == DEGREE_180) {
    src.w = src_origin.w;
    src.h = src_origin.h;
  } else {
    src.w = src_origin.h;
    src.h = src_origin.w;
  }

  dst.w = xvimagesink->render_rect.w;
  dst.h = xvimagesink->render_rect.h;

  switch (xvimagesink->display_geometry_method)
  {
    case DISP_GEO_METHOD_LETTER_BOX:
      gst_video_sink_center_rect (src, dst, &result, TRUE);
      result.x += xvimagesink->render_rect.x;
      result.y += xvimagesink->render_rect.y;
      break;

    case DISP_GEO_METHOD_ORIGIN_SIZE:
      gst_video_sink_center_rect (src, dst, &result, FALSE);
      gst_video_sink_center_rect (dst, src, &src_input, FALSE);

      if (xvimagesink->rotate_angle == DEGREE_90 ||
          xvimagesink->rotate_angle == DEGREE_270) {
        src_input.x = src_input.x ^ src_input.y;
        src_input.y = src_input.x ^ src_input.y;
        src_input.x = src_input.x ^ src_input.y;

        src_input.w = src_input.w ^ src_input.h;
        src_input.h = src_input.w ^ src_input.h;
        src_input.w = src_input.w ^ src_input.h;
      }
      break;

   case DISP_GEO_METHOD_FULL_SCREEN:
      result.x = result.y = 0;
      if (!xvimagesink->get_pixmap_cb) {
        result.w = xvimagesink->xwindow->width;
        result.h = xvimagesink->xwindow->height;
      } else {
        result.w = xvimagesink->xpixmap[idx]->width;
        result.h = xvimagesink->xpixmap[idx]->height;
      }
      break;

   case DISP_GEO_METHOD_CROPPED_FULL_SCREEN:
      gst_video_sink_center_rect(dst, src, &src_input, TRUE);

      result.x = result.y = 0;
      result.w = dst.w;
      result.h = dst.h;

      if (xvimagesink->rotate_angle == DEGREE_90 ||
          xvimagesink->rotate_angle == DEGREE_270) {
        src_input.x = src_input.x ^ src_input.y;
        src_input.y = src_input.x ^ src_input.y;
        src_input.x = src_input.x ^ src_input.y;

        src_input.w = src_input.w ^ src_input.h;
        src_input.h = src_input.w ^ src_input.h;
        src_input.w = src_input.w ^ src_input.h;
      }
      break;

   case DISP_GEO_METHOD_ORIGIN_SIZE_OR_LETTER_BOX:
     if (src.w > dst.w || src.h > dst.h) {
       /* LETTER BOX */
       gst_video_sink_center_rect (src, dst, &result, TRUE);
       result.x += xvimagesink->render_rect.x;
       result.y += xvimagesink->render_rect.y;
     } else {
       /* ORIGIN SIZE */
       gst_video_sink_center_rect (src, dst, &result, FALSE);
       gst_video_sink_center_rect (dst, src, &src_input, FALSE);

       if (xvimagesink->rotate_angle == DEGREE_90 ||
           xvimagesink->rotate_angle == DEGREE_270) {
         src_input.x = src_input.x ^ src_input.y;
         src_input.y = src_input.x ^ src_input.y;
         src_input.x = src_input.x ^ src_input.y;

         src_input.w = src_input.w ^ src_input.h;
         src_input.h = src_input.w ^ src_input.h;
         src_input.w = src_input.w ^ src_input.h;
       }
     }
     break;

    case DISP_GEO_METHOD_CUSTOM_DST_ROI:
    {
      GstVideoRectangle dst_roi_cmpns;
      dst_roi_cmpns.w = xvimagesink->dst_roi.w;
      dst_roi_cmpns.h = xvimagesink->dst_roi.h;
      dst_roi_cmpns.x = xvimagesink->dst_roi.x;
      dst_roi_cmpns.y = xvimagesink->dst_roi.y;

      /* setting for DST ROI mode */
      switch (xvimagesink->dst_roi_mode) {
      case ROI_DISP_GEO_METHOD_FULL_SCREEN:
        break;
      case ROI_DISP_GEO_METHOD_LETTER_BOX:
       {
        GstVideoRectangle roi_result;
        if (xvimagesink->orientation == DEGREE_0 ||
            xvimagesink->orientation == DEGREE_180) {
          src.w = src_origin.w;
          src.h = src_origin.h;
        } else {
          src.w = src_origin.h;
          src.h = src_origin.w;
        }
        dst.w = xvimagesink->dst_roi.w;
        dst.h = xvimagesink->dst_roi.h;

        gst_video_sink_center_rect (src, dst, &roi_result, TRUE);
        dst_roi_cmpns.w = roi_result.w;
        dst_roi_cmpns.h = roi_result.h;
        dst_roi_cmpns.x = xvimagesink->dst_roi.x + roi_result.x;
        dst_roi_cmpns.y = xvimagesink->dst_roi.y + roi_result.y;
       }
        break;
      default:
        break;
      }

      /* calculating coordinates according to rotation angle for DST ROI */
      switch (xvimagesink->rotate_angle) {
      case DEGREE_90:
        result.w = dst_roi_cmpns.h;
        result.h = dst_roi_cmpns.w;

        result.x = dst_roi_cmpns.y;
        if (!xvimagesink->get_pixmap_cb) {
          result.y = xvimagesink->xwindow->height - dst_roi_cmpns.x - dst_roi_cmpns.w;
        } else {
          result.y = xvimagesink->xpixmap[idx]->height - dst_roi_cmpns.x - dst_roi_cmpns.w;
        }
        break;
      case DEGREE_180:
        result.w = dst_roi_cmpns.w;
        result.h = dst_roi_cmpns.h;

        if (!xvimagesink->get_pixmap_cb) {
          result.x = xvimagesink->xwindow->width - result.w - dst_roi_cmpns.x;
          result.y = xvimagesink->xwindow->height - result.h - dst_roi_cmpns.y;
        } else {
          result.x = xvimagesink->xpixmap[idx]->width - result.w - dst_roi_cmpns.x;
          result.y = xvimagesink->xpixmap[idx]->height - result.h - dst_roi_cmpns.y;
        }
        break;
      case DEGREE_270:
        result.w = dst_roi_cmpns.h;
        result.h = dst_roi_cmpns.w;

        if (!xvimagesink->get_pixmap_cb) {
          result.x = xvimagesink->xwindow->width - dst_roi_cmpns.y - dst_roi_cmpns.h;
        } else {
          result.x = xvimagesink->xpixmap[idx]->width - dst_roi_cmpns.y - dst_roi_cmpns.h;
        }
        result.y = dst_roi_cmpns.x;
        break;
      default:
        result.x = dst_roi_cmpns.x;
        result.y = dst_roi_cmpns.y;
        result.w = dst_roi_cmpns.w;
        result.h = dst_roi_cmpns.h;
        break;
      }

      /* orientation setting for auto rotation in DST ROI */
      if (xvimagesink->orientation) {
        res_rotate_angle = (xvimagesink->rotate_angle - xvimagesink->orientation);
        if (res_rotate_angle < 0) {
          res_rotate_angle += DEGREE_NUM;
        }
        GST_LOG_OBJECT(xvimagesink, "changing rotation value internally by ROI orientation[%d] : rotate[%d->%d]",
                     xvimagesink->orientation, xvimagesink->rotate_angle, res_rotate_angle);
      }

      GST_LOG_OBJECT(xvimagesink, "rotate[%d], dst ROI: orientation[%d], mode[%d], input[%d,%d,%dx%d]->result[%d,%d,%dx%d]",
                     xvimagesink->rotate_angle, xvimagesink->orientation, xvimagesink->dst_roi_mode,
                     xvimagesink->dst_roi.x, xvimagesink->dst_roi.y, xvimagesink->dst_roi.w, xvimagesink->dst_roi.h,
                     result.x, result.y, result.w, result.h);
      break;
    }
    default:
      break;
  }

  if (xvimagesink->zoom > 1.0 && xvimagesink->zoom <= 9.0) {
    GST_LOG_OBJECT(xvimagesink, "before zoom[%lf], src_input[x:%d,y:%d,w:%d,h:%d]",
                   xvimagesink->zoom, src_input.x, src_input.y, src_input.w, src_input.h);
    gint default_offset_x = 0;
    gint default_offset_y = 0;
    gfloat w = (gfloat)src_input.w;
    gfloat h = (gfloat)src_input.h;
    if (xvimagesink->orientation == DEGREE_0 ||
        xvimagesink->orientation == DEGREE_180) {
      default_offset_x = ((gint)(w - (w/xvimagesink->zoom)))>>1;
      default_offset_y = ((gint)(h - (h/xvimagesink->zoom)))>>1;
    } else {
      default_offset_y = ((gint)(w - (w/xvimagesink->zoom)))>>1;
      default_offset_x = ((gint)(h - (h/xvimagesink->zoom)))>>1;
    }
    GST_LOG_OBJECT(xvimagesink, "default offset x[%d] y[%d], orientation[%d]", default_offset_x, default_offset_y, xvimagesink->orientation);
    if (xvimagesink->zoom_pos_x == -1) {
      src_input.x += default_offset_x;
    } else {
      if (xvimagesink->orientation == DEGREE_0 ||
          xvimagesink->orientation == DEGREE_180) {
        if ((w/xvimagesink->zoom) > w - xvimagesink->zoom_pos_x) {
          xvimagesink->zoom_pos_x = w - (w/xvimagesink->zoom);
        }
        src_input.x += xvimagesink->zoom_pos_x;
      } else {
        if ((h/xvimagesink->zoom) > h - xvimagesink->zoom_pos_x) {
          xvimagesink->zoom_pos_x = h - (h/xvimagesink->zoom);
        }
        src_input.y += (h - h/xvimagesink->zoom) - xvimagesink->zoom_pos_x;
      }
    }
    if (xvimagesink->zoom_pos_y == -1) {
      src_input.y += default_offset_y;
    } else {
      if (xvimagesink->orientation == DEGREE_0 ||
          xvimagesink->orientation == DEGREE_180) {
        if ((h/xvimagesink->zoom) > h - xvimagesink->zoom_pos_y) {
          xvimagesink->zoom_pos_y = h - (h/xvimagesink->zoom);
        }
        src_input.y += xvimagesink->zoom_pos_y;
      } else {
        if ((w/xvimagesink->zoom) > w - xvimagesink->zoom_pos_y) {
          xvimagesink->zoom_pos_y = w - (w/xvimagesink->zoom);
        }
        src_input.x += (xvimagesink->zoom_pos_y);
      }
    }
    src_input.w = (gint)(w/xvimagesink->zoom);
    src_input.h = (gint)(h/xvimagesink->zoom);
    GST_LOG_OBJECT(xvimagesink, "after zoom[%lf], src_input[x:%d,y:%d,w:%d,h%d], zoom_pos[x:%d,y:%d]",
                   xvimagesink->zoom, src_input.x, src_input.y, src_input.w, src_input.h, xvimagesink->zoom_pos_x, xvimagesink->zoom_pos_y);
  }

#else /* GST_EXT_XV_ENHANCEMENT */
  if (xvimagesink->keep_aspect) {
    GstVideoRectangle src, dst;

    /* We use the calculated geometry from _setcaps as a source to respect
       source and screen pixel aspect ratios. */
    src.w = GST_VIDEO_SINK_WIDTH (xvimagesink);
    src.h = GST_VIDEO_SINK_HEIGHT (xvimagesink);
    dst.w = xvimagesink->render_rect.w;
    dst.h = xvimagesink->render_rect.h;

    gst_video_sink_center_rect (src, dst, &result, TRUE);
    result.x += xvimagesink->render_rect.x;
    result.y += xvimagesink->render_rect.y;
  } else {
    memcpy (&result, &xvimagesink->render_rect, sizeof (GstVideoRectangle));
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_mutex_lock (xvimagesink->x_lock);

#ifdef GST_EXT_XV_ENHANCEMENT
  if (draw_border && xvimagesink->draw_borders && !xvimagesink->get_pixmap_cb) {
#else
  if (draw_border && xvimagesink->draw_borders) {
#endif /* GST_EXT_XV_ENHANCEMENT */
    gst_xvimagesink_xwindow_draw_borders (xvimagesink, xvimagesink->xwindow,
        result);
    xvimagesink->redraw_border = FALSE;
  }

  /* We scale to the window's geometry */
#ifdef HAVE_XSHM
  if (xvimagesink->xcontext->use_xshm) {
    GST_LOG_OBJECT (xvimagesink,
        "XvShmPutImage with image %dx%d and window %dx%d, from xvimage %"
        GST_PTR_FORMAT,
        xvimage->width, xvimage->height,
        xvimagesink->render_rect.w, xvimagesink->render_rect.h, xvimage);

#ifdef GST_EXT_XV_ENHANCEMENT
    switch( res_rotate_angle )
    {
      /* There's slightly weired code (CCW? CW?) */
      case DEGREE_0:
        break;
      case DEGREE_90:
        rotate = 270;
        break;
      case DEGREE_180:
        rotate = 180;
        break;
      case DEGREE_270:
        rotate = 90;
        break;
      default:
        GST_WARNING_OBJECT( xvimagesink, "Unsupported rotation [%d]... set DEGREE 0.",
          res_rotate_angle );
        break;
    }

    /* Trim as proper size */
    if (src_input.w % 2 == 1) {
        src_input.w += 1;
    }
    if (src_input.h % 2 == 1) {
        src_input.h += 1;
    }

    if (!xvimagesink->get_pixmap_cb) {
      GST_LOG_OBJECT( xvimagesink, "screen[%dx%d],window[%d,%d,%dx%d],method[%d],rotate[%d],zoom[%f],dp_mode[%d],src[%dx%d],dst[%d,%d,%dx%d],input[%d,%d,%dx%d],result[%d,%d,%dx%d]",
        xvimagesink->scr_w, xvimagesink->scr_h,
        xvimagesink->xwindow->x, xvimagesink->xwindow->y, xvimagesink->xwindow->width, xvimagesink->xwindow->height,
        xvimagesink->display_geometry_method, rotate, xvimagesink->zoom, xvimagesink->display_mode,
        src_origin.w, src_origin.h,
        dst.x, dst.y, dst.w, dst.h,
        src_input.x, src_input.y, src_input.w, src_input.h,
        result.x, result.y, result.w, result.h );
    } else {
      GST_LOG_OBJECT( xvimagesink, "pixmap[%d,%d,%dx%d],method[%d],rotate[%d],zoom[%f],dp_mode[%d],src[%dx%d],dst[%d,%d,%dx%d],input[%d,%d,%dx%d],result[%d,%d,%dx%d]",
      xvimagesink->xpixmap[idx]->x, xvimagesink->xpixmap[idx]->y, xvimagesink->xpixmap[idx]->width, xvimagesink->xpixmap[idx]->height,
      xvimagesink->display_geometry_method, rotate, xvimagesink->zoom, xvimagesink->display_mode,
      src_origin.w, src_origin.h,
      dst.x, dst.y, dst.w, dst.h,
      src_input.x, src_input.y, src_input.w, src_input.h,
      result.x, result.y, result.w, result.h );
    }
    /* skip request to render if dst rect is 0 */
    if (xvimagesink->is_zero_copy_format && (!result.w || !result.h)) {
      GST_WARNING_OBJECT(xvimagesink, "result.w[%d] or result.h[%d] is 0. Skip xvimage_put.", result.w, result.h);
      g_mutex_unlock(xvimagesink->x_lock);
      g_mutex_unlock(xvimagesink->flow_lock);
      return TRUE;
    }

    static gboolean is_exist = FALSE;
    gchar *attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_ROTATION");
    is_exist = check_supportable_port_attr(xvimagesink, attr_name);
    g_free(attr_name);

    if (is_exist) {
      /* set display rotation */
      if (atom_rotation == None) {
        atom_rotation = XInternAtom(xvimagesink->xcontext->disp,
                                  "_USER_WM_PORT_ATTRIBUTE_ROTATION", False);
      }
      ret = XvSetPortAttribute(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id,
                               atom_rotation, rotate);
      if (ret != Success) {
        GST_ERROR_OBJECT( xvimagesink, "XvSetPortAttribute failed[%d]. disp[%x],xv_port_id[%d],atom[%x],rotate[%d]",
          ret, xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_rotation, rotate );

        g_mutex_unlock(xvimagesink->x_lock);
        g_mutex_unlock(xvimagesink->flow_lock);
        return FALSE;
      }
    } else {
      GST_WARNING("_USER_WM_PORT_ATTRIBUTE_ROTATION is not existed");
    }

    switch( xvimagesink->orientation )
    {
      case DEGREE_0:
        orientation = 0;
        break;
      case DEGREE_90:
        orientation = 90;
        break;
      case DEGREE_180:
        orientation = 180;
        break;
      case DEGREE_270:
        orientation = 270;
        break;
      default:
        GST_WARNING_OBJECT( xvimagesink, "Unsupported orientation [%d]...",
          xvimagesink->orientation );
        break;
    }

    is_exist = FALSE;
    attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_CONTENTS_ROTATION");
    is_exist = check_supportable_port_attr(xvimagesink, attr_name);
    g_free(attr_name);

    if (is_exist) {
      /* set contents rotation for connecting with external display */
      if (atom_contents_rotation == None) {
        atom_contents_rotation = XInternAtom(xvimagesink->xcontext->disp,
                                 "_USER_WM_PORT_ATTRIBUTE_CONTENTS_ROTATION", False);
      }
      ret = XvSetPortAttribute(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id,
                               atom_contents_rotation, orientation);
      if (ret != Success) {
        GST_ERROR_OBJECT( xvimagesink, "XvSetPortAttribute failed[%d]. disp[%x],xv_port_id[%d],atom[%x],orientation[%d]",
          ret, xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_contents_rotation, orientation );

        g_mutex_unlock(xvimagesink->x_lock);
        g_mutex_unlock(xvimagesink->flow_lock);
        return FALSE;
      }
    }else {
      GST_WARNING("_USER_WM_PORT_ATTRIBUTE_CONTENTS_ROTATION is not existed");
    }

    switch (xvimagesink->flip) {
    case FLIP_HORIZONTAL:
      set_hflip = TRUE;
      set_vflip = FALSE;
      break;
    case FLIP_VERTICAL:
      set_hflip = FALSE;
      set_vflip = TRUE;
      break;
    case FLIP_BOTH:
      set_hflip = TRUE;
      set_vflip = TRUE;
      break;
    case FLIP_NONE:
    default:
      set_hflip = FALSE;
      set_vflip = FALSE;
      break;
    }

    GST_LOG("set HFLIP %d, VFLIP %d", set_hflip, set_vflip);

    is_exist = FALSE;
    attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_HFLIP");
    is_exist = check_supportable_port_attr(xvimagesink, attr_name);
    g_free(attr_name);

    if (is_exist) {
      if (atom_hflip == None) {
        /* set display flip */
        atom_hflip = XInternAtom(xvimagesink->xcontext->disp,
                                 "_USER_WM_PORT_ATTRIBUTE_HFLIP", False);
    }
      ret = XvSetPortAttribute(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_hflip, set_hflip);
      if (ret != Success) {
        GST_WARNING("set HFLIP failed[%d]. disp[%x],xv_port_id[%d],atom[%x],hflip[%d]",
                    ret, xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_hflip, set_hflip);
      }
    }

    is_exist = FALSE;
    attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_VFLIP");
    is_exist = check_supportable_port_attr(xvimagesink, attr_name);
    g_free(attr_name);

    if (is_exist) {
      if (atom_vflip == None) {
        /* set display flip */
        atom_vflip = XInternAtom(xvimagesink->xcontext->disp,
                                 "_USER_WM_PORT_ATTRIBUTE_VFLIP", False);
       }
      ret = XvSetPortAttribute(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_vflip, set_vflip);
      if (ret != Success) {
        GST_WARNING("set VFLIP failed[%d]. disp[%x],xv_port_id[%d],atom[%x],vflip[%d]",
                    ret, xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_vflip, set_vflip);
      }
    }

    /* set error handler */
    error_caught = FALSE;
    handler = XSetErrorHandler(gst_xvimagesink_handle_xerror);

    /* src input indicates the status when degree is 0 */
    /* dst input indicates the area that src will be shown regardless of rotate */
    if (xvimagesink->visible &&
      ((!xvimagesink->is_hided) || (xvimagesink->is_hided && (GST_STATE(xvimagesink) == GST_STATE_PLAYING)))) {
      if (xvimagesink->xim_transparenter) {
        GST_LOG_OBJECT( xvimagesink, "Transparent related issue." );
        XPutImage(xvimagesink->xcontext->disp,
          xvimagesink->xwindow->win,
          xvimagesink->xwindow->gc,
          xvimagesink->xim_transparenter,
          0, 0,
          result.x, result.y, result.w, result.h);
      }

      /* store buffer */
      if (xvimagesink->is_zero_copy_format && xvimage->xvimage->data) {
        img_data = (XV_DATA_PTR)xvimage->xvimage->data;
        if (img_data->BufType == XV_BUF_TYPE_DMABUF) {
          _add_displaying_buffer(xvimagesink, img_data, xvimage->current_buffer);
          xvimage->current_buffer = NULL;
        }
      }

      g_mutex_lock(xvimagesink->display_buffer_lock);
      if (xvimagesink->displaying_buffer_count > 3) {
        GST_WARNING("too many buffers are not released. [displaying_buffer_count %d]", xvimagesink->displaying_buffer_count);
      }
      g_mutex_unlock(xvimagesink->display_buffer_lock);

      if (xvimagesink->get_pixmap_cb) {
        gint idx = xvimagesink->current_pixmap_idx;
        ret = XvShmPutImage (xvimagesink->xcontext->disp,
          xvimagesink->xcontext->xv_port_id,
          xvimagesink->xpixmap[idx]->pixmap,
          xvimagesink->xpixmap[idx]->gc, xvimage->xvimage,
          src_input.x, src_input.y, src_input.w, src_input.h,
          result.x, result.y, result.w, result.h, FALSE);
        GST_LOG_OBJECT( xvimagesink, "XvShmPutImage return value [%d], disp[0x%x], gc[0x%x], pixmap id[%d], idx[%d]", ret, xvimagesink->xcontext->disp, xvimagesink->xpixmap[idx]->gc, xvimagesink->xpixmap[idx]->pixmap, idx);
      } else {
        ret = XvShmPutImage (xvimagesink->xcontext->disp,
          xvimagesink->xcontext->xv_port_id,
          xvimagesink->xwindow->win,
          xvimagesink->xwindow->gc, xvimage->xvimage,
          src_input.x, src_input.y, src_input.w, src_input.h,
          result.x, result.y, result.w, result.h, FALSE);
        GST_LOG_OBJECT( xvimagesink, "XvShmPutImage return value [%d], disp[0x%x], gc[0x%x], xid[%d]", ret, xvimagesink->xcontext->disp, xvimagesink->xwindow->gc, xvimagesink->xwindow->win);
      }
    } else {
      GST_LOG_OBJECT( xvimagesink, "visible is FALSE. skip this image..." );
    }
#else /* GST_EXT_XV_ENHANCEMENT */
    XvShmPutImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimagesink->xwindow->win,
        xvimagesink->xwindow->gc, xvimage->xvimage,
        xvimagesink->disp_x, xvimagesink->disp_y,
        xvimagesink->disp_width, xvimagesink->disp_height,
        result.x, result.y, result.w, result.h, FALSE);
#endif /* GST_EXT_XV_ENHANCEMENT */
  } else
#endif /* HAVE_XSHM */
  {
    XvPutImage (xvimagesink->xcontext->disp,
        xvimagesink->xcontext->xv_port_id,
        xvimagesink->xwindow->win,
        xvimagesink->xwindow->gc, xvimage->xvimage,
        xvimagesink->disp_x, xvimagesink->disp_y,
        xvimagesink->disp_width, xvimagesink->disp_height,
        result.x, result.y, result.w, result.h);
  }

  XSync (xvimagesink->xcontext->disp, FALSE);

#ifdef HAVE_XSHM
#ifdef GST_EXT_XV_ENHANCEMENT
  if (ret || error_caught || xvimagesink->get_pixmap_cb) {
    GST_DEBUG("error or pixmap_cb");

    if (ret || error_caught) {
      GST_WARNING("putimage error : ret %d, error_caught %d, pixmap cb %p, displaying buffer count %d",
                  ret, error_caught, xvimagesink->get_pixmap_cb, xvimagesink->displaying_buffer_count);

      if (xvimagesink->get_pixmap_cb) {
        g_signal_emit (G_OBJECT (xvimagesink),
                       gst_xvimagesink_signals[SIGNAL_FRAME_RENDER_ERROR],
                       0,
                       &xvimagesink->xpixmap[idx]->pixmap,
                       &res);
      }
    }

    /* release gem handle */
    if (img_data && img_data->BufType == XV_BUF_TYPE_DMABUF) {
      unsigned int gem_name[XV_BUF_PLANE_NUM] = { 0, };
      gem_name[0] = img_data->YBuf;
      gem_name[1] = img_data->CbBuf;
      gem_name[2] = img_data->CrBuf;
      _remove_displaying_buffer(xvimagesink, gem_name);
    }
  }

  /* Reset error handler */
  if (handler) {
    error_caught = FALSE;
    XSetErrorHandler (handler);
  }
#endif /* GST_EXT_XV_ENHANCEMENT */
#endif /* HAVE_XSHM */

  g_mutex_unlock (xvimagesink->x_lock);

  g_mutex_unlock (xvimagesink->flow_lock);

  return TRUE;
}

static gboolean
gst_xvimagesink_xwindow_decorate (GstXvImageSink * xvimagesink,
    GstXWindow * window)
{
  Atom hints_atom = None;
  MotifWmHints *hints;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), FALSE);
  g_return_val_if_fail (window != NULL, FALSE);

  g_mutex_lock (xvimagesink->x_lock);

  hints_atom = XInternAtom (xvimagesink->xcontext->disp, "_MOTIF_WM_HINTS",
      True);
  if (hints_atom == None) {
    g_mutex_unlock (xvimagesink->x_lock);
    return FALSE;
  }

  hints = g_malloc0 (sizeof (MotifWmHints));

  hints->flags |= MWM_HINTS_DECORATIONS;
  hints->decorations = 1 << 0;

  XChangeProperty (xvimagesink->xcontext->disp, window->win,
      hints_atom, hints_atom, 32, PropModeReplace,
      (guchar *) hints, sizeof (MotifWmHints) / sizeof (long));

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  g_free (hints);

  return TRUE;
}

static void
gst_xvimagesink_xwindow_set_title (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow, const gchar * media_title)
{
  if (media_title) {
    g_free (xvimagesink->media_title);
    xvimagesink->media_title = g_strdup (media_title);
  }
  if (xwindow) {
    /* we have a window */
    if (xwindow->internal) {
      XTextProperty xproperty;
      const gchar *app_name;
      const gchar *title = NULL;
      gchar *title_mem = NULL;

      /* set application name as a title */
      app_name = g_get_application_name ();

      if (app_name && xvimagesink->media_title) {
        title = title_mem = g_strconcat (xvimagesink->media_title, " : ",
            app_name, NULL);
      } else if (app_name) {
        title = app_name;
      } else if (xvimagesink->media_title) {
        title = xvimagesink->media_title;
      }

      if (title) {
        if ((XStringListToTextProperty (((char **) &title), 1,
                    &xproperty)) != 0) {
          XSetWMName (xvimagesink->xcontext->disp, xwindow->win, &xproperty);
          XFree (xproperty.value);
        }

        g_free (title_mem);
      }
    }
  }
}

#ifdef GST_EXT_XV_ENHANCEMENT
static XImage *make_transparent_image(Display *d, Window win, int w, int h)
{
  XImage *xim;

  /* create a normal ximage */
  xim = XCreateImage(d, DefaultVisualOfScreen(DefaultScreenOfDisplay(d)),  24, ZPixmap, 0, NULL, w, h, 32, 0);

  GST_INFO("ximage %p", xim);

  /* allocate data for it */
  if (xim) {
    xim->data = (char *)malloc(xim->bytes_per_line * xim->height);
    if (xim->data) {
      memset(xim->data, 0x00, xim->bytes_per_line * xim->height);
      return xim;
    } else {
      GST_ERROR("failed to alloc data - size %d", xim->bytes_per_line * xim->height);
    }

    XDestroyImage(xim);
  }

  GST_ERROR("failed to create Ximage");

  return NULL;
}


static gboolean set_display_mode(GstXContext *xcontext, int set_mode)
{
  int ret = 0;
  static gboolean is_exist = FALSE;
  static XvPortID current_port_id = -1;
  Atom atom_output = None;

  if (xcontext == NULL) {
    GST_WARNING("xcontext is NULL");
    return FALSE;
  }

  /* check once per one xv_port_id */
  if (current_port_id != xcontext->xv_port_id) {
    /* check whether _USER_WM_PORT_ATTRIBUTE_OUTPUT attribute is existed */
    int i = 0;
    int count = 0;
    XvAttribute *const attr = XvQueryPortAttributes(xcontext->disp,
                                                    xcontext->xv_port_id, &count);
    if (attr) {
      current_port_id = xcontext->xv_port_id;
      for (i = 0 ; i < count ; i++) {
        if (!strcmp(attr[i].name, "_USER_WM_PORT_ATTRIBUTE_OUTPUT")) {
          is_exist = TRUE;
          GST_INFO("_USER_WM_PORT_ATTRIBUTE_OUTPUT[index %d] found", i);
          break;
        }
      }
      XFree(attr);
    } else {
      GST_WARNING("XvQueryPortAttributes disp:%d, port_id:%d failed",
                  xcontext->disp, xcontext->xv_port_id);
    }
  }

  if (is_exist) {
    GST_DEBUG("set display mode %d", set_mode);
    atom_output = XInternAtom(xcontext->disp,
                              "_USER_WM_PORT_ATTRIBUTE_OUTPUT", False);
    ret = XvSetPortAttribute(xcontext->disp, xcontext->xv_port_id,
                             atom_output, set_mode);
    if (ret == Success) {
      return TRUE;
    } else {
      GST_WARNING("display mode[%d] set failed.", set_mode);
    }
  } else {
    GST_WARNING("_USER_WM_PORT_ATTRIBUTE_OUTPUT is not existed");
  }

  return FALSE;
}


static gboolean set_csc_range(GstXContext *xcontext, int set_range)
{
  int ret = 0;
  static gboolean is_exist = FALSE;
  static XvPortID current_port_id = -1;
  Atom atom_csc_range = None;

  if (xcontext == NULL) {
    GST_WARNING("xcontext is NULL");
    return FALSE;
  }

  /* check once per one xv_port_id */
  if (current_port_id != xcontext->xv_port_id) {
    /* check whether _USER_WM_PORT_ATTRIBUTE_OUTPUT attribute is existed */
    int i = 0;
    int count = 0;
    XvAttribute *const attr = XvQueryPortAttributes(xcontext->disp,
                                                    xcontext->xv_port_id, &count);
    if (attr) {
      current_port_id = xcontext->xv_port_id;
      for (i = 0 ; i < count ; i++) {
        if (!strcmp(attr[i].name, "_USER_WM_PORT_ATTRIBUTE_CSC_RANGE")) {
          is_exist = TRUE;
          GST_INFO("_USER_WM_PORT_ATTRIBUTE_OUTPUT[index %d] found", i);
          break;
        }
      }
      XFree(attr);
    } else {
      GST_WARNING("XvQueryPortAttributes disp:%d, port_id:%d failed",
                  xcontext->disp, xcontext->xv_port_id);
    }
  }

  if (is_exist) {
    GST_WARNING("set csc range %d", set_range);
    atom_csc_range = XInternAtom(xcontext->disp,
                                 "_USER_WM_PORT_ATTRIBUTE_CSC_RANGE", False);
    ret = XvSetPortAttribute(xcontext->disp, xcontext->xv_port_id,
                             atom_csc_range, set_range);
    if (ret == Success) {
      return TRUE;
    } else {
      GST_WARNING("csc range[%d] set failed.", set_range);
    }
  } else {
    GST_WARNING("_USER_WM_PORT_ATTRIBUTE_CSC_RANGE is not existed");
  }

  return FALSE;
}


static void drm_init(GstXvImageSink *xvimagesink)
{
	Display *dpy;
	int eventBase = 0;
	int errorBase = 0;
	int dri2Major = 0;
	int dri2Minor = 0;
	char *driverName = NULL;
	char *deviceName = NULL;
	struct drm_auth auth_arg = {0};

	xvimagesink->drm_fd = -1;

	dpy = XOpenDisplay(0);
	if (!dpy) {
		GST_ERROR("XOpenDisplay failed errno:%d", errno);
		return;
	}

	GST_INFO("START");

	/* DRI2 */
	if (!DRI2QueryExtension(dpy, &eventBase, &errorBase)) {
		GST_ERROR("DRI2QueryExtension !!");
		goto DRM_INIT_ERROR;
	}

	if (!DRI2QueryVersion(dpy, &dri2Major, &dri2Minor)) {
		GST_ERROR("DRI2QueryVersion !!");
		goto DRM_INIT_ERROR;
	}

	if (!DRI2Connect(dpy, RootWindow(dpy, DefaultScreen(dpy)), &driverName, &deviceName)) {
		GST_ERROR("DRI2Connect !!");
		goto DRM_INIT_ERROR;
	}

	if (!driverName || !deviceName) {
		GST_ERROR("driverName or deviceName is not valid");
		goto DRM_INIT_ERROR;
	}

	GST_INFO("Open drm device : %s", deviceName);

	/* get the drm_fd though opening the deviceName */
	xvimagesink->drm_fd = open(deviceName, O_RDWR);
	if (xvimagesink->drm_fd < 0) {
		GST_ERROR("cannot open drm device (%s)", deviceName);
		goto DRM_INIT_ERROR;
	}

	/* get magic from drm to authentication */
	if (ioctl(xvimagesink->drm_fd, DRM_IOCTL_GET_MAGIC, &auth_arg)) {
		GST_ERROR("cannot get drm auth magic [drm fd %d]", xvimagesink->drm_fd);
		goto DRM_INIT_ERROR;
	}

	if (!DRI2Authenticate(dpy, RootWindow(dpy, DefaultScreen(dpy)), auth_arg.magic)) {
		GST_ERROR("cannot get drm authentication from X");
		goto DRM_INIT_ERROR;
	}

	XCloseDisplay(dpy);
	free(driverName);
	free(deviceName);

	xvimagesink->bufmgr = tbm_bufmgr_init(xvimagesink->drm_fd);
	if (xvimagesink->bufmgr == NULL) {
		GST_ERROR_OBJECT(xvimagesink, "tbm_bufmgr_init failed");
		goto DRM_INIT_ERROR;
	}

	GST_INFO("DONE");

	return;

DRM_INIT_ERROR:
	if (xvimagesink->drm_fd >= 0) {
		close(xvimagesink->drm_fd);
		xvimagesink->drm_fd = -1;
	}
	if (dpy) {
		XCloseDisplay(dpy);
	}
	if (driverName) {
		free(driverName);
	}
	if (deviceName) {
		free(deviceName);
	}

	return;
}

static void drm_fini(GstXvImageSink *xvimagesink)
{
	GST_WARNING_OBJECT(xvimagesink, "START");

	if (xvimagesink->drm_fd >= 0) {
		int i = 0;
		int j = 0;

		/* close remained gem handle */
		g_mutex_lock(xvimagesink->display_buffer_lock);
		for (i = 0 ; i < DISPLAYING_BUFFERS_MAX_NUM ; i++) {
			if (xvimagesink->displaying_buffers[i].gem_name[0] > 0) {
				GST_WARNING_OBJECT(xvimagesink, "remained buffer %p, name %u %u %u, handle %u %u %u",
				            xvimagesink->displaying_buffers[i].buffer,
				            xvimagesink->displaying_buffers[i].gem_name[0],
				            xvimagesink->displaying_buffers[i].gem_name[1],
				            xvimagesink->displaying_buffers[i].gem_name[2],
				            xvimagesink->displaying_buffers[i].gem_handle[0],
				            xvimagesink->displaying_buffers[i].gem_handle[1],
				            xvimagesink->displaying_buffers[i].gem_handle[2]);

				/* release flush buffer */
				if (xvimagesink->flush_buffer) {
					if (xvimagesink->flush_buffer->gem_name[0] == xvimagesink->displaying_buffers[i].gem_name[0] &&
					    xvimagesink->flush_buffer->gem_name[1] == xvimagesink->displaying_buffers[i].gem_name[1] &&
					    xvimagesink->flush_buffer->gem_name[2] == xvimagesink->displaying_buffers[i].gem_name[2]) {
						_release_flush_buffer(xvimagesink);
					}
				} else {
					GST_WARNING_OBJECT(xvimagesink, "Force Unref buffer");
				}

				for (j = 0 ; j < XV_BUF_PLANE_NUM ; j++) {
					if (xvimagesink->displaying_buffers[i].gem_handle[j] > 0) {
						drm_close_gem(xvimagesink, &(xvimagesink->displaying_buffers[i].gem_handle[j]));
					}
					xvimagesink->displaying_buffers[i].gem_name[j] = 0;
					xvimagesink->displaying_buffers[i].dmabuf_fd[j] = 0;
					xvimagesink->displaying_buffers[i].bo[j] = NULL;
				}

				if (xvimagesink->displaying_buffers[i].buffer) {
					gst_buffer_unref(xvimagesink->displaying_buffers[i].buffer);
					xvimagesink->displaying_buffers[i].buffer = NULL;
				}
			}
		}
		g_mutex_unlock(xvimagesink->display_buffer_lock);

		GST_WARNING_OBJECT(xvimagesink, "destroy tbm buffer manager");
		tbm_bufmgr_deinit(xvimagesink->bufmgr);
		xvimagesink->bufmgr = NULL;

		GST_WARNING_OBJECT(xvimagesink, "close drm_fd %d", xvimagesink->drm_fd);
		close(xvimagesink->drm_fd);
		xvimagesink->drm_fd = -1;
	} else {
		GST_WARNING_OBJECT(xvimagesink, "DRM device is NOT opened");
	}

	GST_WARNING_OBJECT(xvimagesink, "DONE");
}

static unsigned int drm_convert_dmabuf_gemname(GstXvImageSink *xvimagesink, unsigned int dmabuf_fd, unsigned int *gem_handle)
{
	int ret = 0;

	struct drm_prime_handle prime_arg = {0,};
	struct drm_gem_flink flink_arg = {0,};

	if (!xvimagesink || !gem_handle) {
		GST_ERROR("handle[%p,%p] is NULL", xvimagesink, gem_handle);
		return 0;
	}

	if (xvimagesink->drm_fd < 0) {
		GST_ERROR("DRM is not opened");
		return 0;
	}

	if (dmabuf_fd <= 0) {
		GST_LOG("Ignore wrong dmabuf fd [%u]", dmabuf_fd);
		return 0;
	}

	prime_arg.fd = dmabuf_fd;
	ret = ioctl(xvimagesink->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_arg);
	if (ret) {
		GST_ERROR("DRM_IOCTL_PRIME_FD_TO_HANDLE failed. ret %d, dmabuf fd : %u [drm fd %d]",
		          ret, dmabuf_fd, xvimagesink->drm_fd);
		return 0;
	}

	*gem_handle = prime_arg.handle;
	flink_arg.handle = prime_arg.handle;
	ret = ioctl(xvimagesink->drm_fd, DRM_IOCTL_GEM_FLINK, &flink_arg);
	if (ret) {
		GST_ERROR("DRM_IOCTL_GEM_FLINK failed. ret %d, gem_handle %u, gem_name %u [drm fd %d]",
		          ret, *gem_handle, flink_arg.name, xvimagesink->drm_fd);
		return 0;
	}

	return flink_arg.name;
}

static void drm_close_gem(GstXvImageSink *xvimagesink, unsigned int *gem_handle)
{
	struct drm_gem_close close_arg = {0,};

	if (xvimagesink->drm_fd < 0 || !gem_handle) {
		GST_ERROR("DRM is not opened");
		return;
	}

	if (*gem_handle <= 0) {
		GST_DEBUG("invalid gem handle %u", *gem_handle);
		return;
	}

	GST_LOG("Call DRM_IOCTL_GEM_CLOSE - handle %u", *gem_handle);

	close_arg.handle = *gem_handle;
	if (ioctl(xvimagesink->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg)) {
		GST_ERROR("cannot close drm gem handle %u [drm fd %d]", *gem_handle, xvimagesink->drm_fd);
		return;
	}

	*gem_handle = 0;

	return;
}


static void _remove_last_buffer(GstXvImageSink *xvimagesink)
{
  gboolean enable_last_buffer = FALSE;

  if (xvimagesink == NULL) {
    GST_ERROR("handle is NULL");
    return;
  }

  /* get enable-last-buffer */
  g_object_get(G_OBJECT(xvimagesink), "enable-last-buffer", &enable_last_buffer, NULL);

  GST_WARNING_OBJECT(xvimagesink, "current enable-last-buffer : %d", enable_last_buffer);

  /* flush if enable-last-buffer is TRUE */
  if (enable_last_buffer) {
    g_object_set(G_OBJECT(xvimagesink), "enable-last-buffer", FALSE, NULL);
    g_object_set(G_OBJECT(xvimagesink), "enable-last-buffer", TRUE, NULL);
  }

  return;
}


static void _release_flush_buffer(GstXvImageSink *xvimagesink)
{
	int i = 0;

	if (xvimagesink == NULL ||
	    xvimagesink->flush_buffer == NULL) {
		GST_WARNING("handle is NULL");
		return;
	}

	GST_WARNING_OBJECT(xvimagesink, "release FLUSH BUFFER");

	for (i = 0 ; i < XV_BUF_PLANE_NUM ; i++) {
		if (xvimagesink->flush_buffer->bo[i]) {
			tbm_bo_unref(xvimagesink->flush_buffer->bo[i]);
			xvimagesink->flush_buffer->bo[i] = NULL;
		}
	}

	GST_WARNING_OBJECT(xvimagesink, "release FLUSH BUFFER done");

	free(xvimagesink->flush_buffer);
	xvimagesink->flush_buffer = NULL;

	return;
}


static void _add_displaying_buffer(GstXvImageSink *xvimagesink, XV_DATA_PTR img_data, GstBuffer *buffer)
{
	int i = 0;
	int j = 0;

	if (!xvimagesink || !img_data) {
		GST_ERROR("handle is NULL %p, %p", xvimagesink, img_data);
		return;
	}

	/* lock display buffer mutex */
	g_mutex_lock(xvimagesink->display_buffer_lock);

	/* increase displaying buffer count */
	xvimagesink->displaying_buffer_count++;

	/* check duplicated */
	for (i = 0 ; i < DISPLAYING_BUFFERS_MAX_NUM ; i++) {
		if (xvimagesink->displaying_buffers[i].gem_name[0] > 0) {
			if ((img_data->dmabuf_fd[0] > 0 &&
			     xvimagesink->displaying_buffers[i].dmabuf_fd[0] == img_data->dmabuf_fd[0] &&
			     xvimagesink->displaying_buffers[i].dmabuf_fd[1] == img_data->dmabuf_fd[1] &&
			     xvimagesink->displaying_buffers[i].dmabuf_fd[2] == img_data->dmabuf_fd[2]) ||
			    (img_data->bo[0] &&
			     xvimagesink->displaying_buffers[i].bo[0] == img_data->bo[0] &&
			     xvimagesink->displaying_buffers[i].bo[1] == img_data->bo[1] &&
			     xvimagesink->displaying_buffers[i].bo[2] == img_data->bo[2])) {
				/* increase ref count */
				xvimagesink->displaying_buffers[i].ref_count++;

				/* set buffer info */
				img_data->YBuf = xvimagesink->displaying_buffers[i].gem_name[0];
				img_data->CbBuf = xvimagesink->displaying_buffers[i].gem_name[1];
				img_data->CrBuf = xvimagesink->displaying_buffers[i].gem_name[2];

				if (img_data->dmabuf_fd[0] > 0) {
					GST_WARNING("already converted fd [%u %u %u] name [%u %u %u]",
					            img_data->dmabuf_fd[0], img_data->dmabuf_fd[1], img_data->dmabuf_fd[2],
					            img_data->YBuf, img_data->CbBuf, img_data->CrBuf);
				} else {
					GST_WARNING("already exported bo [%p %p %p] gem name [%u %u %u]",
					            img_data->bo[0], img_data->bo[1], img_data->bo[2],
					            img_data->YBuf, img_data->CbBuf, img_data->CrBuf);
				}

				/* unlock display buffer mutex */
				g_mutex_unlock(xvimagesink->display_buffer_lock);
				return;
			}
		}
	}

	/* store buffer temporarily */
	for (i = 0 ; i < DISPLAYING_BUFFERS_MAX_NUM ; i++) {
		if (xvimagesink->displaying_buffers[i].gem_name[0] == 0) {
			if (buffer) {
				/* increase ref count of buffer */
				gst_buffer_ref(buffer);
				xvimagesink->displaying_buffers[i].buffer = buffer;
			}

			if (img_data->dmabuf_fd[0] > 0) {
				/* convert fd to name */
				img_data->YBuf = drm_convert_dmabuf_gemname(xvimagesink, img_data->dmabuf_fd[0], &img_data->gem_handle[0]);
				img_data->CbBuf = drm_convert_dmabuf_gemname(xvimagesink, img_data->dmabuf_fd[1], &img_data->gem_handle[1]);
				img_data->CrBuf = drm_convert_dmabuf_gemname(xvimagesink, img_data->dmabuf_fd[2], &img_data->gem_handle[2]);
			} else {
				/* export bo */
				if (img_data->bo[0]) {
					img_data->YBuf = tbm_bo_export(img_data->bo[0]);
				}
				if (img_data->bo[1]) {
					img_data->CbBuf = tbm_bo_export(img_data->bo[1]);
				}
				if (img_data->bo[2]) {
					img_data->CrBuf = tbm_bo_export(img_data->bo[2]);
				}
			}

			for (j = 0 ; j < XV_BUF_PLANE_NUM ; j++) {
				xvimagesink->displaying_buffers[i].dmabuf_fd[j] = img_data->dmabuf_fd[j];
				xvimagesink->displaying_buffers[i].gem_handle[j] = img_data->gem_handle[j];
				xvimagesink->displaying_buffers[i].bo[j] = img_data->bo[j];
			}

			/* set buffer info */
			xvimagesink->displaying_buffers[i].gem_name[0] = img_data->YBuf;
			xvimagesink->displaying_buffers[i].gem_name[1] = img_data->CbBuf;
			xvimagesink->displaying_buffers[i].gem_name[2] = img_data->CrBuf;

			/* set ref count */
			xvimagesink->displaying_buffers[i].ref_count = 1;

			if (xvimagesink->displayed_buffer_count < _CHECK_DISPLAYED_BUFFER_COUNT) {
				GST_WARNING_OBJECT(xvimagesink, "cnt %d - add idx %d, buf %p, fd [%u %u %u], handle [%u %u %u], name [%u %u %u]",
				                                xvimagesink->displayed_buffer_count,
				                                i, xvimagesink->displaying_buffers[i].buffer,
				                                xvimagesink->displaying_buffers[i].dmabuf_fd[0],
				                                xvimagesink->displaying_buffers[i].dmabuf_fd[1],
				                                xvimagesink->displaying_buffers[i].dmabuf_fd[2],
				                                xvimagesink->displaying_buffers[i].gem_handle[0],
				                                xvimagesink->displaying_buffers[i].gem_handle[1],
				                                xvimagesink->displaying_buffers[i].gem_handle[2],
				                                xvimagesink->displaying_buffers[i].gem_name[0],
				                                xvimagesink->displaying_buffers[i].gem_name[1],
				                                xvimagesink->displaying_buffers[i].gem_name[2]);
			} else {
				GST_DEBUG_OBJECT(xvimagesink, "add idx %d, buf %p, fd [%u %u %u], handle [%u %u %u], name [%u %u %u]",
				                              i, xvimagesink->displaying_buffers[i].buffer,
				                              xvimagesink->displaying_buffers[i].dmabuf_fd[0],
				                              xvimagesink->displaying_buffers[i].dmabuf_fd[1],
				                              xvimagesink->displaying_buffers[i].dmabuf_fd[2],
				                              xvimagesink->displaying_buffers[i].gem_handle[0],
				                              xvimagesink->displaying_buffers[i].gem_handle[1],
				                              xvimagesink->displaying_buffers[i].gem_handle[2],
				                              xvimagesink->displaying_buffers[i].gem_name[0],
				                              xvimagesink->displaying_buffers[i].gem_name[1],
				                              xvimagesink->displaying_buffers[i].gem_name[2]);
			}

			/* set last added buffer index */
			xvimagesink->last_added_buffer_index = i;
			GST_LOG_OBJECT(xvimagesink, "xvimagesink->last_added_buffer_index %d", i);

			/* unlock display buffer mutex */
			g_mutex_unlock(xvimagesink->display_buffer_lock);

			/* get current time */
			gettimeofday(&xvimagesink->request_time[i], NULL);
			return;
		}
	}

	/* decrease displaying buffer count */
	xvimagesink->displaying_buffer_count--;

	/* unlock display buffer mutex */
	g_mutex_unlock(xvimagesink->display_buffer_lock);

	GST_ERROR("should not be reached here. buffer slot is FULL...");

	return;
}


static void _remove_displaying_buffer(GstXvImageSink *xvimagesink, unsigned int *gem_name)
{
	int i = 0;
	int j = 0;

	if (!xvimagesink || !gem_name) {
		GST_ERROR_OBJECT(xvimagesink, "handle is NULL %p, %p", xvimagesink, gem_name);
		return;
	}

	/* lock display buffer mutex */
	g_mutex_lock(xvimagesink->display_buffer_lock);

	if (xvimagesink->displaying_buffer_count == 0) {
		GST_WARNING_OBJECT(xvimagesink, "there is no displaying buffer");
		/* unlock display buffer mutex */
		g_mutex_unlock(xvimagesink->display_buffer_lock);
		return;
	}

	GST_DEBUG_OBJECT(xvimagesink, "gem name [%u %u %u], displaying buffer count %d",
	          gem_name[0], gem_name[1], gem_name[2],
	          xvimagesink->displaying_buffer_count);

	for (i = 0 ; i < DISPLAYING_BUFFERS_MAX_NUM ; i++) {
		if (xvimagesink->displaying_buffers[i].gem_name[0] == gem_name[0] &&
		    xvimagesink->displaying_buffers[i].gem_name[1] == gem_name[1] &&
		    xvimagesink->displaying_buffers[i].gem_name[2] == gem_name[2]) {
			struct timeval current_time;

			/* get current time to calculate displaying time */
			gettimeofday(&current_time, NULL);

			GST_DEBUG_OBJECT(xvimagesink, "buffer return time %8d us",
			                              (current_time.tv_sec - xvimagesink->request_time[i].tv_sec)*1000000 + \
			                              (current_time.tv_usec - xvimagesink->request_time[i].tv_usec));

			if (xvimagesink->displayed_buffer_count < _CHECK_DISPLAYED_BUFFER_COUNT) {
				xvimagesink->displayed_buffer_count++;
				GST_WARNING_OBJECT(xvimagesink, "cnt %d - remove idx %d, buf %p, handle [%u %u %u], name [%u %u %u]",
				                                xvimagesink->displayed_buffer_count,
				                                i, xvimagesink->displaying_buffers[i].buffer,
				                                xvimagesink->displaying_buffers[i].gem_handle[0],
				                                xvimagesink->displaying_buffers[i].gem_handle[1],
				                                xvimagesink->displaying_buffers[i].gem_handle[2],
				                                xvimagesink->displaying_buffers[i].gem_name[0],
				                                xvimagesink->displaying_buffers[i].gem_name[1],
				                                xvimagesink->displaying_buffers[i].gem_name[2]);
			} else {
				GST_DEBUG_OBJECT(xvimagesink, "remove idx %d, buf %p, handle [%u %u %u], name [%u %u %u]",
				                              i, xvimagesink->displaying_buffers[i].buffer,
				                              xvimagesink->displaying_buffers[i].gem_handle[0],
				                              xvimagesink->displaying_buffers[i].gem_handle[1],
				                              xvimagesink->displaying_buffers[i].gem_handle[2],
				                              xvimagesink->displaying_buffers[i].gem_name[0],
				                              xvimagesink->displaying_buffers[i].gem_name[1],
				                              xvimagesink->displaying_buffers[i].gem_name[2]);
			}

			/* decrease displaying buffer count */
			xvimagesink->displaying_buffer_count--;

			/* decrease ref count */
			xvimagesink->displaying_buffers[i].ref_count--;

			if (xvimagesink->displaying_buffers[i].ref_count > 0) {
				GST_WARNING("ref count not zero[%d], skip close gem handle",
				            xvimagesink->displaying_buffers[i].ref_count);
				break;
			}

			/* release flush buffer */
			if (xvimagesink->flush_buffer) {
				if (xvimagesink->flush_buffer->gem_name[0] == gem_name[0] &&
				    xvimagesink->flush_buffer->gem_name[1] == gem_name[1] &&
				    xvimagesink->flush_buffer->gem_name[2] == gem_name[2]) {
					_release_flush_buffer(xvimagesink);
				}
			}

			for (j = 0 ; j < XV_BUF_PLANE_NUM ; j++) {
				if (xvimagesink->displaying_buffers[i].gem_handle[j] > 0) {
					drm_close_gem(xvimagesink, &(xvimagesink->displaying_buffers[i].gem_handle[j]));
				}
				xvimagesink->displaying_buffers[i].gem_name[j] = 0;
				xvimagesink->displaying_buffers[i].dmabuf_fd[j] = 0;
				xvimagesink->displaying_buffers[i].bo[j] = NULL;
			}

			/* reset last_added_buffer_index */
			if (xvimagesink->displaying_buffer_count < 1) {
				xvimagesink->last_added_buffer_index = -1;
				GST_DEBUG_OBJECT(xvimagesink, "displaying_buffer_count %d",
				                                xvimagesink->displaying_buffer_count);
			}

			if (xvimagesink->displaying_buffers[i].buffer) {
				gst_buffer_unref(xvimagesink->displaying_buffers[i].buffer);
				xvimagesink->displaying_buffers[i].buffer = NULL;
			} else {
				GST_WARNING("no buffer to unref");
			}
			break;
		}
	}

	/* unlock display buffer mutex */
	g_mutex_unlock(xvimagesink->display_buffer_lock);

	return;
}


static int _is_connected_to_external_display(GstXvImageSink *xvimagesink)
{
  Atom type_ret = 0;
  int i = 0;
  int ret = 0;
  int size_ret = 0;
  unsigned long num_ret = 0;
  unsigned long bytes = 0;
  unsigned char *prop_ret = NULL;
  unsigned int data = 0;
  int (*handler) (Display *, XErrorEvent *) = NULL;
  Atom atom_output_external;

  atom_output_external = XInternAtom(xvimagesink->xcontext->disp,
                                       "XV_OUTPUT_EXTERNAL", False);
  if (atom_output_external != None) {
    /* set error handler */
    error_caught = FALSE;
    handler = XSetErrorHandler(gst_xvimagesink_handle_xerror);

    ret = XGetWindowProperty(xvimagesink->xcontext->disp,
                             xvimagesink->xwindow->win,
                             atom_output_external, 0, 0x7fffffff,
                             False, XA_CARDINAL, &type_ret, &size_ret,
                             &num_ret, &bytes, &prop_ret);
    XSync(xvimagesink->xcontext->disp, FALSE);
    if (ret != Success || error_caught) {
      GST_WARNING_OBJECT(xvimagesink, "XGetWindowProperty failed");
      if (prop_ret) {
        XFree(prop_ret);
      }
      if (error_caught) {
        GST_WARNING_OBJECT(xvimagesink, "error caught in XGetWindowProperty()");
      }
      if (handler) {
        error_caught = FALSE;
        XSetErrorHandler (handler);
      }
      return False;
    }

    if (handler) {
      error_caught = FALSE;
      XSetErrorHandler (handler);
    }

    if (!num_ret) {
      GST_WARNING_OBJECT(xvimagesink, "XGetWindowProperty num_ret failed");
      if (prop_ret) {
        XFree(prop_ret);
      }
      return False;
    }

    if (prop_ret) {
      switch (size_ret) {
      case 8:
        for (i = 0 ; i < num_ret ; i++) {
          (&data)[i] = prop_ret[i];
        }
        break;
      case 16:
        for (i = 0 ; i < num_ret ; i++) {
          ((unsigned short *)&data)[i] = ((unsigned short *)prop_ret)[i];
        }
        break;
      case 32:
        for (i = 0 ; i < num_ret ; i++) {
          ((unsigned int *)&data)[i] = ((unsigned long *)prop_ret)[i];
        }
        break;
      }
      XFree(prop_ret);
      prop_ret = NULL;

      GST_WARNING_OBJECT(xvimagesink, "external display %d", data);

      return (int)data;
    } else {
      GST_WARNING_OBJECT(xvimagesink, "prop_ret is NULL");
      return False;
    }
  } else {
    GST_WARNING_OBJECT(xvimagesink, "get XV_OUTPUT_EXTERNAL atom failed");
  }

  return False;
}
#endif /* GST_EXT_XV_ENHANCEMENT */

/* This function handles a GstXWindow creation
 * The width and height are the actual pixel size on the display */
static GstXWindow *
gst_xvimagesink_xwindow_new (GstXvImageSink * xvimagesink,
    gint width, gint height)
{
  GstXWindow *xwindow = NULL;
  XGCValues values;
#ifdef GST_EXT_XV_ENHANCEMENT
  XSetWindowAttributes win_attr;
  XWindowAttributes root_attr = {0 , };
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  xwindow = g_new0 (GstXWindow, 1);

  xvimagesink->render_rect.x = xvimagesink->render_rect.y = 0;
#ifdef GST_EXT_XV_ENHANCEMENT
  /* 0 or 180 */
  if (xvimagesink->rotate_angle == 0 || xvimagesink->rotate_angle == 2) {
    xvimagesink->render_rect.w = xwindow->width = width;
    xvimagesink->render_rect.h = xwindow->height = height;
  /* 90 or 270 */
  } else {
    xvimagesink->render_rect.w = xwindow->width = height;
    xvimagesink->render_rect.h = xwindow->height = width;
  }

  if(!xvimagesink->is_pixmap)
    XGetWindowAttributes(xvimagesink->xcontext->disp, xvimagesink->xcontext->root, &root_attr);

  if (xwindow->width > root_attr.width) {
    GST_INFO_OBJECT(xvimagesink, "Width[%d] is bigger than Max width. Set Max[%d].",
                                 xwindow->width, root_attr.width);
    xvimagesink->render_rect.w = xwindow->width = root_attr.width;
  }
  if (xwindow->height > root_attr.height) {
    GST_INFO_OBJECT(xvimagesink, "Height[%d] is bigger than Max Height. Set Max[%d].",
                                 xwindow->height, root_attr.height);
    xvimagesink->render_rect.h = xwindow->height = root_attr.height;
  }
  xwindow->internal = TRUE;

  g_mutex_lock (xvimagesink->x_lock);

  GST_DEBUG_OBJECT( xvimagesink, "window create [%dx%d]", xwindow->width, xwindow->height );

  xwindow->win = XCreateSimpleWindow(xvimagesink->xcontext->disp,
                                     xvimagesink->xcontext->root,
                                     0, 0, xwindow->width, xwindow->height,
                                     0, 0, 0);

  xvimagesink->xim_transparenter = make_transparent_image(xvimagesink->xcontext->disp,
                                                          xvimagesink->xcontext->root,
                                                          xwindow->width, xwindow->height);

  /* Make window manager not to change window size as Full screen */
  win_attr.override_redirect = True;
  if(!xvimagesink->is_pixmap)
    XChangeWindowAttributes(xvimagesink->xcontext->disp, xwindow->win, CWOverrideRedirect, &win_attr);
#else /* GST_EXT_XV_ENHANCEMENT */
  xvimagesink->render_rect.w = width;
  xvimagesink->render_rect.h = height;

  xwindow->width = width;
  xwindow->height = height;
  xwindow->internal = TRUE;

  g_mutex_lock (xvimagesink->x_lock);

  xwindow->win = XCreateSimpleWindow (xvimagesink->xcontext->disp,
      xvimagesink->xcontext->root,
      0, 0, width, height, 0, 0, xvimagesink->xcontext->black);
#endif /* GST_EXT_XV_ENHANCEMENT */

  /* We have to do that to prevent X from redrawing the background on
   * ConfigureNotify. This takes away flickering of video when resizing. */
  XSetWindowBackgroundPixmap (xvimagesink->xcontext->disp, xwindow->win, None);

  /* set application name as a title */
  gst_xvimagesink_xwindow_set_title (xvimagesink, xwindow, NULL);

  if (xvimagesink->handle_events) {
    Atom wm_delete;
#ifdef GST_EXT_XV_ENHANCEMENT
    XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xcontext->root, StructureNotifyMask | SubstructureNotifyMask);
    XSelectInput (xvimagesink->xcontext->disp, xwindow->win, ExposureMask |
         StructureNotifyMask | PointerMotionMask | KeyPressMask |
         KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PropertyChangeMask);
#else
    XSelectInput (xvimagesink->xcontext->disp, xwindow->win, ExposureMask |
        StructureNotifyMask | PointerMotionMask | KeyPressMask |
        KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);
#endif


    /* Tell the window manager we'd like delete client messages instead of
     * being killed */
    wm_delete = XInternAtom (xvimagesink->xcontext->disp,
        "WM_DELETE_WINDOW", True);
    if (wm_delete != None) {
      (void) XSetWMProtocols (xvimagesink->xcontext->disp, xwindow->win,
          &wm_delete, 1);
    }
  }

  xwindow->gc = XCreateGC (xvimagesink->xcontext->disp,
      xwindow->win, 0, &values);

  XMapRaised (xvimagesink->xcontext->disp, xwindow->win);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  gst_xvimagesink_xwindow_decorate (xvimagesink, xwindow);

  gst_x_overlay_got_window_handle (GST_X_OVERLAY (xvimagesink), xwindow->win);

  return xwindow;
}

/* This function destroys a GstXWindow */
static void
gst_xvimagesink_xwindow_destroy (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow)
{
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->x_lock);

  /* If we did not create that window we just free the GC and let it live */
  if (xwindow->internal) {
    XDestroyWindow (xvimagesink->xcontext->disp, xwindow->win);
    if (xvimagesink->xim_transparenter) {
      XDestroyImage(xvimagesink->xim_transparenter);
      xvimagesink->xim_transparenter = NULL;
    }
  } else {
    XSelectInput (xvimagesink->xcontext->disp, xwindow->win, 0);
  }

  XFreeGC (xvimagesink->xcontext->disp, xwindow->gc);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  g_free (xwindow);
}

#ifdef GST_EXT_XV_ENHANCEMENT
/* This function destroys a GstXWindow */
static void
gst_xvimagesink_xpixmap_destroy (GstXvImageSink * xvimagesink,
    GstXPixmap * xpixmap)
{
  g_return_if_fail (xpixmap != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->x_lock);

  XSelectInput (xvimagesink->xcontext->disp, xpixmap->pixmap, 0);

  XFreeGC (xvimagesink->xcontext->disp, xpixmap->gc);

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);

  g_free (xpixmap);
}
#endif /* GST_EXT_XV_ENHANCEMENT */

static void
gst_xvimagesink_xwindow_update_geometry (GstXvImageSink * xvimagesink)
{
#ifdef GST_EXT_XV_ENHANCEMENT
  Window root_window, child_window;
  XWindowAttributes root_attr = {0 , };

  int cur_win_x = 0;
  int cur_win_y = 0;
  unsigned int cur_win_width = 0;
  unsigned int cur_win_height = 0;
  unsigned int cur_win_border_width = 0;
  unsigned int cur_win_depth = 0;
#else /* GST_EXT_XV_ENHANCEMENT */
  XWindowAttributes attr;
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  /* Update the window geometry */
  g_mutex_lock (xvimagesink->x_lock);
  if (G_UNLIKELY (xvimagesink->xwindow == NULL)) {
    g_mutex_unlock (xvimagesink->x_lock);
    return;
  }

#ifdef GST_EXT_XV_ENHANCEMENT
  /* Get root window and size of current window */
  XGetGeometry( xvimagesink->xcontext->disp, xvimagesink->xwindow->win, &root_window,
    &cur_win_x, &cur_win_y, /* relative x, y */
    &cur_win_width, &cur_win_height,
    &cur_win_border_width, &cur_win_depth );

  xvimagesink->xwindow->width = cur_win_width;
  xvimagesink->xwindow->height = cur_win_height;

  /* Get absolute coordinates of current window */
  if(!xvimagesink->is_pixmap) {
    XTranslateCoordinates( xvimagesink->xcontext->disp,
      xvimagesink->xwindow->win,
      root_window,
      0, 0,
      &cur_win_x, &cur_win_y, // relative x, y to root window == absolute x, y
      &child_window );
  }

  xvimagesink->xwindow->x = cur_win_x;
  xvimagesink->xwindow->y = cur_win_y;

  /* Get size of root window == size of screen */
  if(!xvimagesink->is_pixmap)
    XGetWindowAttributes(xvimagesink->xcontext->disp, root_window, &root_attr);

  xvimagesink->scr_w = root_attr.width;
  xvimagesink->scr_h = root_attr.height;

  if (!xvimagesink->have_render_rect) {
    xvimagesink->render_rect.x = xvimagesink->render_rect.y = 0;
    xvimagesink->render_rect.w = cur_win_width;
    xvimagesink->render_rect.h = cur_win_height;
  }
  if (xvimagesink->scr_w != xvimagesink->xwindow->width ||
    xvimagesink->scr_h != xvimagesink->xwindow->height) {
    xvimagesink->is_multi_window = TRUE;
    g_signal_emit_by_name(G_OBJECT (xvimagesink), "multiwindow-active", xvimagesink->is_multi_window);
    GST_INFO_OBJECT(xvimagesink, "It may be multi-window scenario");
  } else {
    xvimagesink->is_multi_window = FALSE;
    g_signal_emit_by_name(G_OBJECT (xvimagesink), "multiwindow-active", xvimagesink->is_multi_window);
    GST_INFO_OBJECT(xvimagesink, "It may be full-window scenario");
  }

  GST_LOG_OBJECT(xvimagesink, "screen size %dx%d, current window geometry %d,%d,%dx%d, render_rect %d,%d,%dx%d",
    xvimagesink->scr_w, xvimagesink->scr_h,
    xvimagesink->xwindow->x, xvimagesink->xwindow->y,
    xvimagesink->xwindow->width, xvimagesink->xwindow->height,
    xvimagesink->render_rect.x, xvimagesink->render_rect.y,
    xvimagesink->render_rect.w, xvimagesink->render_rect.h);
#else /* GST_EXT_XV_ENHANCEMENT */
  XGetWindowAttributes (xvimagesink->xcontext->disp,
      xvimagesink->xwindow->win, &attr);

  xvimagesink->xwindow->width = attr.width;
  xvimagesink->xwindow->height = attr.height;

  if (!xvimagesink->have_render_rect) {
    xvimagesink->render_rect.x = xvimagesink->render_rect.y = 0;
    xvimagesink->render_rect.w = attr.width;
    xvimagesink->render_rect.h = attr.height;
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_mutex_unlock (xvimagesink->x_lock);
}

static void
gst_xvimagesink_xwindow_clear (GstXvImageSink * xvimagesink,
    GstXWindow * xwindow)
{
#ifdef GST_EXT_XV_ENHANCEMENT
  if (xvimagesink->is_subpicture_format)
    return;
#endif
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->x_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
  GST_WARNING_OBJECT(xvimagesink, "CALL XvStopVideo");
#endif /* GST_EXT_XV_ENHANCEMENT */

  XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id,
      xwindow->win);
#ifdef GST_EXT_XV_ENHANCEMENT
#if 0
  /* NOTE : it should be enabled in pixmap buffer case,
            if we can check whether if it is a pixmap or a window by X API */
  /* Preview area is not updated before other UI is updated in the screen. */
  XSetForeground (xvimagesink->xcontext->disp, xwindow->gc,
      xvimagesink->xcontext->black);

  XFillRectangle (xvimagesink->xcontext->disp, xwindow->win, xwindow->gc,
      xvimagesink->render_rect.x, xvimagesink->render_rect.y,
      xvimagesink->render_rect.w, xvimagesink->render_rect.h);
#endif
#endif /* GST_EXT_XV_ENHANCEMENT */

  XSync (xvimagesink->xcontext->disp, FALSE);

  g_mutex_unlock (xvimagesink->x_lock);
}

/* This function commits our internal colorbalance settings to our grabbed Xv
   port. If the xcontext is not initialized yet it simply returns */
static void
gst_xvimagesink_update_colorbalance (GstXvImageSink * xvimagesink)
{
  GList *channels = NULL;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  /* If we haven't initialized the X context we can't update anything */
  if (xvimagesink->xcontext == NULL)
    return;

  /* Don't set the attributes if they haven't been changed, to avoid
   * rounding errors changing the values */
  if (!xvimagesink->cb_changed)
    return;

  /* For each channel of the colorbalance we calculate the correct value
     doing range conversion and then set the Xv port attribute to match our
     values. */
  channels = xvimagesink->xcontext->channels_list;

  while (channels) {
    if (channels->data && GST_IS_COLOR_BALANCE_CHANNEL (channels->data)) {
      GstColorBalanceChannel *channel = NULL;
      Atom prop_atom;
      gint value = 0;
      gdouble convert_coef;

      channel = GST_COLOR_BALANCE_CHANNEL (channels->data);
      g_object_ref (channel);

      /* Our range conversion coef */
      convert_coef = (channel->max_value - channel->min_value) / 2000.0;

      if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
        value = xvimagesink->hue;
      } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
        value = xvimagesink->saturation;
      } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
        value = xvimagesink->contrast;
      } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
        value = xvimagesink->brightness;
      } else {
        g_warning ("got an unknown channel %s", channel->label);
        g_object_unref (channel);
        return;
      }

      /* Committing to Xv port */
      g_mutex_lock (xvimagesink->x_lock);
      prop_atom =
          XInternAtom (xvimagesink->xcontext->disp, channel->label, True);
      if (prop_atom != None) {
        int xv_value;
        xv_value =
            floor (0.5 + (value + 1000) * convert_coef + channel->min_value);
        XvSetPortAttribute (xvimagesink->xcontext->disp,
            xvimagesink->xcontext->xv_port_id, prop_atom, xv_value);
      }
      g_mutex_unlock (xvimagesink->x_lock);

      g_object_unref (channel);
    }
    channels = g_list_next (channels);
  }
}

/* This function handles XEvents that might be in the queue. It generates
   GstEvent that will be sent upstream in the pipeline to handle interactivity
   and navigation. It will also listen for configure events on the window to
   trigger caps renegotiation so on the fly software scaling can work. */
static void
gst_xvimagesink_handle_xevents (GstXvImageSink * xvimagesink)
{
  XEvent e;
  guint pointer_x = 0, pointer_y = 0;
  gboolean pointer_moved = FALSE;
  gboolean exposed = FALSE, configured = FALSE;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

#ifdef GST_EXT_XV_ENHANCEMENT
  GST_LOG("check x event");
  Atom sc_status_atom = XInternAtom (xvimagesink->xcontext->disp, STR_ATOM_SCRNCONF_STATUS, FALSE);
  Atom external_atom = XInternAtom (xvimagesink->xcontext->disp, "XV_OUTPUT_EXTERNAL", FALSE);
#endif /* GST_EXT_XV_ENHANCEMENT */

  /* Handle Interaction, produces navigation events */

  /* We get all pointer motion events, only the last position is
     interesting. */
  g_mutex_lock (xvimagesink->flow_lock);
  g_mutex_lock (xvimagesink->x_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
  if (xvimagesink->xcontext->disp) {
#endif //GST_EXT_XV_ENHANCEMENT
  while (XCheckWindowEvent (xvimagesink->xcontext->disp,
          xvimagesink->xwindow->win, PointerMotionMask, &e)) {
    g_mutex_unlock (xvimagesink->x_lock);
    g_mutex_unlock (xvimagesink->flow_lock);

    switch (e.type) {
      case MotionNotify:
        pointer_x = e.xmotion.x;
        pointer_y = e.xmotion.y;
        pointer_moved = TRUE;
        break;
      default:
        break;
    }
    g_mutex_lock (xvimagesink->flow_lock);
    g_mutex_lock (xvimagesink->x_lock);
  }
#ifdef GST_EXT_XV_ENHANCEMENT
  }
#endif //GST_EXT_XV_ENHANCEMENT
  if (pointer_moved) {
    g_mutex_unlock (xvimagesink->x_lock);
    g_mutex_unlock (xvimagesink->flow_lock);

    GST_DEBUG ("xvimagesink pointer moved over window at %d,%d",
        pointer_x, pointer_y);
    gst_navigation_send_mouse_event (GST_NAVIGATION (xvimagesink),
        "mouse-move", 0, e.xbutton.x, e.xbutton.y);

    g_mutex_lock (xvimagesink->flow_lock);
    g_mutex_lock (xvimagesink->x_lock);
  }

  /* We get all events on our window to throw them upstream */
  while (XCheckWindowEvent (xvimagesink->xcontext->disp,
          xvimagesink->xwindow->win,
          KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask,
          &e)) {
    KeySym keysym;

    /* We lock only for the X function call */
    g_mutex_unlock (xvimagesink->x_lock);
    g_mutex_unlock (xvimagesink->flow_lock);

    switch (e.type) {
      case ButtonPress:
        /* Mouse button pressed over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("xvimagesink button %d pressed over window at %d,%d",
            e.xbutton.button, e.xbutton.x, e.xbutton.y);
        gst_navigation_send_mouse_event (GST_NAVIGATION (xvimagesink),
            "mouse-button-press", e.xbutton.button, e.xbutton.x, e.xbutton.y);
        break;
      case ButtonRelease:
        /* Mouse button released over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("xvimagesink button %d released over window at %d,%d",
            e.xbutton.button, e.xbutton.x, e.xbutton.y);
        gst_navigation_send_mouse_event (GST_NAVIGATION (xvimagesink),
            "mouse-button-release", e.xbutton.button, e.xbutton.x, e.xbutton.y);
        break;
      case KeyPress:
      case KeyRelease:
        /* Key pressed/released over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("xvimagesink key %d pressed over window at %d,%d",
            e.xkey.keycode, e.xkey.x, e.xkey.y);
        g_mutex_lock (xvimagesink->x_lock);
        keysym = XKeycodeToKeysym (xvimagesink->xcontext->disp,
            e.xkey.keycode, 0);
        g_mutex_unlock (xvimagesink->x_lock);
        if (keysym != NoSymbol) {
          char *key_str = NULL;

          g_mutex_lock (xvimagesink->x_lock);
          key_str = XKeysymToString (keysym);
          g_mutex_unlock (xvimagesink->x_lock);
          gst_navigation_send_key_event (GST_NAVIGATION (xvimagesink),
              e.type == KeyPress ? "key-press" : "key-release", key_str);
        } else {
          gst_navigation_send_key_event (GST_NAVIGATION (xvimagesink),
              e.type == KeyPress ? "key-press" : "key-release", "unknown");
        }
        break;
      default:
        GST_DEBUG ("xvimagesink unhandled X event (%d)", e.type);
    }
    g_mutex_lock (xvimagesink->flow_lock);
    g_mutex_lock (xvimagesink->x_lock);
  }
  /* Handle Expose */
  while (XCheckWindowEvent (xvimagesink->xcontext->disp,
          xvimagesink->xwindow->win, ExposureMask | StructureNotifyMask, &e)) {
    switch (e.type) {
      case Expose:
        exposed = TRUE;
        break;
      case ConfigureNotify:
        g_mutex_unlock (xvimagesink->x_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
        GST_WARNING("Call gst_xvimagesink_xwindow_update_geometry!");
#endif /* GST_EXT_XV_ENHANCEMENT */
        gst_xvimagesink_xwindow_update_geometry (xvimagesink);
#ifdef GST_EXT_XV_ENHANCEMENT
        GST_WARNING("Return gst_xvimagesink_xwindow_update_geometry!");
#endif /* GST_EXT_XV_ENHANCEMENT */
        g_mutex_lock (xvimagesink->x_lock);
        configured = TRUE;
        break;
      default:
        break;
    }
  }

  if (xvimagesink->handle_expose && (exposed || configured)) {
    g_mutex_unlock (xvimagesink->x_lock);
    g_mutex_unlock (xvimagesink->flow_lock);

    gst_xvimagesink_expose (GST_X_OVERLAY (xvimagesink));

    g_mutex_lock (xvimagesink->flow_lock);
    g_mutex_lock (xvimagesink->x_lock);
  }

  /* Handle Display events */
  while (XPending (xvimagesink->xcontext->disp)) {
    XNextEvent (xvimagesink->xcontext->disp, &e);
    switch (e.type) {
      case ClientMessage:{
#ifdef GST_EXT_XV_ENHANCEMENT
        XClientMessageEvent *cme = (XClientMessageEvent *)&e;
        Atom buffer_atom = XInternAtom(xvimagesink->xcontext->disp, "XV_RETURN_BUFFER", False);
        Atom qp_state_atom = XInternAtom(xvimagesink->xcontext->disp, "_E_ILLUME_QUICKPANEL_STATE", False);
        Atom qp_on_atom = XInternAtom(xvimagesink->xcontext->disp, "_E_ILLUME_QUICKPANEL_ON", False);
        Atom qp_off_atom = XInternAtom(xvimagesink->xcontext->disp, "_E_ILLUME_QUICKPANEL_OFF", False);
#endif /* GST_EXT_XV_ENHANCEMENT */
        Atom wm_delete;

#ifdef GST_EXT_XV_ENHANCEMENT
        GST_LOG_OBJECT(xvimagesink, "message type %d, buffer atom %d", cme->message_type, buffer_atom);
        if (cme->message_type == buffer_atom) {
          unsigned int gem_name[XV_BUF_PLANE_NUM] = { 0, };

          GST_DEBUG("data.l[0] -> %d, data.l[1] -> %d",
                    cme->data.l[0], cme->data.l[1]);

          gem_name[0] = cme->data.l[0];
          gem_name[1] = cme->data.l[1];

          _remove_displaying_buffer(xvimagesink, gem_name);
          break;
        } else if (cme->message_type == sc_status_atom) {
          int stat = cme->data.s[0];
          if (stat == UTILX_SCRNCONF_STATUS_NULL) {
            GST_WARNING ("get UTILX_SCRNCONF_STATUS_NULL event\n");
            check_hdmi_connected(xvimagesink);
          } else if (stat == UTILX_SCRNCONF_STATUS_CONNECT) {
            GST_WARNING ("get UTILX_SCRNCONF_STATUS_CONNECT event\n");
            check_hdmi_connected(xvimagesink);
          } else if (stat == UTILX_SCRNCONF_STATUS_ACTIVE) {
            GST_WARNING ("get UTILX_SCRNCONF_STATUS_ACTIVE event\n");
            g_signal_emit_by_name(G_OBJECT (xvimagesink), "display-status", DISPLAY_STATUS_HDMI_ACTIVE);
            check_hdmi_connected(xvimagesink);
          } else {
            GST_INFO ("Wrong status\n");
          }
          break;
        } else if (cme->message_type == qp_state_atom) {
          if ((Atom) cme->data.l[0] == qp_on_atom) {
            /* quick panel on */
            GST_WARNING_OBJECT(xvimagesink, "quick panel is ON");
            xvimagesink->is_quick_panel_on = TRUE;
            g_signal_emit_by_name(G_OBJECT (xvimagesink), "quick-panel-on", TRUE);
          } else if ((Atom) cme->data.l[0] == qp_off_atom) {
            /* quick panel off */
            GST_WARNING_OBJECT(xvimagesink, "quick panel is OFF");
            xvimagesink->is_quick_panel_on = FALSE;
            g_signal_emit_by_name(G_OBJECT (xvimagesink), "quick-panel-on", FALSE);
          }
          break;
        }
#endif /* GST_EXT_XV_ENHANCEMENT */

        wm_delete = XInternAtom (xvimagesink->xcontext->disp,
            "WM_DELETE_WINDOW", True);
        if (wm_delete != None && wm_delete == (Atom) e.xclient.data.l[0]) {
          /* Handle window deletion by posting an error on the bus */
          GST_ELEMENT_ERROR (xvimagesink, RESOURCE, NOT_FOUND,
              ("Output window was closed"), (NULL));

          g_mutex_unlock (xvimagesink->x_lock);
          gst_xvimagesink_xwindow_destroy (xvimagesink, xvimagesink->xwindow);
          xvimagesink->xwindow = NULL;
          g_mutex_lock (xvimagesink->x_lock);
        }
        break;
      }
#ifdef GST_EXT_XV_ENHANCEMENT
      case VisibilityNotify:
        if (xvimagesink->xwindow &&
            (e.xvisibility.window == xvimagesink->xwindow->win)) {
          if (e.xvisibility.state == VisibilityFullyObscured) {
            Atom atom_stream;

            GST_WARNING_OBJECT(xvimagesink, "current window is FULLY HIDED");

            xvimagesink->is_hided = TRUE;
            g_signal_emit_by_name(G_OBJECT (xvimagesink), "hided-window", TRUE);
            if (!_is_connected_to_external_display(xvimagesink)) {
              GST_WARNING_OBJECT(xvimagesink, "no external display, calling XvStopVideo()");
              XvStopVideo(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xwindow->win);
              XSync(xvimagesink->xcontext->disp, FALSE);
            } else {
              if (GST_STATE(xvimagesink) == GST_STATE_PLAYING || xvimagesink->keep_external_fullscreen_prev) {
                GST_WARNING_OBJECT(xvimagesink, "external display is enabled. skip XvStopVideo()");
              } else {
                GST_WARNING_OBJECT(xvimagesink, "external display is enabled, but not in the middle of playing, calling XvStopVideo()");
                XvStopVideo(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xwindow->win);
                XSync(xvimagesink->xcontext->disp, FALSE);
              }
            }
          } else {
            GST_INFO_OBJECT(xvimagesink, "current window is SHOWN");

            if (xvimagesink->is_hided) {
              g_mutex_unlock(xvimagesink->x_lock);
              g_mutex_unlock(xvimagesink->flow_lock);

              xvimagesink->is_hided = FALSE;
              g_signal_emit_by_name(G_OBJECT (xvimagesink), "hided-window", FALSE);
              gst_xvimagesink_expose(GST_X_OVERLAY(xvimagesink));

              g_mutex_lock(xvimagesink->flow_lock);
              g_mutex_lock(xvimagesink->x_lock);
            } else {
              GST_INFO_OBJECT(xvimagesink, "current window is not HIDED, skip this event");
            }
          }
        }
        break;

      case PropertyNotify:
        {
          XPropertyEvent *noti = (XPropertyEvent *)&e;
          if(xvimagesink->xwindow) {
            if (noti->window == xvimagesink->xwindow->win && noti->atom == external_atom) {
              int value = 0;
              get_window_prop_card32_property (xvimagesink->xcontext->disp,
                                               xvimagesink->xwindow->win,
                                               external_atom, XA_CARDINAL,
                                               (unsigned int*)&value, 1);
              if (value) {
                // If value is 1, video will be displayed only on external display.
                // video won't be displayed on LCD.
                g_signal_emit_by_name(G_OBJECT (xvimagesink), "display-status", DISPLAY_STATUS_UNKNOWN_ACTIVE);
                if(xvimagesink->external_width==0 && xvimagesink->external_height==0) {
                  xvimagesink->external_width = 1920;
                  xvimagesink->external_height = 1080;
                  GST_WARNING("connected unknown external display");
                }
              } else {
                g_signal_emit_by_name(G_OBJECT (xvimagesink), "display-status", DISPLAY_STATUS_NULL); //NULL
                if(xvimagesink->external_width!=0 || xvimagesink->external_height!=0) {
                  xvimagesink->external_width = 0;
                  xvimagesink->external_height = 0;
                  GST_WARNING("disconnected external display");
                }
              }

              g_signal_emit_by_name(G_OBJECT (xvimagesink), "external-resolution", xvimagesink->external_width, xvimagesink->external_height);
              GST_INFO ("external device : %s\n", (value)?"on":"off");
            }
          }
          break;
       }

#endif /* GST_EXT_XV_ENHANCEMENT */
      default:
        break;
    }
  }
  g_mutex_unlock (xvimagesink->x_lock);
  g_mutex_unlock (xvimagesink->flow_lock);
}
#ifdef GST_EXT_XV_ENHANCEMENT
static int
get_window_prop_card32_property (Display* dpy, Window win, Atom atom, Atom type,
                                 unsigned int *val, unsigned int len)
{
  unsigned char* prop_ret;
  Atom type_ret;
  unsigned long bytes_after, num_ret;
  int format_ret;
  unsigned int i;
  int num;
  int ret;
  int (*handler) (Display *, XErrorEvent *) = NULL;

  prop_ret = NULL;

  /* set error handler */
  error_caught = FALSE;
  handler = XSetErrorHandler(gst_xvimagesink_handle_xerror);

  ret = XGetWindowProperty(dpy, win, atom, 0, 0x7fffffff, False,
                           type, &type_ret, &format_ret, &num_ret,
                           &bytes_after, &prop_ret);
  XSync(dpy, FALSE);
  if (ret != Success || error_caught) {
    GST_WARNING("XGetWindowProperty failed [%d, %d]", ret, error_caught);
    if (handler) {
      error_caught = FALSE;
      XSetErrorHandler (handler);
    }
    return -1;
  }

  if (handler) {
    error_caught = FALSE;
    XSetErrorHandler(handler);
  }

  if (type_ret != type || format_ret != 32)
    num = -1;
  else if (num_ret == 0 || !prop_ret)
    num = 0;
  else
  {
    if (num_ret < len)
      len = num_ret;
    for (i = 0; i < len; i++)
      val[i] = ((unsigned long *)prop_ret)[i];
    num = len;
  }

  if (prop_ret)
    XFree(prop_ret);

  return num;
}

static void check_hdmi_connected(GstXvImageSink *xvimagesink)
{
  char *str_output = NULL;
  char *str_resolution = NULL;
  int external[2];
  int cnt = 0;
  char** list = NULL;
  char** walk = NULL;

  UtilxScrnConf *scrnconf = utilx_scrnconf_allocate();
  if (!scrnconf)
  {
    GST_WARNING ("fail to allocate scrnconf");
    return;
  }
  utilx_scrnconf_get_info (xvimagesink->xcontext->disp, scrnconf);

  str_output = scrnconf->str_output;

  if (scrnconf->status == UTILX_SCRNCONF_STATUS_CONNECT)
  {
    GST_INFO("CONNECT");
  }
  else if (scrnconf->status == UTILX_SCRNCONF_STATUS_ACTIVE)
  {
     GST_INFO("ACTIVE");

    list = g_strsplit(scrnconf->str_resolution, "x", 2);

    if (!list)
    {
        if (scrnconf)
        utilx_scrnconf_free (scrnconf);
        return;
    }
    for(walk = list; *walk; walk++)
    {
      external[cnt++] = atoi(*walk);
    }
    if (cnt!=2) {
      GST_WARNING("data error");
      if(scrnconf)
        utilx_scrnconf_free (scrnconf);
      g_strfreev(list);
      return;
    } else {
      xvimagesink->external_width = external[0];
      xvimagesink->external_height = external[1];
      g_strfreev(list);
    }
    GST_INFO("external display : %d * %d", xvimagesink->external_width, xvimagesink->external_height);
    g_signal_emit_by_name(G_OBJECT (xvimagesink), "external-resolution", xvimagesink->external_width, xvimagesink->external_height);
  }
  else
  {
     GST_INFO("NULL");
  }

  str_resolution = scrnconf->str_resolution;

  if (scrnconf->dispmode == UTILX_SCRNCONF_DISPMODE_CLONE)
  {
     GST_INFO("CLONE mode");
  }
  else if (scrnconf->dispmode == UTILX_SCRNCONF_DISPMODE_EXTENDED)
  {
    GST_INFO("EXTENDED mode");
  }
  else
  {
    GST_INFO("NULL mode");
  }

  GST_INFO ("[Display status] : %s, %s\n", str_output, str_resolution);

  if(scrnconf)
    utilx_scrnconf_free (scrnconf);

}

static gboolean
check_supportable_port_attr(GstXvImageSink * xvimagesink, gchar* attr_name)
{
  int i = 0;
  int count = 0;

  XvAttribute *attr = XvQueryPortAttributes(xvimagesink->xcontext->disp,
                                            xvimagesink->xcontext->xv_port_id, &count);
  if (attr) {
    for (i = 0 ; i < count ; i++) {
      if (!strcmp(attr[i].name, attr_name)) {
        GST_INFO("%s[index %d] found", attr_name, i);
        XFree(attr);
        return TRUE;
      }
    }
    XFree(attr);
  } else {
    GST_WARNING("XvQueryPortAttributes disp:%d, port_id:%d failed",
                xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id);
  }
  return FALSE;
}
#endif //GST_EXT_XV_ENHANCEMENT

static void
gst_lookup_xv_port_from_adaptor (GstXContext * xcontext,
    XvAdaptorInfo * adaptors, int adaptor_no)
{
  gint j;
  gint res;

  /* Do we support XvImageMask ? */
  if (!(adaptors[adaptor_no].type & XvImageMask)) {
    GST_DEBUG ("XV Adaptor %s has no support for XvImageMask",
        adaptors[adaptor_no].name);
    return;
  }

  /* We found such an adaptor, looking for an available port */
  for (j = 0; j < adaptors[adaptor_no].num_ports && !xcontext->xv_port_id; j++) {
    /* We try to grab the port */
    res = XvGrabPort (xcontext->disp, adaptors[adaptor_no].base_id + j, 0);
    if (Success == res) {
      xcontext->xv_port_id = adaptors[adaptor_no].base_id + j;
      GST_DEBUG ("XV Adaptor %s with %ld ports", adaptors[adaptor_no].name,
          adaptors[adaptor_no].num_ports);
    } else {
      GST_DEBUG ("GrabPort %d for XV Adaptor %s failed: %d", j,
          adaptors[adaptor_no].name, res);
    }
  }
}

#ifdef GST_EXT_XV_ENHANCEMENT
static void
gst_lookup_xv_port_for_subtitle (GstXContext * xcontext,
    XvAdaptorInfo * adaptors, guint adaptor_no, guint nb_adaptors)
{
  int i;
  if (!adaptors)
    return;
  for (i = 0; i < nb_adaptors; i++)
  {
    int min, max;
    if (!(adaptors[i].type & XvOutputMask) ||
        !(adaptors[i].type & XvStillMask))
        continue;
    min = adaptors[i].base_id;
    max = adaptors[i].base_id + adaptors[i].num_ports;
    for (adaptors[adaptor_no].num_ports = min; adaptors[adaptor_no].num_ports < max ; adaptors[adaptor_no].num_ports++)
    {
      if (XvGrabPort (xcontext->disp, adaptors[adaptor_no].num_ports, 0) == Success)
      {
        GST_INFO ("========================================");
        GST_INFO ("XvGrabPort  success : %ld", adaptors[adaptor_no].num_ports);
        GST_INFO ("========================================");
        xcontext->xv_port_id = adaptors[adaptor_no].num_ports;
        return;
      }
      GST_WARNING ("fail : grab port(%ld)\n", adaptors[adaptor_no].num_ports);
      usleep(10000);
    }
  }
}
#endif


/* This function generates a caps with all supported format by the first
   Xv grabable port we find. We store each one of the supported formats in a
   format list and append the format to a newly created caps that we return
   If this function does not return NULL because of an error, it also grabs
   the port via XvGrabPort */
static GstCaps *
gst_xvimagesink_get_xv_support (GstXvImageSink * xvimagesink,
    GstXContext * xcontext)
{
  gint i;
  XvAdaptorInfo *adaptors;
  gint nb_formats;
  XvImageFormatValues *formats = NULL;
  guint nb_encodings;
  XvEncodingInfo *encodings = NULL;
  gulong max_w = G_MAXINT, max_h = G_MAXINT;
  GstCaps *caps = NULL;
  GstCaps *rgb_caps = NULL;

  g_return_val_if_fail (xcontext != NULL, NULL);

  /* First let's check that XVideo extension is available */
  if (!XQueryExtension (xcontext->disp, "XVideo", &i, &i, &i)) {
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, SETTINGS,
        ("Could not initialise Xv output"),
        ("XVideo extension is not available"));
    return NULL;
  }

  /* Then we get adaptors list */
  if (Success != XvQueryAdaptors (xcontext->disp, xcontext->root,
          &xcontext->nb_adaptors, &adaptors)) {
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, SETTINGS,
        ("Could not initialise Xv output"),
        ("Failed getting XV adaptors list"));
    return NULL;
  }

  xcontext->xv_port_id = 0;

  GST_DEBUG_OBJECT(xvimagesink, "Found %u XV adaptor(s)", xcontext->nb_adaptors);

  xcontext->adaptors =
      (gchar **) g_malloc0 (xcontext->nb_adaptors * sizeof (gchar *));

  /* Now fill up our adaptor name array */
  for (i = 0; i < xcontext->nb_adaptors; i++) {
    xcontext->adaptors[i] = g_strdup (adaptors[i].name);
  }

  if (xvimagesink->adaptor_no >= 0 &&
      xvimagesink->adaptor_no < xcontext->nb_adaptors) {
    /* Find xv port from user defined adaptor */
#ifdef GST_EXT_XV_ENHANCEMENT
    if(!xvimagesink->subpicture)
#endif
      gst_lookup_xv_port_from_adaptor (xcontext, adaptors, xvimagesink->adaptor_no);
#ifdef GST_EXT_XV_ENHANCEMENT
    else
      gst_lookup_xv_port_for_subtitle (xcontext, adaptors, xvimagesink->adaptor_no, xcontext->nb_adaptors);
#endif
  }

  if (!xcontext->xv_port_id) {
    /* Now search for an adaptor that supports XvImageMask */
    for (i = 0; i < xcontext->nb_adaptors && !xcontext->xv_port_id; i++) {
#ifdef GST_EXT_XV_ENHANCEMENT
      if(!xvimagesink->subpicture)
#endif
        gst_lookup_xv_port_from_adaptor (xcontext, adaptors, i);
#ifdef GST_EXT_XV_ENHANCEMENT
      else
        gst_lookup_xv_port_for_subtitle (xcontext, adaptors, i, xcontext->nb_adaptors);
#endif
      xvimagesink->adaptor_no = i;
    }
  }

  XvFreeAdaptorInfo (adaptors);

  if (!xcontext->xv_port_id) {
    xvimagesink->adaptor_no = -1;
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, BUSY,
        ("Could not initialise Xv output"), ("No port available"));
    return NULL;
  }

  /* Set XV_AUTOPAINT_COLORKEY and XV_DOUBLE_BUFFER and XV_COLORKEY */
  {
    int count, todo = 3;
    XvAttribute *const attr = XvQueryPortAttributes (xcontext->disp,
        xcontext->xv_port_id, &count);
    static const char autopaint[] = "XV_AUTOPAINT_COLORKEY";
    static const char dbl_buffer[] = "XV_DOUBLE_BUFFER";
    static const char colorkey[] = "XV_COLORKEY";

    GST_DEBUG_OBJECT (xvimagesink, "Checking %d Xv port attributes", count);

    xvimagesink->have_autopaint_colorkey = FALSE;
    xvimagesink->have_double_buffer = FALSE;
    xvimagesink->have_colorkey = FALSE;

    for (i = 0; ((i < count) && todo); i++)
      if (!strcmp (attr[i].name, autopaint)) {
        const Atom atom = XInternAtom (xcontext->disp, autopaint, False);

        /* turn on autopaint colorkey */
        XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom,
            (xvimagesink->autopaint_colorkey ? 1 : 0));
        todo--;
        xvimagesink->have_autopaint_colorkey = TRUE;
      } else if (!strcmp (attr[i].name, dbl_buffer)) {
        const Atom atom = XInternAtom (xcontext->disp, dbl_buffer, False);

        XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom,
            (xvimagesink->double_buffer ? 1 : 0));
        todo--;
        xvimagesink->have_double_buffer = TRUE;
      } else if (!strcmp (attr[i].name, colorkey)) {
        /* Set the colorkey, default is something that is dark but hopefully
         * won't randomly appear on the screen elsewhere (ie not black or greys)
         * can be overridden by setting "colorkey" property
         */
        const Atom atom = XInternAtom (xcontext->disp, colorkey, False);
        guint32 ckey = 0;
        gboolean set_attr = TRUE;
        guint cr, cg, cb;

        /* set a colorkey in the right format RGB565/RGB888
         * We only handle these 2 cases, because they're the only types of
         * devices we've encountered. If we don't recognise it, leave it alone
         */
        cr = (xvimagesink->colorkey >> 16);
        cg = (xvimagesink->colorkey >> 8) & 0xFF;
        cb = (xvimagesink->colorkey) & 0xFF;
        switch (xcontext->depth) {
          case 16:             /* RGB 565 */
            cr >>= 3;
            cg >>= 2;
            cb >>= 3;
            ckey = (cr << 11) | (cg << 5) | cb;
            break;
          case 24:
          case 32:             /* RGB 888 / ARGB 8888 */
            ckey = (cr << 16) | (cg << 8) | cb;
            break;
          default:
            GST_DEBUG_OBJECT (xvimagesink,
                "Unknown bit depth %d for Xv Colorkey - not adjusting",
                xcontext->depth);
            set_attr = FALSE;
            break;
        }

        if (set_attr) {
          ckey = CLAMP (ckey, (guint32) attr[i].min_value,
              (guint32) attr[i].max_value);
          GST_LOG_OBJECT (xvimagesink,
              "Setting color key for display depth %d to 0x%x",
              xcontext->depth, ckey);

          XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom,
              (gint) ckey);
        }
        todo--;
        xvimagesink->have_colorkey = TRUE;
      }

    XFree (attr);
  }

  /* Get the list of encodings supported by the adapter and look for the
   * XV_IMAGE encoding so we can determine the maximum width and height
   * supported */
  XvQueryEncodings (xcontext->disp, xcontext->xv_port_id, &nb_encodings,
      &encodings);

  for (i = 0; i < nb_encodings; i++) {
    GST_LOG_OBJECT (xvimagesink,
        "Encoding %d, name %s, max wxh %lux%lu rate %d/%d",
        i, encodings[i].name, encodings[i].width, encodings[i].height,
        encodings[i].rate.numerator, encodings[i].rate.denominator);
    if (strcmp (encodings[i].name, "XV_IMAGE") == 0) {
      max_w = encodings[i].width;
      max_h = encodings[i].height;
#ifdef GST_EXT_XV_ENHANCEMENT
      xvimagesink->scr_w = max_w;
      xvimagesink->scr_h = max_h;
#endif /* GST_EXT_XV_ENHANCEMENT */
    }
  }

  XvFreeEncodingInfo (encodings);
#ifdef GST_EXT_XV_ENHANCEMENT
if (!xvimagesink->subpicture) {
#endif
  /* We get all image formats supported by our port */
  formats = XvListImageFormats (xcontext->disp,
      xcontext->xv_port_id, &nb_formats);
  caps = gst_caps_new_empty ();
  for (i = 0; i < nb_formats; i++) {
    GstCaps *format_caps = NULL;
    gboolean is_rgb_format = FALSE;

    /* We set the image format of the xcontext to an existing one. This
       is just some valid image format for making our xshm calls check before
       caps negotiation really happens. */
    xcontext->im_format = formats[i].id;

    switch (formats[i].type) {
      case XvRGB:
      {
        XvImageFormatValues *fmt = &(formats[i]);
        gint endianness = G_BIG_ENDIAN;

        if (fmt->byte_order == LSBFirst) {
          /* our caps system handles 24/32bpp RGB as big-endian. */
          if (fmt->bits_per_pixel == 24 || fmt->bits_per_pixel == 32) {
            fmt->red_mask = GUINT32_TO_BE (fmt->red_mask);
            fmt->green_mask = GUINT32_TO_BE (fmt->green_mask);
            fmt->blue_mask = GUINT32_TO_BE (fmt->blue_mask);

            if (fmt->bits_per_pixel == 24) {
              fmt->red_mask >>= 8;
              fmt->green_mask >>= 8;
              fmt->blue_mask >>= 8;
            }
          } else
            endianness = G_LITTLE_ENDIAN;
        }

        format_caps = gst_caps_new_simple ("video/x-raw-rgb",
#ifdef GST_EXT_XV_ENHANCEMENT
            "format", GST_TYPE_FOURCC, formats[i].id,
#endif /* GST_EXT_XV_ENHANCEMENT */
            "endianness", G_TYPE_INT, endianness,
            "depth", G_TYPE_INT, fmt->depth,
            "bpp", G_TYPE_INT, fmt->bits_per_pixel,
            "red_mask", G_TYPE_INT, fmt->red_mask,
            "green_mask", G_TYPE_INT, fmt->green_mask,
            "blue_mask", G_TYPE_INT, fmt->blue_mask,
            "width", GST_TYPE_INT_RANGE, 1, max_w,
            "height", GST_TYPE_INT_RANGE, 1, max_h,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

        is_rgb_format = TRUE;
        break;
      }
      case XvYUV:
        format_caps = gst_caps_new_simple ("video/x-raw-yuv",
            "format", GST_TYPE_FOURCC, formats[i].id,
            "width", GST_TYPE_INT_RANGE, 1, max_w,
            "height", GST_TYPE_INT_RANGE, 1, max_h,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    if (format_caps) {
      GstXvImageFormat *format = NULL;

      format = g_new0 (GstXvImageFormat, 1);
      if (format) {
        format->format = formats[i].id;
        format->caps = gst_caps_copy (format_caps);
        xcontext->formats_list = g_list_append (xcontext->formats_list, format);
      }

      if (is_rgb_format) {
        if (rgb_caps == NULL)
          rgb_caps = format_caps;
        else
          gst_caps_append (rgb_caps, format_caps);
      } else
        gst_caps_append (caps, format_caps);
    }
  }

  /* Collected all caps into either the caps or rgb_caps structures.
   * Append rgb_caps on the end of YUV, so that YUV is always preferred */
  if (rgb_caps)
    gst_caps_append (caps, rgb_caps);

  if (formats)
    XFree (formats);

  GST_DEBUG ("Generated the following caps: %" GST_PTR_FORMAT, caps);

  if (gst_caps_is_empty (caps)) {
    gst_caps_unref (caps);
    XvUngrabPort (xcontext->disp, xcontext->xv_port_id, 0);
    GST_ELEMENT_ERROR (xvimagesink, STREAM, WRONG_TYPE, (NULL),
        ("No supported format found"));
    return NULL;
  }
#ifdef GST_EXT_XV_ENHANCEMENT
} //subpicture
#endif

  return caps;
}

static gpointer
gst_xvimagesink_event_thread (GstXvImageSink * xvimagesink)
{
  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  GST_OBJECT_LOCK (xvimagesink);
  while (xvimagesink->running) {
    GST_OBJECT_UNLOCK (xvimagesink);

    if (xvimagesink->xwindow) {
      gst_xvimagesink_handle_xevents (xvimagesink);
    }

#ifdef GST_EXT_XV_ENHANCEMENT
    g_usleep (_EVENT_THREAD_CHECK_INTERVAL);
#else /* GST_EXT_XV_ENHANCEMENT */
    /* FIXME: do we want to align this with the framerate or anything else? */
    g_usleep (G_USEC_PER_SEC / 20);
#endif /* GST_EXT_XV_ENHANCEMENT */

    GST_OBJECT_LOCK (xvimagesink);
  }
  GST_OBJECT_UNLOCK (xvimagesink);

  return NULL;
}

static void
gst_xvimagesink_manage_event_thread (GstXvImageSink * xvimagesink)
{
  GThread *thread = NULL;

  /* don't start the thread too early */
  if (xvimagesink->xcontext == NULL) {
    return;
  }

  GST_OBJECT_LOCK (xvimagesink);
  if (xvimagesink->handle_expose || xvimagesink->handle_events) {
    if (!xvimagesink->event_thread) {
      /* Setup our event listening thread */
      GST_DEBUG_OBJECT (xvimagesink, "run xevent thread, expose %d, events %d",
          xvimagesink->handle_expose, xvimagesink->handle_events);
      xvimagesink->running = TRUE;
#if !GLIB_CHECK_VERSION (2, 31, 0)
      xvimagesink->event_thread = g_thread_create (
          (GThreadFunc) gst_xvimagesink_event_thread, xvimagesink, TRUE, NULL);
#else
      xvimagesink->event_thread = g_thread_try_new ("xvimagesink-events",
          (GThreadFunc) gst_xvimagesink_event_thread, xvimagesink, NULL);
#endif
    }
  } else {
    if (xvimagesink->event_thread) {
      GST_DEBUG_OBJECT (xvimagesink, "stop xevent thread, expose %d, events %d",
          xvimagesink->handle_expose, xvimagesink->handle_events);
      xvimagesink->running = FALSE;
      /* grab thread and mark it as NULL */
      thread = xvimagesink->event_thread;
      xvimagesink->event_thread = NULL;
    }
  }
  GST_OBJECT_UNLOCK (xvimagesink);

  /* Wait for our event thread to finish */
  if (thread)
    g_thread_join (thread);

}


#ifdef GST_EXT_XV_ENHANCEMENT
/**
 * gst_xvimagesink_prepare_xid:
 * @overlay: a #GstXOverlay which does not yet have an XWindow or XPixmap.
 *
 * This will post a "prepare-xid" element message with video size and display size on the bus
 * to give applications an opportunity to call
 * gst_x_overlay_set_xwindow_id() before a plugin creates its own
 * window or pixmap.
 *
 * This function should only be used by video overlay plugin developers.
 */
static void
gst_xvimagesink_prepare_xid (GstXOverlay * overlay)
{
  GstStructure *s;
  GstMessage *msg;

  g_return_if_fail (overlay != NULL);
  g_return_if_fail (GST_IS_X_OVERLAY (overlay));

  GstXvImageSink *xvimagesink;
  xvimagesink = GST_XVIMAGESINK (GST_OBJECT (overlay));

  GST_DEBUG ("post \"prepare-xid\" element message with video-width(%d), video-height(%d), display-width(%d), display-height(%d)",
        GST_VIDEO_SINK_WIDTH (xvimagesink), GST_VIDEO_SINK_HEIGHT (xvimagesink), xvimagesink->xcontext->width, xvimagesink->xcontext->height);

  GST_LOG_OBJECT (GST_OBJECT (overlay), "prepare xid");
  s = gst_structure_new ("prepare-xid",
        "video-width", G_TYPE_INT, GST_VIDEO_SINK_WIDTH (xvimagesink),
        "video-height", G_TYPE_INT, GST_VIDEO_SINK_HEIGHT (xvimagesink),
        "display-width", G_TYPE_INT, xvimagesink->xcontext->width,
        "display-height", G_TYPE_INT, xvimagesink->xcontext->height,
        NULL);
  msg = gst_message_new_element (GST_OBJECT (overlay), s);
  gst_element_post_message (GST_ELEMENT (overlay), msg);
}
#endif /* GST_EXT_XV_ENHANCEMENT */


/* This function calculates the pixel aspect ratio based on the properties
 * in the xcontext structure and stores it there. */
static void
gst_xvimagesink_calculate_pixel_aspect_ratio (GstXContext * xcontext)
{
  static const gint par[][2] = {
    {1, 1},                     /* regular screen */
    {16, 15},                   /* PAL TV */
    {11, 10},                   /* 525 line Rec.601 video */
#ifdef GST_EXT_XV_ENHANCEMENT
    {44, 46},                   /* Gear S Curved Display */
#endif
    {54, 59},                   /* 625 line Rec.601 video */
    {64, 45},                   /* 1280x1024 on 16:9 display */
    {5, 3},                     /* 1280x1024 on 4:3 display */
    {4, 3}                      /*  800x600 on 16:9 display */
  };
  gint i;
  gint index;
  gdouble ratio;
  gdouble delta;

#define DELTA(idx) (ABS (ratio - ((gdouble) par[idx][0] / par[idx][1])))

  /* first calculate the "real" ratio based on the X values;
   * which is the "physical" w/h divided by the w/h in pixels of the display */
  ratio = (gdouble) (xcontext->widthmm * xcontext->height)
      / (xcontext->heightmm * xcontext->width);

  /* DirectFB's X in 720x576 reports the physical dimensions wrong, so
   * override here */
  if (xcontext->width == 720 && xcontext->height == 576) {
    ratio = 4.0 * 576 / (3.0 * 720);
  }
  GST_DEBUG ("calculated pixel aspect ratio: %f", ratio);
  /* now find the one from par[][2] with the lowest delta to the real one */
  delta = DELTA (0);
  index = 0;

  for (i = 1; i < sizeof (par) / (sizeof (gint) * 2); ++i) {
    gdouble this_delta = DELTA (i);

    if (this_delta < delta) {
      index = i;
      delta = this_delta;
    }
  }

  GST_DEBUG ("Decided on index %d (%d/%d)", index,
      par[index][0], par[index][1]);

  g_free (xcontext->par);
  xcontext->par = g_new0 (GValue, 1);
  g_value_init (xcontext->par, GST_TYPE_FRACTION);
  gst_value_set_fraction (xcontext->par, par[index][0], par[index][1]);
  GST_DEBUG ("set xcontext PAR to %d/%d",
      gst_value_get_fraction_numerator (xcontext->par),
      gst_value_get_fraction_denominator (xcontext->par));
}

/* This function gets the X Display and global info about it. Everything is
   stored in our object and will be cleaned when the object is disposed. Note
   here that caps for supported format are generated without any window or
   image creation */
static GstXContext *
gst_xvimagesink_xcontext_get (GstXvImageSink * xvimagesink)
{
  GstXContext *xcontext = NULL;
  XPixmapFormatValues *px_formats = NULL;
  gint nb_formats = 0, i, j, N_attr;
  XvAttribute *xv_attr = NULL;
  Atom prop_atom;
  const char *channels[4] = { "XV_HUE", "XV_SATURATION",
    "XV_BRIGHTNESS", "XV_CONTRAST"
  };

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  xcontext = g_new0 (GstXContext, 1);
  xcontext->im_format = 0;

  g_mutex_lock (xvimagesink->x_lock);

  xcontext->disp = XOpenDisplay (xvimagesink->display_name);
  if (!xcontext->disp) {
    g_mutex_unlock (xvimagesink->x_lock);
    g_free (xcontext);
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
        ("Could not initialise Xv output"), ("Could not open display"));
    return NULL;
  }

#ifdef GST_EXT_XV_ENHANCEMENT
  {
    int i = 0;
    for (i = 0; i < MAX_DISPLAY_IN_XVIMAGESINK; i++) {
      if (g_display_id[i] == NULL) {
        g_display_id[i] = xcontext->disp;
        GST_INFO_OBJECT(xvimagesink, "x display array[%d] = display(0x%x)", i, xcontext->disp);
        break;
      }
    }
    if (i == MAX_DISPLAY_IN_XVIMAGESINK) {
      GST_WARNING_OBJECT(xvimagesink, "out of index(%d) for x display array", i);
    }
  }
#endif

  xcontext->screen = DefaultScreenOfDisplay (xcontext->disp);
  xcontext->screen_num = DefaultScreen (xcontext->disp);
  xcontext->visual = DefaultVisual (xcontext->disp, xcontext->screen_num);
  xcontext->root = DefaultRootWindow (xcontext->disp);
  xcontext->white = XWhitePixel (xcontext->disp, xcontext->screen_num);
  xcontext->black = XBlackPixel (xcontext->disp, xcontext->screen_num);
  xcontext->depth = DefaultDepthOfScreen (xcontext->screen);

  xcontext->width = DisplayWidth (xcontext->disp, xcontext->screen_num);
  xcontext->height = DisplayHeight (xcontext->disp, xcontext->screen_num);
  xcontext->widthmm = DisplayWidthMM (xcontext->disp, xcontext->screen_num);
  xcontext->heightmm = DisplayHeightMM (xcontext->disp, xcontext->screen_num);

  GST_DEBUG_OBJECT (xvimagesink, "X reports %dx%d pixels and %d mm x %d mm",
      xcontext->width, xcontext->height, xcontext->widthmm, xcontext->heightmm);

  gst_xvimagesink_calculate_pixel_aspect_ratio (xcontext);
  /* We get supported pixmap formats at supported depth */
  px_formats = XListPixmapFormats (xcontext->disp, &nb_formats);

  if (!px_formats) {
#ifdef GST_EXT_XV_ENHANCEMENT
  {
    int i = 0;
    for (i = 0; i < MAX_DISPLAY_IN_XVIMAGESINK; i++) {
      if (g_display_id[i] == xcontext->disp) {
        g_display_id[i] = NULL;
      }
    }
  }
#endif
    XCloseDisplay (xcontext->disp);
    g_mutex_unlock (xvimagesink->x_lock);
    g_free (xcontext->par);
    g_free (xcontext);
    GST_ELEMENT_ERROR (xvimagesink, RESOURCE, SETTINGS,
        ("Could not initialise Xv output"), ("Could not get pixel formats"));
    return NULL;
  }

  /* We get bpp value corresponding to our running depth */
  for (i = 0; i < nb_formats; i++) {
    if (px_formats[i].depth == xcontext->depth)
      xcontext->bpp = px_formats[i].bits_per_pixel;
  }

  XFree (px_formats);

  xcontext->endianness =
      (ImageByteOrder (xcontext->disp) ==
      LSBFirst) ? G_LITTLE_ENDIAN : G_BIG_ENDIAN;

  /* our caps system handles 24/32bpp RGB as big-endian. */
  if ((xcontext->bpp == 24 || xcontext->bpp == 32) &&
      xcontext->endianness == G_LITTLE_ENDIAN) {
    xcontext->endianness = G_BIG_ENDIAN;
    xcontext->visual->red_mask = GUINT32_TO_BE (xcontext->visual->red_mask);
    xcontext->visual->green_mask = GUINT32_TO_BE (xcontext->visual->green_mask);
    xcontext->visual->blue_mask = GUINT32_TO_BE (xcontext->visual->blue_mask);
    if (xcontext->bpp == 24) {
      xcontext->visual->red_mask >>= 8;
      xcontext->visual->green_mask >>= 8;
      xcontext->visual->blue_mask >>= 8;
    }
  }

  xcontext->caps = gst_xvimagesink_get_xv_support (xvimagesink, xcontext);

  if (!xcontext->caps
#ifdef GST_EXT_XV_ENHANCEMENT
    && !xvimagesink->subpicture
#endif
    ) {
#ifdef GST_EXT_XV_ENHANCEMENT
  {
    int i = 0;
    for (i = 0; i < MAX_DISPLAY_IN_XVIMAGESINK; i++) {
      if (g_display_id[i] == xcontext->disp) {
        g_display_id[i] = NULL;
      }
    }
  }
#endif
    XCloseDisplay (xcontext->disp);
    g_mutex_unlock (xvimagesink->x_lock);
    g_free (xcontext->par);
    g_free (xcontext);
    /* GST_ELEMENT_ERROR is thrown by gst_xvimagesink_get_xv_support */
    return NULL;
  }
#ifdef HAVE_XSHM
#ifdef GST_EXT_XV_ENHANCEMENT
  if (!xvimagesink->subpicture) {
#endif //GST_EXT_XV_ENHANCEMENT
    /* Search for XShm extension support */
    if (XShmQueryExtension (xcontext->disp) &&
        gst_xvimagesink_check_xshm_calls (xcontext)) {
      xcontext->use_xshm = TRUE;
      GST_DEBUG ("xvimagesink is using XShm extension");
    } else
#endif /* HAVE_XSHM */
    {
      xcontext->use_xshm = FALSE;
      GST_DEBUG ("xvimagesink is not using XShm extension");
    }

    xv_attr = XvQueryPortAttributes (xcontext->disp,
        xcontext->xv_port_id, &N_attr);
#ifdef GST_EXT_XV_ENHANCEMENT
  }
#endif //GST_EXT_XV_ENHANCEMENT

  /* Generate the channels list */
  for (i = 0; i < (sizeof (channels) / sizeof (char *)); i++) {
    XvAttribute *matching_attr = NULL;

    /* Retrieve the property atom if it exists. If it doesn't exist,
     * the attribute itself must not either, so we can skip */
    prop_atom = XInternAtom (xcontext->disp, channels[i], True);
    if (prop_atom == None)
      continue;

    if (xv_attr != NULL) {
      for (j = 0; j < N_attr && matching_attr == NULL; ++j)
        if (!g_ascii_strcasecmp (channels[i], xv_attr[j].name))
          matching_attr = xv_attr + j;
    }

    if (matching_attr) {
      GstColorBalanceChannel *channel;

      channel = g_object_new (GST_TYPE_COLOR_BALANCE_CHANNEL, NULL);
      channel->label = g_strdup (channels[i]);
      channel->min_value = matching_attr->min_value;
      channel->max_value = matching_attr->max_value;

      xcontext->channels_list = g_list_append (xcontext->channels_list,
          channel);

      /* If the colorbalance settings have not been touched we get Xv values
         as defaults and update our internal variables */
      if (!xvimagesink->cb_changed) {
        gint val;

        XvGetPortAttribute (xcontext->disp, xcontext->xv_port_id,
            prop_atom, &val);
        /* Normalize val to [-1000, 1000] */
        val = floor (0.5 + -1000 + 2000 * (val - channel->min_value) /
            (double) (channel->max_value - channel->min_value));

        if (!g_ascii_strcasecmp (channels[i], "XV_HUE"))
          xvimagesink->hue = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_SATURATION"))
          xvimagesink->saturation = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_BRIGHTNESS"))
          xvimagesink->brightness = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_CONTRAST"))
          xvimagesink->contrast = val;
      }
    }
  }
#ifdef GST_EXT_XV_ENHANCEMENT
  if (!xvimagesink->subpicture) {
#endif
  if (xv_attr)
    XFree (xv_attr);
#ifdef GST_EXT_XV_ENHANCEMENT
  }
  if(!xvimagesink->subpicture) {
    set_display_mode(xvimagesink->xcontext, xvimagesink->display_mode);
    set_csc_range(xcontext, xvimagesink->csc_range);
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_mutex_unlock (xvimagesink->x_lock);

  return xcontext;
}

/* This function cleans the X context. Closing the Display, releasing the XV
   port and unrefing the caps for supported formats. */
static void
gst_xvimagesink_xcontext_clear (GstXvImageSink * xvimagesink)
{
  GList *formats_list, *channels_list = NULL;
  GstXContext *xcontext;
  gint i = 0;

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  GST_OBJECT_LOCK (xvimagesink);
  if (xvimagesink->xcontext == NULL) {
    GST_OBJECT_UNLOCK (xvimagesink);
    return;
  }

  /* Take the XContext from the sink and clean it up */
  xcontext = xvimagesink->xcontext;
  xvimagesink->xcontext = NULL;

  GST_OBJECT_UNLOCK (xvimagesink);

#ifdef GST_EXT_XV_ENHANCEMENT
  if (!xvimagesink->subpicture)  {
#endif
  formats_list = xcontext->formats_list;

  while (formats_list) {
    GstXvImageFormat *format = formats_list->data;

    gst_caps_unref (format->caps);
    g_free (format);
    formats_list = g_list_next (formats_list);
  }

  if (xcontext->formats_list)
    g_list_free (xcontext->formats_list);

  channels_list = xcontext->channels_list;
#ifdef GST_EXT_XV_ENHANCEMENT
  }
#endif
  while (channels_list) {
    GstColorBalanceChannel *channel = channels_list->data;

    g_object_unref (channel);
    channels_list = g_list_next (channels_list);
  }

  if (xcontext->channels_list)
    g_list_free (xcontext->channels_list);

  gst_caps_unref (xcontext->caps);
  if (xcontext->last_caps)
    gst_caps_replace (&xcontext->last_caps, NULL);

  for (i = 0; i < xcontext->nb_adaptors; i++) {
    g_free (xcontext->adaptors[i]);
  }

  g_free (xcontext->adaptors);

  g_free (xcontext->par);

  g_mutex_lock (xvimagesink->x_lock);

  GST_DEBUG_OBJECT (xvimagesink, "Closing display and freeing X Context");

  XvUngrabPort (xcontext->disp, xcontext->xv_port_id, 0);

#ifdef GST_EXT_XV_ENHANCEMENT
  {
    int i = 0;
    for (i = 0; i < MAX_DISPLAY_IN_XVIMAGESINK; i++) {
      if (g_display_id[i] == xcontext->disp) {
        g_display_id[i] = NULL;
      }
    }
  }
#endif
  XCloseDisplay (xcontext->disp);
  g_mutex_unlock (xvimagesink->x_lock);

  g_free (xcontext);
}

static void
gst_xvimagesink_imagepool_clear (GstXvImageSink * xvimagesink)
{
  g_mutex_lock (xvimagesink->pool_lock);

  while (xvimagesink->image_pool) {
    GstXvImageBuffer *xvimage = xvimagesink->image_pool->data;

    xvimagesink->image_pool = g_slist_delete_link (xvimagesink->image_pool,
        xvimagesink->image_pool);
    gst_xvimage_buffer_free (xvimage);
  }

  g_mutex_unlock (xvimagesink->pool_lock);
}

/* Element stuff */

/* This function tries to get a format matching with a given caps in the
   supported list of formats we generated in gst_xvimagesink_get_xv_support */
static gint
gst_xvimagesink_get_format_from_caps (GstXvImageSink * xvimagesink,
    GstCaps * caps)
{
  GList *list = NULL;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), 0);

  list = xvimagesink->xcontext->formats_list;

  while (list) {
    GstXvImageFormat *format = list->data;

    if (format) {
      if (gst_caps_can_intersect (caps, format->caps)) {
        return format->format;
      }
    }
    list = g_list_next (list);
  }

  return -1;
}

static GstCaps *
gst_xvimagesink_getcaps (GstBaseSink * bsink)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (bsink);
  if (xvimagesink->xcontext
#ifdef GST_EXT_XV_ENHANCEMENT
    && !xvimagesink->subpicture
#endif
) {
    return gst_caps_ref (xvimagesink->xcontext->caps);
  }
  return
      gst_caps_copy (gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD
          (xvimagesink)));
}

static gboolean
gst_xvimagesink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstXvImageSink *xvimagesink;
  GstStructure *structure;
  guint32 im_format = 0;
  gboolean ret;
  gint video_width, video_height;
  gint disp_x, disp_y;
  gint disp_width, disp_height;
  gint video_par_n, video_par_d;        /* video's PAR */
  gint display_par_n, display_par_d;    /* display's PAR */
  const GValue *caps_par;
  const GValue *caps_disp_reg;
  const GValue *fps;
  guint num, den;
#ifdef GST_EXT_XV_ENHANCEMENT
  gboolean subtitle;
  gchar *str_in = gst_caps_to_string(caps);
  if(str_in == NULL) {
    GST_ERROR("gst_caps_to_string() returns NULL...");
  }else{
    GST_INFO("In setcaps. incaps:%s", str_in);
    g_free (str_in);
    str_in = NULL;
  }
#endif /* #ifdef GST_EXT_XV_ENHANCEMENT */

  xvimagesink = GST_XVIMAGESINK (bsink);

  GST_DEBUG_OBJECT (xvimagesink,
      "In setcaps. Possible caps %" GST_PTR_FORMAT ", setting caps %"
      GST_PTR_FORMAT, xvimagesink->xcontext->caps, caps);

  if (!gst_caps_can_intersect (xvimagesink->xcontext->caps, caps)
#ifdef GST_EXT_XV_ENHANCEMENT
    && !xvimagesink->subpicture
#endif
    )
    goto incompatible_caps;

  structure = gst_caps_get_structure (caps, 0);
  ret = gst_structure_get_int (structure, "width", &video_width);
  ret &= gst_structure_get_int (structure, "height", &video_height);
  fps = gst_structure_get_value (structure, "framerate");
  ret &= (fps != NULL);
#ifdef GST_EXT_XV_ENHANCEMENT
  if(gst_structure_get_boolean (structure, "subtitle", &subtitle) && xvimagesink->subpicture)
  {
    xvimagesink->is_subpicture_format = TRUE;
    GST_LOG("It is type of subpicture and subtitle.");
  }
#endif
  if (!ret)
    goto incomplete_caps;

#ifdef GST_EXT_XV_ENHANCEMENT
  xvimagesink->aligned_width = video_width;
  xvimagesink->aligned_height = video_height;

#ifdef GST_EXT_ENABLE_HEVC
  /*get combine prop of hevc*/
  if(gst_structure_get_int (structure, "yuvcombine", &(xvimagesink->need_combine_data)))
  {
    GST_INFO_OBJECT(xvimagesink, "need combine data : %d", xvimagesink->need_combine_data);
  }
  else
  {
    /*Not need to combine data, just directly copy*/
    xvimagesink->need_combine_data = 0;
  }
#endif
  _remove_last_buffer(xvimagesink);
#endif /* GST_EXT_XV_ENHANCEMENT */

  xvimagesink->fps_n = gst_value_get_fraction_numerator (fps);
  xvimagesink->fps_d = gst_value_get_fraction_denominator (fps);

  xvimagesink->video_width = video_width;
  xvimagesink->video_height = video_height;
#ifdef GST_EXT_XV_ENHANCEMENT
  if (!xvimagesink->subpicture) {
#endif
  im_format = gst_xvimagesink_get_format_from_caps (xvimagesink, caps);
  if (im_format == -1)
    goto invalid_format;
#ifdef GST_EXT_XV_ENHANCEMENT
  }
#endif
  /* get aspect ratio from caps if it's present, and
   * convert video width and height to a display width and height
   * using wd / hd = wv / hv * PARv / PARd */

  /* get video's PAR */
  caps_par = gst_structure_get_value (structure, "pixel-aspect-ratio");
  if (caps_par) {
    video_par_n = gst_value_get_fraction_numerator (caps_par);
    video_par_d = gst_value_get_fraction_denominator (caps_par);
  } else {
    video_par_n = 1;
    video_par_d = 1;
  }
  /* get display's PAR */
  if (xvimagesink->par) {
    display_par_n = gst_value_get_fraction_numerator (xvimagesink->par);
    display_par_d = gst_value_get_fraction_denominator (xvimagesink->par);
  } else {
    display_par_n = 1;
    display_par_d = 1;
  }

  /* get the display region */
  caps_disp_reg = gst_structure_get_value (structure, "display-region");
  if (caps_disp_reg) {
    disp_x = g_value_get_int (gst_value_array_get_value (caps_disp_reg, 0));
    disp_y = g_value_get_int (gst_value_array_get_value (caps_disp_reg, 1));
    disp_width = g_value_get_int (gst_value_array_get_value (caps_disp_reg, 2));
    disp_height =
        g_value_get_int (gst_value_array_get_value (caps_disp_reg, 3));
  } else {
    disp_x = disp_y = 0;
    disp_width = video_width;
    disp_height = video_height;
  }

  if (!gst_video_calculate_display_ratio (&num, &den, video_width,
          video_height, video_par_n, video_par_d, display_par_n, display_par_d))
    goto no_disp_ratio;

  xvimagesink->disp_x = disp_x;
  xvimagesink->disp_y = disp_y;
  xvimagesink->disp_width = disp_width;
  xvimagesink->disp_height = disp_height;

  GST_DEBUG_OBJECT (xvimagesink,
      "video width/height: %dx%d, calculated display ratio: %d/%d",
      video_width, video_height, num, den);

  /* now find a width x height that respects this display ratio.
   * prefer those that have one of w/h the same as the incoming video
   * using wd / hd = num / den */

  /* start with same height, because of interlaced video */
  /* check hd / den is an integer scale factor, and scale wd with the PAR */
  if (video_height % den == 0) {
    GST_DEBUG_OBJECT (xvimagesink, "keeping video height");
    GST_VIDEO_SINK_WIDTH (xvimagesink) = (guint)
        gst_util_uint64_scale_int (video_height, num, den);
    GST_VIDEO_SINK_HEIGHT (xvimagesink) = video_height;
  } else if (video_width % num == 0) {
    GST_DEBUG_OBJECT (xvimagesink, "keeping video width");
    GST_VIDEO_SINK_WIDTH (xvimagesink) = video_width;
    GST_VIDEO_SINK_HEIGHT (xvimagesink) = (guint)
        gst_util_uint64_scale_int (video_width, den, num);
  } else {
    GST_DEBUG_OBJECT (xvimagesink, "approximating while keeping video height");
    GST_VIDEO_SINK_WIDTH (xvimagesink) = (guint)
        gst_util_uint64_scale_int (video_height, num, den);
    GST_VIDEO_SINK_HEIGHT (xvimagesink) = video_height;
  }
  GST_DEBUG_OBJECT (xvimagesink, "scaling to %dx%d",
      GST_VIDEO_SINK_WIDTH (xvimagesink), GST_VIDEO_SINK_HEIGHT (xvimagesink));

  /* Notify application to set xwindow id now */
  g_mutex_lock (xvimagesink->flow_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
  if (!xvimagesink->xwindow && !xvimagesink->get_pixmap_cb) {
    g_mutex_unlock (xvimagesink->flow_lock);
    gst_xvimagesink_prepare_xid (GST_X_OVERLAY (xvimagesink));
#else
  if (!xvimagesink->xwindow) {
    g_mutex_unlock (xvimagesink->flow_lock);
    gst_x_overlay_prepare_xwindow_id (GST_X_OVERLAY (xvimagesink));
#endif
  } else {
    g_mutex_unlock (xvimagesink->flow_lock);
  }
#ifdef GST_EXT_XV_ENHANCEMENT
if (!xvimagesink->is_subpicture_format) {
#endif
    /* Creating our window and our image with the display size in pixels */
    if (GST_VIDEO_SINK_WIDTH (xvimagesink) <= 0 ||
        GST_VIDEO_SINK_HEIGHT (xvimagesink) <= 0)
      goto no_display_size;

    g_mutex_lock (xvimagesink->flow_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
    if (!xvimagesink->xwindow && !xvimagesink->get_pixmap_cb) {
      GST_DEBUG_OBJECT (xvimagesink, "xwindow is null and not multi-pixmaps usage case");
#else
    if (!xvimagesink->xwindow) {
#endif
      xvimagesink->xwindow = gst_xvimagesink_xwindow_new (xvimagesink,
          GST_VIDEO_SINK_WIDTH (xvimagesink),
          GST_VIDEO_SINK_HEIGHT (xvimagesink));
    }
  }

  /* After a resize, we want to redraw the borders in case the new frame size
   * doesn't cover the same area */
  xvimagesink->redraw_border = TRUE;

  /* We renew our xvimage only if size or format changed;
   * the xvimage is the same size as the video pixel size */
  if ((xvimagesink->xvimage) &&
      ((im_format != xvimagesink->xvimage->im_format) ||
          (video_width != xvimagesink->xvimage->width) ||
          (video_height != xvimagesink->xvimage->height)) &&
          (!xvimagesink->subpicture)) {
    GST_DEBUG_OBJECT (xvimagesink,
        "old format %" GST_FOURCC_FORMAT ", new format %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (xvimagesink->xvimage->im_format),
        GST_FOURCC_ARGS (im_format));
    GST_DEBUG_OBJECT (xvimagesink, "renewing xvimage");
    gst_buffer_unref (GST_BUFFER (xvimagesink->xvimage));
    xvimagesink->xvimage = NULL;
  }

#ifdef GST_EXT_XV_ENHANCEMENT
  /* In case of starting player with connecting external display, we have to check status.
   * It will be unconditionally executed. */
  if(!xvimagesink->is_subpicture_format)
    check_hdmi_connected(xvimagesink);
#endif
  g_mutex_unlock (xvimagesink->flow_lock);

  return TRUE;

  /* ERRORS */
incompatible_caps:
  {
    GST_ERROR_OBJECT (xvimagesink, "caps incompatible");
    return FALSE;
  }
incomplete_caps:
  {
    GST_DEBUG_OBJECT (xvimagesink, "Failed to retrieve either width, "
        "height or framerate from intersected caps");
    return FALSE;
  }
invalid_format:
  {
    GST_DEBUG_OBJECT (xvimagesink,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
no_disp_ratio:
  {
    GST_ELEMENT_ERROR (xvimagesink, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }
no_display_size:
  {
    GST_ELEMENT_ERROR (xvimagesink, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }
#ifdef GST_EXT_XV_ENHANCEMENT
}
#endif

static GstStateChangeReturn
gst_xvimagesink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstXvImageSink *xvimagesink;
  GstXContext *xcontext = NULL;
#ifdef GST_EXT_XV_ENHANCEMENT
  Atom atom_preemption = None;
#endif /* GST_EXT_XV_ENHANCEMENT */

  xvimagesink = GST_XVIMAGESINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("NULL_TO_READY start");
#endif /* GST_EXT_XV_ENHANCEMENT */
      /* Initializing the XContext */
      if (xvimagesink->xcontext == NULL) {
        xcontext = gst_xvimagesink_xcontext_get (xvimagesink);
        if (xcontext == NULL)
          return GST_STATE_CHANGE_FAILURE;
        GST_OBJECT_LOCK (xvimagesink);
        if (xcontext)
          xvimagesink->xcontext = xcontext;
        GST_OBJECT_UNLOCK (xvimagesink);
      }

      /* update object's par with calculated one if not set yet */
      if (!xvimagesink->par) {
        xvimagesink->par = g_new0 (GValue, 1);
        gst_value_init_and_copy (xvimagesink->par, xvimagesink->xcontext->par);
        GST_DEBUG_OBJECT (xvimagesink, "set calculated PAR on object's PAR");
      }
      /* call XSynchronize with the current value of synchronous */
      GST_DEBUG_OBJECT (xvimagesink, "XSynchronize called with %s",
          xvimagesink->synchronous ? "TRUE" : "FALSE");
      XSynchronize (xvimagesink->xcontext->disp, xvimagesink->synchronous);
      gst_xvimagesink_update_colorbalance (xvimagesink);
      gst_xvimagesink_manage_event_thread (xvimagesink);
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("NULL_TO_READY done");
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("READY_TO_PAUSED start");
#endif /* GST_EXT_XV_ENHANCEMENT */
      g_mutex_lock (xvimagesink->pool_lock);
      xvimagesink->pool_invalid = FALSE;
      g_mutex_unlock (xvimagesink->pool_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("READY_TO_PAUSED done");
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
#ifdef GST_EXT_XV_ENHANCEMENT
      if(vconf_set_int(VCONFKEY_XV_STATE, (xvimagesink->eos_received << 2) | XV_STATUS_PLAYING))
        GST_WARNING("vconf set fail");
      GST_WARNING("PAUSED_TO_PLAYING done");
      xvimagesink->is_during_seek = FALSE;
      xvimagesink->keep_external_fullscreen_prev = FALSE;
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("PAUSED_TO_READY start");
#endif /* GST_EXT_XV_ENHANCEMENT */
      g_mutex_lock (xvimagesink->pool_lock);
      xvimagesink->pool_invalid = TRUE;
      g_mutex_unlock (xvimagesink->pool_lock);
#ifdef GST_EXT_XV_ENHANCEMENT
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("PLAYING_TO_PAUSED start");
      g_mutex_lock (xvimagesink->flow_lock);
      /* init displayed buffer count */
      xvimagesink->displayed_buffer_count = 0;

      g_mutex_lock (xvimagesink->x_lock);
      if ((xvimagesink->is_hided || xvimagesink->is_quick_panel_on || xvimagesink->is_multi_window) && _is_connected_to_external_display(xvimagesink) && !xvimagesink->keep_external_fullscreen_prev) {
        GST_WARNING_OBJECT(xvimagesink, "release external display mode");
        XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id,
                     xvimagesink->xwindow->win);
        XSync(xvimagesink->xcontext->disp, FALSE);
        xvimagesink->skip_frame_due_to_external_dev = TRUE;
      }
      if((xvimagesink->is_hided_subpicture || xvimagesink->is_quick_panel_on_subpicture || xvimagesink->is_multi_window_subpicture)
        && xvimagesink->is_subpicture_format && xvimagesink->pixmap_for_subpicture) {
        GST_WARNING_OBJECT(xvimagesink, "calling XvStopVideo() for %d port [pixmap : %p]", xvimagesink->xcontext->xv_port_id, xvimagesink->pixmap_for_subpicture);
        XvStopVideo(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->pixmap_for_subpicture);
        XSync(xvimagesink->xcontext->disp, FALSE);
      }
      g_mutex_unlock (xvimagesink->x_lock);
      g_mutex_unlock (xvimagesink->flow_lock);
      if(vconf_set_int(VCONFKEY_XV_STATE, (xvimagesink->eos_received << 2) | XV_STATUS_PAUSED))
        GST_WARNING("vconf set fail");
      GST_WARNING("PLAYING_TO_PAUSED done");
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING_OBJECT(xvimagesink, "PAUSED_TO_READY start - %d %d %d %p",
                                      xvimagesink->is_zero_copy_format,
                                      xvimagesink->enable_flush_buffer,
                                      xvimagesink->secure_path,
                                      xvimagesink->get_pixmap_cb);

      if ((xvimagesink->enable_flush_buffer == FALSE ||
           xvimagesink->secure_path == SECURE_PATH_ON) &&
          xvimagesink->xcontext && xvimagesink->xwindow) {
        GST_WARNING_OBJECT(xvimagesink, "Call XvStopVideo and remove last buffer");
        g_mutex_lock(xvimagesink->x_lock);
        XvStopVideo(xvimagesink->xcontext->disp,
                    xvimagesink->xcontext->xv_port_id,
                    xvimagesink->xwindow->win);
        XSync(xvimagesink->xcontext->disp, FALSE);
        g_mutex_unlock(xvimagesink->x_lock);
        _remove_last_buffer(xvimagesink);
      } else if (xvimagesink->is_zero_copy_format &&
                 xvimagesink->xvimage &&
                 xvimagesink->xvimage->xvimage &&
                 !xvimagesink->get_pixmap_cb) {
        if (gst_xvimagesink_make_flush_buffer(xvimagesink)) {
          int i = 0;
          XV_DATA_PTR img_data = (XV_DATA_PTR) xvimagesink->xvimage->xvimage->data;
          memset(img_data, 0x0, sizeof(XV_DATA));
          XV_INIT_DATA(img_data);

          img_data->bo[0] = xvimagesink->flush_buffer->bo[0];
          img_data->bo[1] = xvimagesink->flush_buffer->bo[1];
          img_data->bo[2] = xvimagesink->flush_buffer->bo[2];

          gst_xvimagesink_xvimage_put(xvimagesink, xvimagesink->xvimage);

          GST_WARNING_OBJECT(xvimagesink, "gst_xvimagesink_xvimage_put done");

          /* check whether putimage is succeeded or not */
          g_mutex_lock(xvimagesink->display_buffer_lock);

          for (i = 0 ; i < DISPLAYING_BUFFERS_MAX_NUM ; i++) {
            if (xvimagesink->displaying_buffers[i].gem_name[0] > 0) {
              if ((img_data->dmabuf_fd[0] > 0 &&
                   xvimagesink->displaying_buffers[i].dmabuf_fd[0] == img_data->dmabuf_fd[0] &&
                   xvimagesink->displaying_buffers[i].dmabuf_fd[1] == img_data->dmabuf_fd[1] &&
                   xvimagesink->displaying_buffers[i].dmabuf_fd[2] == img_data->dmabuf_fd[2]) ||
                  (img_data->bo[0] &&
                   xvimagesink->displaying_buffers[i].bo[0] == img_data->bo[0] &&
                   xvimagesink->displaying_buffers[i].bo[1] == img_data->bo[1] &&
                   xvimagesink->displaying_buffers[i].bo[2] == img_data->bo[2])) {
                GST_WARNING_OBJECT(xvimagesink, "found flush buffer in displaying_buffers");
                break;
              }
            }
          }

          if (i >= DISPLAYING_BUFFERS_MAX_NUM) {
            GST_WARNING_OBJECT(xvimagesink, "flush buffer is not existed in displaying_buffers");
            _release_flush_buffer(xvimagesink);
          } else {
            _remove_last_buffer(xvimagesink);
          }

          g_mutex_unlock(xvimagesink->display_buffer_lock);
        }
      }
#endif /* GST_EXT_XV_ENHANCEMENT */
      xvimagesink->fps_n = 0;
      xvimagesink->fps_d = 1;
      GST_VIDEO_SINK_WIDTH (xvimagesink) = 0;
      GST_VIDEO_SINK_HEIGHT (xvimagesink) = 0;
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("PAUSED_TO_READY done");
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
#ifdef GST_EXT_XV_ENHANCEMENT
      GST_WARNING("READY_TO_NULL start");
#endif /* GST_EXT_XV_ENHANCEMENT */
      gst_xvimagesink_reset (xvimagesink);
#ifdef GST_EXT_XV_ENHANCEMENT
      /* close drm */
      drm_fini(xvimagesink);
      /* init displaying_buffer_count */
      xvimagesink->displaying_buffer_count = 0;
      GST_WARNING("READY_TO_NULL done");
#endif /* GST_EXT_XV_ENHANCEMENT */
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_xvimagesink_get_times (GstBaseSink * bsink, GstBuffer * buf,
    GstClockTime * start, GstClockTime * end)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (bsink);

  if (GST_BUFFER_TIMESTAMP_IS_VALID (buf)) {
    *start = GST_BUFFER_TIMESTAMP (buf);
    if (GST_BUFFER_DURATION_IS_VALID (buf)) {
      *end = *start + GST_BUFFER_DURATION (buf);
    } else {
      if (xvimagesink->fps_n > 0) {
        *end = *start +
            gst_util_uint64_scale_int (GST_SECOND, xvimagesink->fps_d,
            xvimagesink->fps_n);
      }
    }
  }
}

#ifdef GST_EXT_ENABLE_HEVC
static void
gst_xvimagesink_combine_i420_scmn_data(GstXvImageSink *xvimagesink, GstBuffer *buf)
{
    unsigned int outsize = 0;
    unsigned char *temp_outbuf = xvimagesink->xvimage->xvimage->data;
    int cnt = 0, j = 0;
    unsigned char *temp_imgb;
    SCMN_IMGB *imgb = NULL;
    int stride, w, h, i;

    GST_DEBUG("Begin");
    imgb = (SCMN_IMGB *)GST_BUFFER_DATA(buf);
    if(imgb == NULL || imgb->a[0] == NULL)
    {
        return;
    }

    stride = GST_ROUND_UP_4 (imgb->w[0]);
    h = imgb->h[0];
    w = imgb->w[0];
    /*Y plane copy */
    for (i = 0; i < h; i++)
    {
        memcpy (temp_outbuf + i * stride, imgb->a[0] + i * imgb->s[0], w);
    }

    temp_outbuf += GST_ROUND_UP_4 (imgb->w[0]) * GST_ROUND_UP_2 (imgb->h[0]);

    stride = GST_ROUND_UP_4 (GST_ROUND_UP_2 (imgb->w[0]) / 2);
    h = GST_ROUND_UP_2 (imgb->h[0]) / 2;
    w = GST_ROUND_UP_2 (imgb->w[0]) / 2;
    /* Cb plane copy */
    for (i = 0; i < h; i++)
    {
        memcpy (temp_outbuf + i * stride, imgb->a[1] + i * imgb->s[1], w);
    }

    temp_outbuf += GST_ROUND_UP_4 (GST_ROUND_UP_2 (imgb->w[0]) / 2) * (GST_ROUND_UP_2 (imgb->h[0]) / 2);
    /* Same stride, height, width as above */
    /* Cr plane copy */
    for (i = 0; i < h; i++)
    {
        memcpy (temp_outbuf + i * stride, imgb->a[2] + i * imgb->s[2], w);
    }

    outsize  = imgb->w[0] * imgb->h[0]* 3 >> 1;
    xvimagesink->xvimage->size = MIN(outsize, xvimagesink->xvimage->size);

    GST_DEBUG("End");
}
#endif

#ifdef GST_EXT_XV_ENHANCEMENT
static gboolean gst_xvimagesink_make_flush_buffer(GstXvImageSink *xvimagesink)
{
  GstXvImageFlushBuffer *flush_buffer = NULL;
  GstXvImageDisplayingBuffer *display_buffer = NULL;
  tbm_bo bo = NULL;
  int size = 0;
  int i = 0;
  int ret = 0;

  if (xvimagesink == NULL) {
    GST_ERROR("handle is NULL");
    return FALSE;
  }

  if (xvimagesink->last_added_buffer_index == -1) {
    GST_WARNING_OBJECT(xvimagesink, "there is no remained buffer");
    return FALSE;
  }

  if (xvimagesink->drm_fd < 0 || xvimagesink->bufmgr == NULL) {
    GST_ERROR_OBJECT(xvimagesink, "drm fd[%d] or bufmgr[%p] is invalid",
                                  xvimagesink->drm_fd, xvimagesink->bufmgr);
    return FALSE;
  }

  flush_buffer = (GstXvImageFlushBuffer *)malloc(sizeof(GstXvImageFlushBuffer));
  if (flush_buffer == NULL) {
    GST_ERROR_OBJECT(xvimagesink, "GstXvImageFlushBuffer alloc failed");
    return FALSE;
  }

  memset(flush_buffer, 0x0, sizeof(GstXvImageFlushBuffer));

  display_buffer = &(xvimagesink->displaying_buffers[xvimagesink->last_added_buffer_index]);
  GST_WARNING_OBJECT(xvimagesink, "last_added_buffer_index [%d]",
                                  xvimagesink->last_added_buffer_index);

  for (i = 0 ; i < XV_BUF_PLANE_NUM ; i++) {
    if (display_buffer->bo[i]) {
      tbm_bo_handle vaddr_src;
      tbm_bo_handle vaddr_dst;

      /* get bo size */
      size = tbm_bo_size(display_buffer->bo[i]);

      /* alloc bo */
      bo = tbm_bo_alloc(xvimagesink->bufmgr, size, TBM_BO_DEFAULT);
      if (bo == NULL) {
        GST_ERROR_OBJECT(xvimagesink, "bo alloc[%d] failed", size);
        goto FLUSH_BUFFER_FAILED;
      }

      GST_WARNING_OBJECT(xvimagesink, "[%d] bo %p, size %d alloc done", i, bo, size);

      flush_buffer->gem_name[i] = tbm_bo_export(bo);
      flush_buffer->bo[i] = bo;

      /* get virtual address */
      vaddr_src = tbm_bo_map(display_buffer->bo[i], TBM_DEVICE_CPU, TBM_OPTION_READ|TBM_OPTION_WRITE);
      vaddr_dst = tbm_bo_map(bo, TBM_DEVICE_CPU, TBM_OPTION_READ|TBM_OPTION_WRITE);
      if (vaddr_src.ptr == NULL || vaddr_dst.ptr == NULL) {
        GST_WARNING_OBJECT(xvimagesink, "get vaddr failed src %p, dst %p",
                                        vaddr_src.ptr, vaddr_dst.ptr);
        if (vaddr_src.ptr) {
          tbm_bo_unmap(display_buffer->bo[i]);
        }
        if (vaddr_dst.ptr) {
          tbm_bo_unmap(bo);
        }
        goto FLUSH_BUFFER_FAILED;
      }

      /* copy buffer */
      memcpy(vaddr_dst.ptr, vaddr_src.ptr, size);

      tbm_bo_unmap(display_buffer->bo[i]);
      tbm_bo_unmap(bo);

      GST_WARNING_OBJECT(xvimagesink, "[%d] copy done", i);

      xvimagesink->flush_buffer = flush_buffer;

      ret = TRUE;
    }
  }

  return ret;

FLUSH_BUFFER_FAILED:
  if (flush_buffer) {
    for (i = 0 ; i < XV_BUF_PLANE_NUM ; i++) {
      if (flush_buffer->bo[i]) {
        tbm_bo_unref(flush_buffer->bo[i]);
        flush_buffer->bo[i] = NULL;
      }
    }
    free(flush_buffer);
    flush_buffer = NULL;
  }

  return FALSE;
}
#endif /* GST_EXT_XV_ENHANCEMENT */


static GstFlowReturn
gst_xvimagesink_show_frame (GstVideoSink * vsink, GstBuffer * buf)
{
  GstXvImageSink *xvimagesink;

#ifdef GST_EXT_XV_ENHANCEMENT
  XV_DATA_PTR img_data = NULL;
  SCMN_IMGB *scmn_imgb = NULL;
  gint format = 0;
  gboolean ret = FALSE;
  int res = -1;
  int (*handler) (Display *, XErrorEvent *) = NULL;
  Atom atom_overlay;
#endif /* GST_EXT_XV_ENHANCEMENT */

  xvimagesink = GST_XVIMAGESINK (vsink);

#ifdef GST_EXT_XV_ENHANCEMENT
  if (xvimagesink->stop_video) {
    GST_INFO( "Stop video is TRUE. so skip show frame..." );
    return GST_FLOW_OK;
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  /* If this buffer has been allocated using our buffer management we simply
     put the ximage which is in the PRIVATE pointer */
  if (GST_IS_XVIMAGE_BUFFER (buf)) {
    GST_LOG_OBJECT (xvimagesink, "fast put of bufferpool buffer %p", buf);
#ifdef GST_EXT_XV_ENHANCEMENT
    xvimagesink->xid_updated = FALSE;
#endif /* GST_EXT_XV_ENHANCEMENT */
    if (!gst_xvimagesink_xvimage_put (xvimagesink,
            GST_XVIMAGE_BUFFER_CAST (buf)))
      goto no_window;
  } else {
    GST_CAT_LOG_OBJECT (GST_CAT_PERFORMANCE, xvimagesink,
        "slow copy into bufferpool buffer %p", buf);
    /* Else we have to copy the data into our private image, */
    /* if we have one... */
#ifdef GST_EXT_XV_ENHANCEMENT
    g_mutex_lock (xvimagesink->flow_lock);
    if (xvimagesink->skip_frame_due_to_external_dev) {
      GST_WARNING_OBJECT( xvimagesink, "skip_frame_due_to_external_dev is TRUE. so skip show frame..." );
      xvimagesink->skip_frame_due_to_external_dev = FALSE;
      g_mutex_unlock (xvimagesink->flow_lock);
      return GST_FLOW_OK;
    }

#endif /* GST_EXT_XV_ENHANCEMENT */
      if (!xvimagesink->xvimage
#ifdef GST_EXT_XV_ENHANCEMENT
      && !xvimagesink->is_subpicture_format
#endif
        ) {
        GST_DEBUG_OBJECT (xvimagesink, "creating our xvimage");

#ifdef GST_EXT_XV_ENHANCEMENT
      format = gst_xvimagesink_get_format_from_caps(xvimagesink, GST_BUFFER_CAPS(buf));
      switch (format) {
        case GST_MAKE_FOURCC('S', 'T', '1', '2'):
        case GST_MAKE_FOURCC('S', 'N', '1', '2'):
        case GST_MAKE_FOURCC('S', 'N', '2', '1'):
        case GST_MAKE_FOURCC('S', '4', '2', '0'):
        case GST_MAKE_FOURCC('S', 'U', 'Y', '2'):
        case GST_MAKE_FOURCC('S', 'U', 'Y', 'V'):
        case GST_MAKE_FOURCC('S', 'Y', 'V', 'Y'):
        case GST_MAKE_FOURCC('I', 'T', 'L', 'V'):
        case GST_MAKE_FOURCC('S', 'R', '3', '2'):
        case GST_MAKE_FOURCC('S', 'V', '1', '2'):
          xvimagesink->is_zero_copy_format = TRUE;
          scmn_imgb = (SCMN_IMGB *)GST_BUFFER_MALLOCDATA(buf);
          if(scmn_imgb == NULL) {
            GST_DEBUG_OBJECT( xvimagesink, "scmn_imgb is NULL. Skip xvimage put..." );
            g_mutex_unlock (xvimagesink->flow_lock);
            return GST_FLOW_OK;
          }

          /* skip buffer if aligned size is smaller than size of caps */
          if (scmn_imgb->s[0] < xvimagesink->video_width ||
              scmn_imgb->e[0] < xvimagesink->video_height) {
            GST_WARNING_OBJECT(xvimagesink, "invalid size[caps:%dx%d,aligned:%dx%d]. Skip this buffer...",
                                            xvimagesink->video_width, xvimagesink->video_height,
                                            scmn_imgb->s[0], scmn_imgb->e[0]);
            g_mutex_unlock (xvimagesink->flow_lock);
            return GST_FLOW_OK;
          }

          xvimagesink->aligned_width = scmn_imgb->s[0];
          xvimagesink->aligned_height = scmn_imgb->e[0];
          GST_INFO_OBJECT(xvimagesink, "Use aligned width,height[%dx%d]",
                                       xvimagesink->aligned_width, xvimagesink->aligned_height);
          break;
        default:
          xvimagesink->is_zero_copy_format = FALSE;
          GST_INFO_OBJECT(xvimagesink, "Use original width,height of caps");
          break;
      }

      GST_INFO("zero copy format - %d", xvimagesink->is_zero_copy_format);
#endif /* GST_EXT_XV_ENHANCEMENT */

      xvimagesink->xvimage = gst_xvimagesink_xvimage_new (xvimagesink,
          GST_BUFFER_CAPS (buf));

      if (!xvimagesink->xvimage)
        /* The create method should have posted an informative error */
        goto no_image;

#ifdef GST_EXT_XV_ENHANCEMENT
      if ((xvimagesink->is_zero_copy_format == FALSE &&
           xvimagesink->xvimage->size < GST_BUFFER_SIZE (buf)) ||
          (xvimagesink->is_zero_copy_format &&
           xvimagesink->xvimage->size < sizeof(SCMN_IMGB))) {
#else /* GST_EXT_XV_ENHANCEMENT */
      if (xvimagesink->xvimage->size < GST_BUFFER_SIZE (buf)) {
#endif /* GST_EXT_XV_ENHANCEMENT */
        GST_ELEMENT_ERROR (xvimagesink, RESOURCE, WRITE,
            ("Failed to create output image buffer of %dx%d pixels",
                xvimagesink->xvimage->width, xvimagesink->xvimage->height),
            ("XServer allocated buffer size did not match input buffer"));

        gst_xvimage_buffer_destroy (xvimagesink->xvimage);
        xvimagesink->xvimage = NULL;
        goto no_image;
      }
    }

#ifdef GST_EXT_XV_ENHANCEMENT
    if (xvimagesink->is_zero_copy_format) {
      /* Cases for specified formats of Samsung extension */
        GST_LOG("Samsung EXT format - fourcc:%c%c%c%c, display mode:%d, Rotate angle:%d",
                xvimagesink->xvimage->im_format, xvimagesink->xvimage->im_format>>8,
                xvimagesink->xvimage->im_format>>16, xvimagesink->xvimage->im_format>>24,
                xvimagesink->display_mode, xvimagesink->rotate_angle);

        if (xvimagesink->xvimage->xvimage->data) {
          int i = 0;
          img_data = (XV_DATA_PTR) xvimagesink->xvimage->xvimage->data;
          memset(img_data, 0x0, sizeof(XV_DATA));
          XV_INIT_DATA(img_data);

          scmn_imgb = (SCMN_IMGB *)GST_BUFFER_MALLOCDATA(buf);
          if (scmn_imgb == NULL) {
            GST_DEBUG_OBJECT( xvimagesink, "scmn_imgb is NULL. Skip xvimage put..." );
            g_mutex_unlock (xvimagesink->flow_lock);
            return GST_FLOW_OK;
          }

          /* Keep the vaddr of current image for copying last image */
          if (scmn_imgb->buf_share_method != BUF_SHARE_METHOD_FLUSH_BUFFER) {
            for(i = 0; i < SCMN_IMGB_MAX_PLANE; i++) {
              xvimagesink->last_image_vaddr[i] = (unsigned int)scmn_imgb->a[i];
            }
            GST_LOG("Vaddr - YBuf[0x%x], CbBuf[0x%x], CrBuf[0x%x]",
              xvimagesink->last_image_vaddr[0], xvimagesink->last_image_vaddr[1], xvimagesink->last_image_vaddr[2]);
          }

          if (scmn_imgb->buf_share_method == BUF_SHARE_METHOD_PADDR) {
            img_data->YBuf = (unsigned int)scmn_imgb->p[0];
            img_data->CbBuf = (unsigned int)scmn_imgb->p[1];
            img_data->CrBuf = (unsigned int)scmn_imgb->p[2];
            img_data->BufType = XV_BUF_TYPE_LEGACY;

            GST_DEBUG("YBuf[0x%x], CbBuf[0x%x], CrBuf[0x%x]",
                      img_data->YBuf, img_data->CbBuf, img_data->CrBuf );
          } else if (scmn_imgb->buf_share_method == BUF_SHARE_METHOD_FD ||
                     scmn_imgb->buf_share_method == BUF_SHARE_METHOD_TIZEN_BUFFER) {
            gboolean do_set_secure = FALSE;

            /* open drm to use gem */
            if (xvimagesink->drm_fd < 0) {
              drm_init(xvimagesink);
            }

            if (scmn_imgb->buf_share_method == BUF_SHARE_METHOD_FD) {
              /* keep dma-buf fd. fd will be converted in gst_xvimagesink_xvimage_put */
              img_data->dmabuf_fd[0] = scmn_imgb->dmabuf_fd[0];
              img_data->dmabuf_fd[1] = scmn_imgb->dmabuf_fd[1];
              img_data->dmabuf_fd[2] = scmn_imgb->dmabuf_fd[2];
              img_data->BufType = XV_BUF_TYPE_DMABUF;
              GST_DEBUG("DMABUF fd %u,%u,%u", img_data->dmabuf_fd[0], img_data->dmabuf_fd[1], img_data->dmabuf_fd[2]);
            } else {
              /* keep bo. bo will be converted in gst_xvimagesink_xvimage_put */
              img_data->bo[0] = scmn_imgb->bo[0];
              img_data->bo[1] = scmn_imgb->bo[1];
              img_data->bo[2] = scmn_imgb->bo[2];
              GST_DEBUG("TBM bo %p %p %p", img_data->bo[0], img_data->bo[1], img_data->bo[2]);
            }

            /* check secure contents path */
            if (scmn_imgb->tz_enable) {
              if (xvimagesink->secure_path != SECURE_PATH_ON) {
                xvimagesink->secure_path = SECURE_PATH_ON;
                do_set_secure = TRUE;
              }
            } else {
              if (xvimagesink->secure_path != SECURE_PATH_OFF) {
                xvimagesink->secure_path = SECURE_PATH_OFF;
                do_set_secure = TRUE;
              }
            }
            static gboolean is_exist = FALSE;
            gchar *attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_SECURE");
            is_exist = check_supportable_port_attr(xvimagesink, attr_name);
            g_free(attr_name);

            if (do_set_secure && is_exist) {
              Atom atom_secure = None;
              g_mutex_lock (xvimagesink->x_lock);
              atom_secure = XInternAtom(xvimagesink->xcontext->disp, "_USER_WM_PORT_ATTRIBUTE_SECURE", False);
              if (atom_secure != None) {
                if (XvSetPortAttribute(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_secure, xvimagesink->secure_path) != Success) {
                  GST_ERROR_OBJECT(xvimagesink, "%d: XvSetPortAttribute: secure setting failed.", atom_secure);
                } else {
                  GST_WARNING_OBJECT(xvimagesink, "set contents path [%d] (0:NORMAL, 1:SECURE)", xvimagesink->secure_path);
                }
                XSync (xvimagesink->xcontext->disp, FALSE);
              } else {
                GST_ERROR_OBJECT(xvimagesink, "_USER_WM_PORT_ATTRIBUTE_SECURE is not existed");
              }
              g_mutex_unlock (xvimagesink->x_lock);
            }

            is_exist = FALSE;
            attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_DRM_LEVEL");
            is_exist = check_supportable_port_attr(xvimagesink, attr_name);
            g_free(attr_name);


            if (xvimagesink->drm_level && is_exist) {
              Atom atom_drm = None;
              g_mutex_lock (xvimagesink->x_lock);
              atom_drm = XInternAtom( xvimagesink->xcontext->disp,
                                          "_USER_WM_PORT_ATTRIBUTE_DRM_LEVEL", False);
              if (atom_drm != None) {
                GST_INFO_OBJECT(xvimagesink, "DRM LEVEL -> 1");
                if (XvSetPortAttribute(xvimagesink->xcontext->disp,
                                     xvimagesink->xcontext->xv_port_id,
                                     atom_drm, xvimagesink->drm_level ) != Success) {
                  GST_WARNING_OBJECT( xvimagesink, "Set DRM LEVEL 1 failed" );
                }
                XSync (xvimagesink->xcontext->disp, FALSE);
                g_mutex_unlock (xvimagesink->x_lock);
                xvimagesink->drm_level = DRM_LEVEL_0;
              }
            }

            /* set current buffer */
            xvimagesink->xvimage->current_buffer = buf;
          } else if (scmn_imgb->buf_share_method == BUF_SHARE_METHOD_FLUSH_BUFFER) {
            /* Flush Buffer, we are going to push a new buffer for recieving return buffer event from X */
            GST_WARNING_OBJECT(xvimagesink, "BUF_SHARE_METHOD_FLUSH_BUFFER case");
            if (gst_xvimagesink_make_flush_buffer(xvimagesink)) {
              img_data->bo[0] = xvimagesink->flush_buffer->bo[0];
              img_data->bo[1] = xvimagesink->flush_buffer->bo[1];
              img_data->bo[2] = xvimagesink->flush_buffer->bo[2];
            } else {
              g_mutex_unlock(xvimagesink->flow_lock);
              return GST_FLOW_OK;
            }
          } else {
            GST_WARNING("unknown buf_share_method type [%d]. skip xvimage put...",
                        scmn_imgb->buf_share_method);
            g_mutex_unlock (xvimagesink->flow_lock);
            return GST_FLOW_OK;
          }
        } else {
          GST_WARNING_OBJECT( xvimagesink, "xvimage->data is NULL. skip xvimage put..." );
          g_mutex_unlock (xvimagesink->flow_lock);
          return GST_FLOW_OK;
        }
    } else if (xvimagesink->is_subpicture_format) {
      GC gc;
      xvimagesink->pixmap_for_subpicture = (Pixmap)GST_BUFFER_DATA(buf);
      if(!xvimagesink->pixmap_for_subpicture) {
        GST_ERROR("no pixmap");
        g_mutex_unlock (xvimagesink->flow_lock);
        return GST_FLOW_OK;
      }
      if(xvimagesink->video_width!=xvimagesink->external_width || xvimagesink->video_height!=xvimagesink->external_height) {
        GST_ERROR("pixmap's size and current resolution are different");
        g_mutex_unlock (xvimagesink->flow_lock);
        return GST_FLOW_OK;
      }
      gc = XCreateGC (xvimagesink->xcontext->disp, xvimagesink->pixmap_for_subpicture, 0, 0);

      GST_WARNING_OBJECT(xvimagesink, "xvimagesink pixmap ID : %p, port : %ld, GC : %p", xvimagesink->pixmap_for_subpicture,
        xvimagesink->xcontext->xv_port_id, gc);

      /* set error handler */
      error_caught = FALSE;
      handler = XSetErrorHandler(gst_xvimagesink_handle_xerror);

      if(!xvimagesink->set_overlay_for_subpicture_just_once)
      {
        GST_LOG("setting attribute overlay");
        atom_overlay = XInternAtom (xvimagesink->xcontext->disp, "_USER_WM_PORT_ATTRIBUTE_OVERLAY", FALSE);
        XvSetPortAttribute (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_overlay, 1);
        XSync (xvimagesink->xcontext->disp, FALSE);
        xvimagesink->set_overlay_for_subpicture_just_once = TRUE;
      }
      res = XvGetStill(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id,
        xvimagesink->pixmap_for_subpicture, gc, 0, 0, xvimagesink->external_width, xvimagesink->external_height,
        0, 0, xvimagesink->external_width, xvimagesink->external_height);
      XSync (xvimagesink->xcontext->disp, FALSE);

      GST_WARNING_OBJECT(xvimagesink, "BUFFER TS=%" GST_TIME_FORMAT ", DUR=%" GST_TIME_FORMAT ", SIZE=%d\n",
                          GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buf)),
                          GST_TIME_ARGS(GST_BUFFER_DURATION(buf)),
                          GST_BUFFER_SIZE(buf));
      if(gc) {
        GST_DEBUG("FreeGC");
        XFreeGC (xvimagesink->xcontext->disp, gc);
      }
      if (error_caught)
        GST_WARNING_OBJECT(xvimagesink, "XvGetStill error");
      else
        GST_WARNING_OBJECT(xvimagesink, "XvGetStill %s  ==> (width : %d, height : %d)", res == 0 ? "SUCCESS" : "FAIL", xvimagesink->external_width, xvimagesink->external_height);

      /* Reset error handler */
      if (handler) {
        error_caught = FALSE;
        XSetErrorHandler (handler);
      }
    } else {
        GST_DEBUG("Normal format activated. fourcc = %d", xvimagesink->xvimage->im_format);

#ifdef GST_EXT_ENABLE_HEVC
        if(xvimagesink->need_combine_data == 1)
        {
            gst_xvimagesink_combine_i420_scmn_data(xvimagesink, buf);
        }
        else
#endif
        {
            memcpy (xvimagesink->xvimage->xvimage->data,
            GST_BUFFER_DATA (buf),
            MIN (GST_BUFFER_SIZE (buf), xvimagesink->xvimage->size));
        }
    }

    g_mutex_unlock (xvimagesink->flow_lock);
    ret = gst_xvimagesink_xvimage_put(xvimagesink, xvimagesink->xvimage);
    if (!ret && !xvimagesink->is_subpicture_format) {
      goto no_window;
    }
#else /* GST_EXT_XV_ENHANCEMENT */
    memcpy (xvimagesink->xvimage->xvimage->data,
        GST_BUFFER_DATA (buf),
        MIN (GST_BUFFER_SIZE (buf), xvimagesink->xvimage->size));

    if (!gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->xvimage))
      goto no_window;
#endif /* GST_EXT_XV_ENHANCEMENT */
  }

  return GST_FLOW_OK;

  /* ERRORS */
no_image:
  {
    /* No image available. That's very bad ! */
    GST_WARNING_OBJECT (xvimagesink, "could not create image");
#ifdef GST_EXT_XV_ENHANCEMENT
    g_mutex_unlock (xvimagesink->flow_lock);
#endif
    return GST_FLOW_ERROR;
  }
no_window:
  {
    /* No Window available to put our image into */
    GST_WARNING_OBJECT (xvimagesink, "could not output image - no window");
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_xvimagesink_event (GstBaseSink * sink, GstEvent * event)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (sink);
  if(GST_EVENT_TYPE (event) == GST_EVENT_NEWSEGMENT)
  {
      if(vconf_set_int(VCONFKEY_XV_STATE, (xvimagesink->eos_received << 2) | XV_STATUS_SEEK))
        GST_WARNING("vconf set fail");
  }

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:{
      GstTagList *l;
      gchar *title = NULL;

      gst_event_parse_tag (event, &l);
      gst_tag_list_get_string (l, GST_TAG_TITLE, &title);

      if (title) {
#ifdef GST_EXT_XV_ENHANCEMENT
        if (!xvimagesink->get_pixmap_cb) {
#endif
        GST_DEBUG_OBJECT (xvimagesink, "got tags, title='%s'", title);
        gst_xvimagesink_xwindow_set_title (xvimagesink, xvimagesink->xwindow,
            title);

        g_free (title);
#ifdef GST_EXT_XV_ENHANCEMENT
        }
#endif
      }
      break;
    }
#ifdef GST_EXT_XV_ENHANCEMENT
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (xvimagesink, "flush start");
      break;
    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (xvimagesink, "flush stop");
      xvimagesink->is_during_seek = TRUE;
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    {
      const GstStructure *st = NULL;
      st = gst_event_get_structure (event);
      if(!st) {
        GST_WARNING_OBJECT (xvimagesink, "could not get structure for custom downstream event");
      } else {
        if (gst_structure_has_name (st, "Content_Is_DRM_Playready")) {
          GST_INFO_OBJECT (xvimagesink, "got a event for DRM playready");
          xvimagesink->drm_level = DRM_LEVEL_1;
          if (xvimagesink->drm_level && xvimagesink->xcontext) {
            static gboolean is_exist = FALSE;
            gchar *attr_name = g_strdup("_USER_WM_PORT_ATTRIBUTE_DRM_LEVEL");
            is_exist = check_supportable_port_attr(xvimagesink, attr_name);
            g_free(attr_name);

            if(is_exist)
            {
              Atom atom_drm = None;
              g_mutex_lock (xvimagesink->x_lock);
              atom_drm = XInternAtom( xvimagesink->xcontext->disp,
                                          "_USER_WM_PORT_ATTRIBUTE_DRM_LEVEL", False);
              if (atom_drm != None) {
                GST_INFO_OBJECT(xvimagesink, "DRM LEVEL -> 1");
                if (XvSetPortAttribute(xvimagesink->xcontext->disp,
                                     xvimagesink->xcontext->xv_port_id,
                                     atom_drm, xvimagesink->drm_level ) != Success) {
                  GST_WARNING_OBJECT( xvimagesink, "Set DRM LEVEL 1 failed" );
                }
                XSync (xvimagesink->xcontext->disp, FALSE);
                xvimagesink->drm_level = DRM_LEVEL_0;
              }
              g_mutex_unlock (xvimagesink->x_lock);
            }
          }
        }
      }
      break;
    }
    case GST_EVENT_EOS:
      xvimagesink->eos_received = TRUE;
      GST_DEBUG_OBJECT(xvimagesink, "got eos");
      break;
    case GST_EVENT_NEWSEGMENT:
      xvimagesink->eos_received = FALSE;
      GST_DEBUG_OBJECT(xvimagesink, "got newsegment event");
      break;
#endif
    default:
      break;
  }
  if (GST_BASE_SINK_CLASS (parent_class)->event)
    return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
  else
    return TRUE;
}

/* Buffer management */

static GstCaps *
gst_xvimage_sink_different_size_suggestion (GstXvImageSink * xvimagesink,
    GstCaps * caps)
{
  GstCaps *intersection;
  GstCaps *new_caps;
  GstStructure *s;
  gint width, height;
  gint par_n = 1, par_d = 1;
  gint dar_n, dar_d;
  gint w, h;

  new_caps = gst_caps_copy (caps);

  s = gst_caps_get_structure (new_caps, 0);

  gst_structure_get_int (s, "width", &width);
  gst_structure_get_int (s, "height", &height);
  gst_structure_get_fraction (s, "pixel-aspect-ratio", &par_n, &par_d);

  gst_structure_remove_field (s, "width");
  gst_structure_remove_field (s, "height");
  gst_structure_remove_field (s, "pixel-aspect-ratio");

  intersection = gst_caps_intersect (xvimagesink->xcontext->caps, new_caps);
  gst_caps_unref (new_caps);

  if (gst_caps_is_empty (intersection))
    return intersection;

  s = gst_caps_get_structure (intersection, 0);

  gst_util_fraction_multiply (width, height, par_n, par_d, &dar_n, &dar_d);

  /* xvimagesink supports all PARs */

  gst_structure_fixate_field_nearest_int (s, "width", width);
  gst_structure_fixate_field_nearest_int (s, "height", height);
  gst_structure_get_int (s, "width", &w);
  gst_structure_get_int (s, "height", &h);

  gst_util_fraction_multiply (h, w, dar_n, dar_d, &par_n, &par_d);
  gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, par_n, par_d,
      NULL);

  return intersection;
}

static GstFlowReturn
gst_xvimagesink_buffer_alloc (GstBaseSink * bsink, guint64 offset, guint size,
    GstCaps * caps, GstBuffer ** buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstXvImageSink *xvimagesink;
  GstXvImageBuffer *xvimage = NULL;
  GstCaps *intersection = NULL;
  GstStructure *structure = NULL;
  gint width, height, image_format;

  xvimagesink = GST_XVIMAGESINK (bsink);

  if (G_UNLIKELY (!caps))
    goto no_caps;

  g_mutex_lock (xvimagesink->pool_lock);
  if (G_UNLIKELY (xvimagesink->pool_invalid))
    goto invalid;

  if (G_LIKELY (xvimagesink->xcontext->last_caps &&
          gst_caps_is_equal (caps, xvimagesink->xcontext->last_caps))) {
    GST_LOG_OBJECT (xvimagesink,
        "buffer alloc for same last_caps, reusing caps");
    intersection = gst_caps_ref (caps);
    image_format = xvimagesink->xcontext->last_format;
    width = xvimagesink->xcontext->last_width;
    height = xvimagesink->xcontext->last_height;

    goto reuse_last_caps;
  }

  GST_DEBUG_OBJECT (xvimagesink, "buffer alloc requested size %d with caps %"
      GST_PTR_FORMAT ", intersecting with our caps %" GST_PTR_FORMAT, size,
      caps, xvimagesink->xcontext->caps);

  /* Check the caps against our xcontext */
  intersection = gst_caps_intersect (xvimagesink->xcontext->caps, caps);

  GST_DEBUG_OBJECT (xvimagesink, "intersection in buffer alloc returned %"
      GST_PTR_FORMAT, intersection);

  if (gst_caps_is_empty (intersection)) {
    GstCaps *new_caps;

    gst_caps_unref (intersection);

    /* So we don't support this kind of buffer, let's define one we'd like */
    new_caps = gst_caps_copy (caps);

    structure = gst_caps_get_structure (new_caps, 0);
    if (!gst_structure_has_field (structure, "width") ||
        !gst_structure_has_field (structure, "height")) {
      gst_caps_unref (new_caps);
      goto invalid;
    }

    /* Try different dimensions */
    intersection =
        gst_xvimage_sink_different_size_suggestion (xvimagesink, new_caps);

    if (gst_caps_is_empty (intersection)) {
      /* Try with different YUV formats first */
      gst_structure_set_name (structure, "video/x-raw-yuv");

      /* Remove format specific fields */
      gst_structure_remove_field (structure, "format");
      gst_structure_remove_field (structure, "endianness");
      gst_structure_remove_field (structure, "depth");
      gst_structure_remove_field (structure, "bpp");
      gst_structure_remove_field (structure, "red_mask");
      gst_structure_remove_field (structure, "green_mask");
      gst_structure_remove_field (structure, "blue_mask");
      gst_structure_remove_field (structure, "alpha_mask");

      /* Reuse intersection with Xcontext */
      intersection = gst_caps_intersect (xvimagesink->xcontext->caps, new_caps);
    }

    if (gst_caps_is_empty (intersection)) {
      /* Try with different dimensions and YUV formats */
      intersection =
          gst_xvimage_sink_different_size_suggestion (xvimagesink, new_caps);
    }

    if (gst_caps_is_empty (intersection)) {
      /* Now try with RGB */
      gst_structure_set_name (structure, "video/x-raw-rgb");
      /* And interset again */
      gst_caps_unref (intersection);
      intersection = gst_caps_intersect (xvimagesink->xcontext->caps, new_caps);
    }

    if (gst_caps_is_empty (intersection)) {
      /* Try with different dimensions and RGB formats */
      intersection =
          gst_xvimage_sink_different_size_suggestion (xvimagesink, new_caps);
    }

    /* Clean this copy */
    gst_caps_unref (new_caps);

    if (gst_caps_is_empty (intersection))
      goto incompatible;
  }

  /* Ensure the returned caps are fixed */
  gst_caps_truncate (intersection);

  GST_DEBUG_OBJECT (xvimagesink, "allocating a buffer with caps %"
      GST_PTR_FORMAT, intersection);
  if (gst_caps_is_equal (intersection, caps)) {
    /* Things work better if we return a buffer with the same caps ptr
     * as was asked for when we can */
    gst_caps_replace (&intersection, caps);
  }

  /* Get image format from caps */
  image_format = gst_xvimagesink_get_format_from_caps (xvimagesink,
      intersection);

  /* Get geometry from caps */
  structure = gst_caps_get_structure (intersection, 0);
  if (!gst_structure_get_int (structure, "width", &width) ||
      !gst_structure_get_int (structure, "height", &height) ||
      image_format == -1)
    goto invalid_caps;

  /* Store our caps and format as the last_caps to avoid expensive
   * caps intersection next time */
  gst_caps_replace (&xvimagesink->xcontext->last_caps, intersection);
  xvimagesink->xcontext->last_format = image_format;
  xvimagesink->xcontext->last_width = width;
  xvimagesink->xcontext->last_height = height;

reuse_last_caps:

  /* Walking through the pool cleaning unusable images and searching for a
     suitable one */
  while (xvimagesink->image_pool) {
    xvimage = xvimagesink->image_pool->data;

    if (xvimage) {
      /* Removing from the pool */
      xvimagesink->image_pool = g_slist_delete_link (xvimagesink->image_pool,
          xvimagesink->image_pool);

      /* We check for geometry or image format changes */
      if ((xvimage->width != width) ||
          (xvimage->height != height) || (xvimage->im_format != image_format)) {
        /* This image is unusable. Destroying... */
        gst_xvimage_buffer_free (xvimage);
        xvimage = NULL;
      } else {
        /* We found a suitable image */
        GST_LOG_OBJECT (xvimagesink, "found usable image in pool");
        break;
      }
    }
  }

  if (!xvimage) {
#ifdef GST_EXT_XV_ENHANCEMENT
    /* init aligned size */
    xvimagesink->aligned_width = 0;
    xvimagesink->aligned_height = 0;
#endif /* GST_EXT_XV_ENHANCEMENT */

    /* We found no suitable image in the pool. Creating... */
    GST_DEBUG_OBJECT (xvimagesink, "no usable image in pool, creating xvimage");
    xvimage = gst_xvimagesink_xvimage_new (xvimagesink, intersection);
  }
  g_mutex_unlock (xvimagesink->pool_lock);

  if (xvimage) {
    /* Make sure the buffer is cleared of any previously used flags */
    GST_MINI_OBJECT_CAST (xvimage)->flags = 0;
    gst_buffer_set_caps (GST_BUFFER_CAST (xvimage), intersection);
  }

  *buf = GST_BUFFER_CAST (xvimage);

beach:
  if (intersection) {
    gst_caps_unref (intersection);
  }

  return ret;

  /* ERRORS */
invalid:
  {
    GST_DEBUG_OBJECT (xvimagesink, "the pool is flushing");
    ret = GST_FLOW_WRONG_STATE;
    g_mutex_unlock (xvimagesink->pool_lock);
    goto beach;
  }
incompatible:
  {
    GST_WARNING_OBJECT (xvimagesink, "we were requested a buffer with "
        "caps %" GST_PTR_FORMAT ", but our xcontext caps %" GST_PTR_FORMAT
        " are completely incompatible with those caps", caps,
        xvimagesink->xcontext->caps);
    ret = GST_FLOW_NOT_NEGOTIATED;
    g_mutex_unlock (xvimagesink->pool_lock);
    goto beach;
  }
invalid_caps:
  {
    GST_WARNING_OBJECT (xvimagesink, "invalid caps for buffer allocation %"
        GST_PTR_FORMAT, intersection);
    ret = GST_FLOW_NOT_NEGOTIATED;
    g_mutex_unlock (xvimagesink->pool_lock);
    goto beach;
  }
no_caps:
  {
    GST_WARNING_OBJECT (xvimagesink, "have no caps, doing fallback allocation");
    *buf = NULL;
    ret = GST_FLOW_OK;
    goto beach;
  }
}

/* Interfaces stuff */

static gboolean
gst_xvimagesink_interface_supported (GstImplementsInterface * iface, GType type)
{
  if (type == GST_TYPE_NAVIGATION || type == GST_TYPE_X_OVERLAY ||
      type == GST_TYPE_COLOR_BALANCE || type == GST_TYPE_PROPERTY_PROBE)
    return TRUE;
  else
    return FALSE;
}

static void
gst_xvimagesink_interface_init (GstImplementsInterfaceClass * klass)
{
  klass->supported = gst_xvimagesink_interface_supported;
}

static void
gst_xvimagesink_navigation_send_event (GstNavigation * navigation,
    GstStructure * structure)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (navigation);
  GstPad *peer;

  if ((peer = gst_pad_get_peer (GST_VIDEO_SINK_PAD (xvimagesink)))) {
    GstEvent *event;
    GstVideoRectangle src, dst, result;
    gdouble x, y, xscale = 1.0, yscale = 1.0;

    event = gst_event_new_navigation (structure);

    /* We take the flow_lock while we look at the window */
    g_mutex_lock (xvimagesink->flow_lock);

    if (!xvimagesink->xwindow) {
      g_mutex_unlock (xvimagesink->flow_lock);
      return;
    }

    if (xvimagesink->keep_aspect) {
      /* We get the frame position using the calculated geometry from _setcaps
         that respect pixel aspect ratios */
      src.w = GST_VIDEO_SINK_WIDTH (xvimagesink);
      src.h = GST_VIDEO_SINK_HEIGHT (xvimagesink);
      dst.w = xvimagesink->render_rect.w;
      dst.h = xvimagesink->render_rect.h;

      gst_video_sink_center_rect (src, dst, &result, TRUE);
      result.x += xvimagesink->render_rect.x;
      result.y += xvimagesink->render_rect.y;
    } else {
      memcpy (&result, &xvimagesink->render_rect, sizeof (GstVideoRectangle));
    }

    g_mutex_unlock (xvimagesink->flow_lock);

    /* We calculate scaling using the original video frames geometry to include
       pixel aspect ratio scaling. */
    xscale = (gdouble) xvimagesink->video_width / result.w;
    yscale = (gdouble) xvimagesink->video_height / result.h;

    /* Converting pointer coordinates to the non scaled geometry */
    if (gst_structure_get_double (structure, "pointer_x", &x)) {
      x = MIN (x, result.x + result.w);
      x = MAX (x - result.x, 0);
      gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE,
          (gdouble) x * xscale, NULL);
    }
    if (gst_structure_get_double (structure, "pointer_y", &y)) {
      y = MIN (y, result.y + result.h);
      y = MAX (y - result.y, 0);
      gst_structure_set (structure, "pointer_y", G_TYPE_DOUBLE,
          (gdouble) y * yscale, NULL);
    }

    gst_pad_send_event (peer, event);
    gst_object_unref (peer);
  }
}

static void
gst_xvimagesink_navigation_init (GstNavigationInterface * iface)
{
  iface->send_event = gst_xvimagesink_navigation_send_event;
}

#ifdef GST_EXT_XV_ENHANCEMENT
static void
gst_xvimagesink_set_pixmap_handle (GstXOverlay * overlay, guintptr id)
{
  XID pixmap_id = id;
  int i = 0;
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);
  GstXPixmap *xpixmap = NULL;
  int (*handler) (Display *, XErrorEvent *);

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  if (xvimagesink->subpicture)
   return;

  /* If the element has not initialized the X11 context try to do so */
  if (!xvimagesink->xcontext && !(xvimagesink->xcontext = gst_xvimagesink_xcontext_get (xvimagesink))) {
    /* we have thrown a GST_ELEMENT_ERROR now */
    return;
  }

  gst_xvimagesink_update_colorbalance (xvimagesink);

  GST_DEBUG_OBJECT( xvimagesink, "pixmap id : %d", pixmap_id );

  /* if the returned pixmap_id is 0, set current pixmap index to -2 to skip putImage() */
  if (pixmap_id == 0) {
    xvimagesink->current_pixmap_idx = -2;
    return;
  }

  g_mutex_lock (xvimagesink->x_lock);

  for (i = 0; i < MAX_PIXMAP_NUM; i++) {
    if (!xvimagesink->xpixmap[i]) {
      Window root_window;
      int cur_win_x = 0;
      int cur_win_y = 0;
      unsigned int cur_win_width = 0;
      unsigned int cur_win_height = 0;
      unsigned int cur_win_border_width = 0;
      unsigned int cur_win_depth = 0;

      GST_INFO_OBJECT( xvimagesink, "xpixmap[%d] is empty, create it with pixmap_id(%d)", i, pixmap_id );

      xpixmap = g_new0 (GstXPixmap, 1);
      if (xpixmap) {
        xpixmap->pixmap = pixmap_id;

        /* Get root window and size of current window */
        XGetGeometry(xvimagesink->xcontext->disp, xpixmap->pixmap, &root_window,
                     &cur_win_x, &cur_win_y, /* relative x, y */
                     &cur_win_width, &cur_win_height,
                     &cur_win_border_width, &cur_win_depth);
        if (!cur_win_width || !cur_win_height) {
          GST_INFO_OBJECT( xvimagesink, "cur_win_width(%d) or cur_win_height(%d) is null..", cur_win_width, cur_win_height );
          g_mutex_unlock (xvimagesink->x_lock);
          return;
        }
        xpixmap->width = cur_win_width;
        xpixmap->height = cur_win_height;

        if (!xvimagesink->render_rect.w)
          xvimagesink->render_rect.w = cur_win_width;
        if (!xvimagesink->render_rect.h)
          xvimagesink->render_rect.h = cur_win_height;

        /* Setting an error handler to catch failure */
        error_caught = FALSE;
        handler = XSetErrorHandler (gst_xvimagesink_handle_xerror);

        /* Create a GC */
        xpixmap->gc = XCreateGC (xvimagesink->xcontext->disp, xpixmap->pixmap, 0, NULL);

        XSync(xvimagesink->xcontext->disp, FALSE);
        if (error_caught) {
          GST_ERROR_OBJECT(xvimagesink, "XCreateGC failed [gc:0x%x, pixmap id:%d]", xpixmap->gc, xpixmap->pixmap);
        }
        if (handler) {
          error_caught = FALSE;
          XSetErrorHandler (handler);
        }

        xvimagesink->xpixmap[i] = xpixmap;
        xvimagesink->current_pixmap_idx = i;
      } else {
        GST_ERROR("failed to create xpixmap errno: %d", errno);
      }

      g_mutex_unlock (xvimagesink->x_lock);
      return;

    } else if (xvimagesink->xpixmap[i]->pixmap == pixmap_id) {
      GST_DEBUG_OBJECT( xvimagesink, "found xpixmap[%d]->pixmap : %d", i, pixmap_id );
      xvimagesink->current_pixmap_idx = i;

      g_mutex_unlock (xvimagesink->x_lock);
      return;

    } else {
      continue;
    }
  }

  GST_ERROR_OBJECT( xvimagesink, "could not find the pixmap id(%d) in xpixmap array", pixmap_id );
  xvimagesink->current_pixmap_idx = -1;

  g_mutex_unlock (xvimagesink->x_lock);
  return;
}
#endif /* GST_EXT_XV_ENHANCEMENT */

static void
gst_xvimagesink_set_window_handle (GstXOverlay * overlay, guintptr id)
{
  XID xwindow_id = id;
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);
  GstXWindow *xwindow = NULL;
#ifdef GST_EXT_XV_ENHANCEMENT
  GstState current_state = GST_STATE_NULL;
  int (*handler) (Display *, XErrorEvent *);
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));

  g_mutex_lock (xvimagesink->flow_lock);

#ifdef GST_EXT_XV_ENHANCEMENT
  gst_element_get_state(GST_ELEMENT(xvimagesink), &current_state, NULL, 0);
  GST_WARNING_OBJECT(xvimagesink, "ENTER, id : %d, current state : %d",
                                  xwindow_id, current_state);
#endif /* GST_EXT_XV_ENHANCEMENT */

  /* If we already use that window return */
  if (xvimagesink->xwindow && (xwindow_id == xvimagesink->xwindow->win)) {
    g_mutex_unlock (xvimagesink->flow_lock);
    return;
  }

  /* If the element has not initialized the X11 context try to do so */
  if (!xvimagesink->xcontext &&
      !(xvimagesink->xcontext = gst_xvimagesink_xcontext_get (xvimagesink))) {
    g_mutex_unlock (xvimagesink->flow_lock);
    /* we have thrown a GST_ELEMENT_ERROR now */
    return;
  }

  gst_xvimagesink_update_colorbalance (xvimagesink);

  /* Clear image pool as the images are unusable anyway */
  gst_xvimagesink_imagepool_clear (xvimagesink);

  /* Clear the xvimage */
  if (xvimagesink->xvimage) {
    gst_xvimage_buffer_free (xvimagesink->xvimage);
    xvimagesink->xvimage = NULL;
  }

  /* If a window is there already we destroy it */
  if (xvimagesink->xwindow) {
    gst_xvimagesink_xwindow_destroy (xvimagesink, xvimagesink->xwindow);
    xvimagesink->xwindow = NULL;
  }

  /* If the xid is 0 we go back to an internal window */
  if (xwindow_id == 0) {
    /* If no width/height caps nego did not happen window will be created
       during caps nego then */
#ifdef GST_EXT_XV_ENHANCEMENT
  GST_INFO_OBJECT( xvimagesink, "xid is 0. create window[%dx%d]",
    GST_VIDEO_SINK_WIDTH (xvimagesink), GST_VIDEO_SINK_HEIGHT (xvimagesink) );
#endif /* GST_EXT_XV_ENHANCEMENT */
    if (GST_VIDEO_SINK_WIDTH (xvimagesink)
        && GST_VIDEO_SINK_HEIGHT (xvimagesink)) {
      xwindow =
          gst_xvimagesink_xwindow_new (xvimagesink,
          GST_VIDEO_SINK_WIDTH (xvimagesink),
          GST_VIDEO_SINK_HEIGHT (xvimagesink));
    }
  } else {
    XWindowAttributes attr;

    xwindow = g_new0 (GstXWindow, 1);
    xwindow->win = xwindow_id;

    /* Set the event we want to receive and create a GC */
    g_mutex_lock (xvimagesink->x_lock);

    if(!xvimagesink->is_pixmap)
      XGetWindowAttributes (xvimagesink->xcontext->disp, xwindow->win, &attr);

    xwindow->width = attr.width;
    xwindow->height = attr.height;
    xwindow->internal = FALSE;
    if (!xvimagesink->have_render_rect) {
      xvimagesink->render_rect.x = xvimagesink->render_rect.y = 0;
      xvimagesink->render_rect.w = attr.width;
      xvimagesink->render_rect.h = attr.height;
    }
    if (xvimagesink->handle_events) {
#ifdef GST_EXT_XV_ENHANCEMENT
      XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xcontext->root, StructureNotifyMask | SubstructureNotifyMask);
      XSelectInput (xvimagesink->xcontext->disp, xwindow->win, ExposureMask |
          StructureNotifyMask | PointerMotionMask | KeyPressMask |
          KeyReleaseMask | PropertyChangeMask);
#else
      XSelectInput (xvimagesink->xcontext->disp, xwindow->win, ExposureMask |
          StructureNotifyMask | PointerMotionMask | KeyPressMask |
          KeyReleaseMask);
#endif

    }
#ifdef GST_EXT_XV_ENHANCEMENT
    /* Setting an error handler to catch failure */
    error_caught = FALSE;
    handler = XSetErrorHandler (gst_xvimagesink_handle_xerror);
#endif
    xwindow->gc = XCreateGC (xvimagesink->xcontext->disp,
        xwindow->win, 0, NULL);
#ifdef GST_EXT_XV_ENHANCEMENT
    XSync(xvimagesink->xcontext->disp, FALSE);
    if (error_caught) {
      GST_ERROR_OBJECT(xvimagesink, "XCreateGC failed [gc:0x%x, xid:%d]", xwindow->gc, xwindow->win);
    }
    if (handler) {
      error_caught = FALSE;
      XSetErrorHandler (handler);
    }
#endif
    g_mutex_unlock (xvimagesink->x_lock);
  }

  if (xwindow)
    xvimagesink->xwindow = xwindow;

#ifdef GST_EXT_XV_ENHANCEMENT
  xvimagesink->xid_updated = TRUE;
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_mutex_unlock (xvimagesink->flow_lock);

#ifdef GST_EXT_XV_ENHANCEMENT
  if (current_state == GST_STATE_PAUSED) {
    GstBuffer *last_buffer = NULL;
    g_object_get(G_OBJECT(xvimagesink), "last-buffer", &last_buffer, NULL);
    GST_WARNING_OBJECT(xvimagesink, "PASUED state: window handle is updated. last buffer %p", last_buffer);
    if (last_buffer) {
      gst_xvimagesink_show_frame((GstVideoSink *)xvimagesink, last_buffer);
      gst_buffer_unref(last_buffer);
      last_buffer = NULL;
    }
  }
#endif /* GST_EXT_XV_ENHANCEMENT */
}

static void
gst_xvimagesink_expose (GstXOverlay * overlay)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);

  gst_xvimagesink_xwindow_update_geometry (xvimagesink);
#ifdef GST_EXT_XV_ENHANCEMENT
  GST_INFO_OBJECT(xvimagesink, "Overlay window exposed. update it");
#endif /* GST_EXT_XV_ENHANCEMENT */
  gst_xvimagesink_xvimage_put (xvimagesink, NULL);
}

static void
gst_xvimagesink_set_event_handling (GstXOverlay * overlay,
    gboolean handle_events)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);

  xvimagesink->handle_events = handle_events;

  g_mutex_lock (xvimagesink->flow_lock);

  if (G_UNLIKELY (!xvimagesink->xwindow)) {
    g_mutex_unlock (xvimagesink->flow_lock);
    return;
  }

  g_mutex_lock (xvimagesink->x_lock);

  if (handle_events) {
    if (xvimagesink->xwindow->internal) {
#ifdef GST_EXT_XV_ENHANCEMENT
      XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xcontext->root, StructureNotifyMask | SubstructureNotifyMask);
#endif
      XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xwindow->win,
#ifdef GST_EXT_XV_ENHANCEMENT
          ExposureMask | StructureNotifyMask | PointerMotionMask | VisibilityChangeMask | PropertyChangeMask |
#else /* GST_EXT_XV_ENHANCEMENT */
          ExposureMask | StructureNotifyMask | PointerMotionMask |
#endif /* GST_EXT_XV_ENHANCEMENT */
          KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);
    } else {
#ifdef GST_EXT_XV_ENHANCEMENT
      XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xcontext->root, StructureNotifyMask | SubstructureNotifyMask);
#endif
      XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xwindow->win,
#ifdef GST_EXT_XV_ENHANCEMENT
          ExposureMask | StructureNotifyMask | PointerMotionMask | VisibilityChangeMask | PropertyChangeMask |
#else /* GST_EXT_XV_ENHANCEMENT */
          ExposureMask | StructureNotifyMask | PointerMotionMask |
#endif /* GST_EXT_XV_ENHANCEMENT */
          KeyPressMask | KeyReleaseMask);
    }
  } else {
    XSelectInput (xvimagesink->xcontext->disp, xvimagesink->xwindow->win, 0);
  }

  g_mutex_unlock (xvimagesink->x_lock);

  g_mutex_unlock (xvimagesink->flow_lock);
}

static void
gst_xvimagesink_set_render_rectangle (GstXOverlay * overlay, gint x, gint y,
    gint width, gint height)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (overlay);

  /* FIXME: how about some locking? */
  if (width >= 0 && height >= 0) {
    xvimagesink->render_rect.x = x;
    xvimagesink->render_rect.y = y;
    xvimagesink->render_rect.w = width;
    xvimagesink->render_rect.h = height;
    xvimagesink->have_render_rect = TRUE;
  } else {
    xvimagesink->render_rect.x = 0;
    xvimagesink->render_rect.y = 0;
    xvimagesink->render_rect.w = xvimagesink->xwindow->width;
    xvimagesink->render_rect.h = xvimagesink->xwindow->height;
    xvimagesink->have_render_rect = FALSE;
  }
}

static void
gst_xvimagesink_xoverlay_init (GstXOverlayClass * iface)
{
  iface->set_window_handle = gst_xvimagesink_set_window_handle;
  iface->expose = gst_xvimagesink_expose;
  iface->handle_events = gst_xvimagesink_set_event_handling;
  iface->set_render_rectangle = gst_xvimagesink_set_render_rectangle;
}

static const GList *
gst_xvimagesink_colorbalance_list_channels (GstColorBalance * balance)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (balance);

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), NULL);

  if (xvimagesink->xcontext)
    return xvimagesink->xcontext->channels_list;
  else
    return NULL;
}

static void
gst_xvimagesink_colorbalance_set_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel, gint value)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (balance);

  g_return_if_fail (GST_IS_XVIMAGESINK (xvimagesink));
  g_return_if_fail (channel->label != NULL);

  xvimagesink->cb_changed = TRUE;

  /* Normalize val to [-1000, 1000] */
  value = floor (0.5 + -1000 + 2000 * (value - channel->min_value) /
      (double) (channel->max_value - channel->min_value));

  if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
    xvimagesink->hue = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
    xvimagesink->saturation = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
    xvimagesink->contrast = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
    xvimagesink->brightness = value;
  } else {
    g_warning ("got an unknown channel %s", channel->label);
    return;
  }

  gst_xvimagesink_update_colorbalance (xvimagesink);
}

static gint
gst_xvimagesink_colorbalance_get_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (balance);
  gint value = 0;

  g_return_val_if_fail (GST_IS_XVIMAGESINK (xvimagesink), 0);
  g_return_val_if_fail (channel->label != NULL, 0);

  if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
    value = xvimagesink->hue;
  } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
    value = xvimagesink->saturation;
  } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
    value = xvimagesink->contrast;
  } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
    value = xvimagesink->brightness;
  } else {
    g_warning ("got an unknown channel %s", channel->label);
  }

  /* Normalize val to [channel->min_value, channel->max_value] */
  value = channel->min_value + (channel->max_value - channel->min_value) *
      (value + 1000) / 2000;

  return value;
}

static void
gst_xvimagesink_colorbalance_init (GstColorBalanceClass * iface)
{
  GST_COLOR_BALANCE_TYPE (iface) = GST_COLOR_BALANCE_HARDWARE;
  iface->list_channels = gst_xvimagesink_colorbalance_list_channels;
  iface->set_value = gst_xvimagesink_colorbalance_set_value;
  iface->get_value = gst_xvimagesink_colorbalance_get_value;
}

static const GList *
gst_xvimagesink_probe_get_properties (GstPropertyProbe * probe)
{
  GObjectClass *klass = G_OBJECT_GET_CLASS (probe);
  static GList *list = NULL;

  if (!list) {
    list = g_list_append (NULL, g_object_class_find_property (klass, "device"));
    list =
        g_list_append (list, g_object_class_find_property (klass,
            "autopaint-colorkey"));
    list =
        g_list_append (list, g_object_class_find_property (klass,
            "double-buffer"));
    list =
        g_list_append (list, g_object_class_find_property (klass, "colorkey"));
  }

  return list;
}

static void
gst_xvimagesink_probe_probe_property (GstPropertyProbe * probe,
    guint prop_id, const GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (probe);

  switch (prop_id) {
    case PROP_DEVICE:
    case PROP_AUTOPAINT_COLORKEY:
    case PROP_DOUBLE_BUFFER:
    case PROP_COLORKEY:
      GST_DEBUG_OBJECT (xvimagesink,
          "probing device list and get capabilities");
      if (!xvimagesink->xcontext) {
        GST_DEBUG_OBJECT (xvimagesink, "generating xcontext");
        xvimagesink->xcontext = gst_xvimagesink_xcontext_get (xvimagesink);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (probe, prop_id, pspec);
      break;
  }
}

static gboolean
gst_xvimagesink_probe_needs_probe (GstPropertyProbe * probe,
    guint prop_id, const GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (probe);
  gboolean ret = FALSE;

  switch (prop_id) {
    case PROP_DEVICE:
    case PROP_AUTOPAINT_COLORKEY:
    case PROP_DOUBLE_BUFFER:
    case PROP_COLORKEY:
      if (xvimagesink->xcontext != NULL) {
        ret = FALSE;
      } else {
        ret = TRUE;
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (probe, prop_id, pspec);
      break;
  }

  return ret;
}

static GValueArray *
gst_xvimagesink_probe_get_values (GstPropertyProbe * probe,
    guint prop_id, const GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink = GST_XVIMAGESINK (probe);
  GValueArray *array = NULL;

  if (G_UNLIKELY (!xvimagesink->xcontext)) {
    GST_WARNING_OBJECT (xvimagesink, "we don't have any xcontext, can't "
        "get values");
    goto beach;
  }

  switch (prop_id) {
    case PROP_DEVICE:
    {
      guint i;
      GValue value = { 0 };

      array = g_value_array_new (xvimagesink->xcontext->nb_adaptors);
      g_value_init (&value, G_TYPE_STRING);

      for (i = 0; i < xvimagesink->xcontext->nb_adaptors; i++) {
        gchar *adaptor_id_s = g_strdup_printf ("%u", i);

        g_value_set_string (&value, adaptor_id_s);
        g_value_array_append (array, &value);
        g_free (adaptor_id_s);
      }
      g_value_unset (&value);
      break;
    }
    case PROP_AUTOPAINT_COLORKEY:
      if (xvimagesink->have_autopaint_colorkey) {
        GValue value = { 0 };

        array = g_value_array_new (2);
        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, FALSE);
        g_value_array_append (array, &value);
        g_value_set_boolean (&value, TRUE);
        g_value_array_append (array, &value);
        g_value_unset (&value);
      }
      break;
    case PROP_DOUBLE_BUFFER:
      if (xvimagesink->have_double_buffer) {
        GValue value = { 0 };

        array = g_value_array_new (2);
        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, FALSE);
        g_value_array_append (array, &value);
        g_value_set_boolean (&value, TRUE);
        g_value_array_append (array, &value);
        g_value_unset (&value);
      }
      break;
    case PROP_COLORKEY:
      if (xvimagesink->have_colorkey) {
        GValue value = { 0 };

        array = g_value_array_new (1);
        g_value_init (&value, GST_TYPE_INT_RANGE);
        gst_value_set_int_range (&value, 0, 0xffffff);
        g_value_array_append (array, &value);
        g_value_unset (&value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (probe, prop_id, pspec);
      break;
  }

beach:
  return array;
}

static void
gst_xvimagesink_property_probe_interface_init (GstPropertyProbeInterface *
    iface)
{
  iface->get_properties = gst_xvimagesink_probe_get_properties;
  iface->probe_property = gst_xvimagesink_probe_probe_property;
  iface->needs_probe = gst_xvimagesink_probe_needs_probe;
  iface->get_values = gst_xvimagesink_probe_get_values;
}

/* =========================================== */
/*                                             */
/*              Init & Class init              */
/*                                             */
/* =========================================== */

static void
gst_xvimagesink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink;

  g_return_if_fail (GST_IS_XVIMAGESINK (object));

  xvimagesink = GST_XVIMAGESINK (object);

  switch (prop_id) {
    case PROP_HUE:
      xvimagesink->hue = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case PROP_CONTRAST:
      xvimagesink->contrast = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case PROP_BRIGHTNESS:
      xvimagesink->brightness = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case PROP_SATURATION:
      xvimagesink->saturation = g_value_get_int (value);
      xvimagesink->cb_changed = TRUE;
      gst_xvimagesink_update_colorbalance (xvimagesink);
      break;
    case PROP_DISPLAY:
      xvimagesink->display_name = g_strdup (g_value_get_string (value));
      break;
    case PROP_SYNCHRONOUS:
      xvimagesink->synchronous = g_value_get_boolean (value);
      if (xvimagesink->xcontext) {
        XSynchronize (xvimagesink->xcontext->disp, xvimagesink->synchronous);
        GST_DEBUG_OBJECT (xvimagesink, "XSynchronize called with %s",
            xvimagesink->synchronous ? "TRUE" : "FALSE");
      }
      break;
    case PROP_PIXEL_ASPECT_RATIO:
      g_free (xvimagesink->par);
      xvimagesink->par = g_new0 (GValue, 1);
      g_value_init (xvimagesink->par, GST_TYPE_FRACTION);
      if (!g_value_transform (value, xvimagesink->par)) {
        g_warning ("Could not transform string to aspect ratio");
        gst_value_set_fraction (xvimagesink->par, 1, 1);
      }
      GST_DEBUG_OBJECT (xvimagesink, "set PAR to %d/%d",
          gst_value_get_fraction_numerator (xvimagesink->par),
          gst_value_get_fraction_denominator (xvimagesink->par));
      break;
    case PROP_FORCE_ASPECT_RATIO:
      xvimagesink->keep_aspect = g_value_get_boolean (value);
      break;
    case PROP_HANDLE_EVENTS:
      gst_xvimagesink_set_event_handling (GST_X_OVERLAY (xvimagesink),
          g_value_get_boolean (value));
      gst_xvimagesink_manage_event_thread (xvimagesink);
      break;
    case PROP_DEVICE:
      xvimagesink->adaptor_no = atoi (g_value_get_string (value));
      break;
    case PROP_HANDLE_EXPOSE:
      xvimagesink->handle_expose = g_value_get_boolean (value);
      gst_xvimagesink_manage_event_thread (xvimagesink);
      break;
    case PROP_DOUBLE_BUFFER:
      xvimagesink->double_buffer = g_value_get_boolean (value);
      break;
    case PROP_AUTOPAINT_COLORKEY:
      xvimagesink->autopaint_colorkey = g_value_get_boolean (value);
      break;
    case PROP_COLORKEY:
      xvimagesink->colorkey = g_value_get_int (value);
      break;
    case PROP_DRAW_BORDERS:
      xvimagesink->draw_borders = g_value_get_boolean (value);
      break;
#ifdef GST_EXT_XV_ENHANCEMENT
    case PROP_DISPLAY_MODE:
    {
      int set_mode = g_value_get_enum (value);
      g_mutex_lock(xvimagesink->flow_lock);
      xvimagesink->display_mode = set_mode;
      if(!xvimagesink->get_pixmap_cb && !xvimagesink->subpicture) {
        if(xvimagesink->display_mode==DISPLAY_MODE_PRI_VIDEO_OFF_AND_SEC_VIDEO_FULL_SCREEN) {
          if(!xvimagesink->is_multi_window || (GST_STATE(xvimagesink) == GST_STATE_PLAYING)) {
            set_display_mode(xvimagesink->xcontext, xvimagesink->display_mode);
          } else {
            GST_WARNING_OBJECT(xvimagesink, "display-mode will be set later. just save value now(%d)", xvimagesink->display_mode);
          }
        } else if(xvimagesink->display_mode==DISPLAY_MODE_PRI_VIDEO_ON_AND_SEC_VIDEO_CLONE){
          if(xvimagesink->is_multi_window && (GST_STATE(xvimagesink) != GST_STATE_PLAYING)) {
            set_display_mode(xvimagesink->xcontext, xvimagesink->display_mode);
          } else {
            GST_WARNING_OBJECT(xvimagesink, "display-mode will be set later. just save value now(%d)", xvimagesink->display_mode);
          }
        } else {
          GST_WARNING_OBJECT(xvimagesink, "unsupported format(%d)", xvimagesink->display_mode);
        }
      }
      g_mutex_unlock(xvimagesink->flow_lock);
    }
      break;
    case PROP_CSC_RANGE:
    {
      int set_range = g_value_get_enum (value);

      g_mutex_lock(xvimagesink->flow_lock);
      g_mutex_lock(xvimagesink->x_lock);

      if (xvimagesink->csc_range != set_range) {
        if (xvimagesink->xcontext && !xvimagesink->subpicture) {
          /* set color space range */
          if (set_csc_range(xvimagesink->xcontext, set_range)) {
            xvimagesink->csc_range = set_range;
          } else {
            GST_WARNING_OBJECT(xvimagesink, "csc range[%d] set failed.", set_range);
          }
        } else {
          /* "xcontext" is not created yet. It will be applied when xcontext is created. */
          GST_INFO_OBJECT(xvimagesink, "xcontext is NULL. color space range will be set later.");
          xvimagesink->csc_range = set_range;
        }
      } else if (xvimagesink->subpicture) {
        GST_WARNING("skip to set csc range, because it is subpicture format.");
      } else {
        GST_INFO_OBJECT(xvimagesink, "skip to set csc range %d, because current is same", set_range);
      }

      g_mutex_unlock(xvimagesink->x_lock);
      g_mutex_unlock(xvimagesink->flow_lock);
    }
      break;
    case PROP_DISPLAY_GEOMETRY_METHOD:
      xvimagesink->display_geometry_method = g_value_get_enum (value);
      GST_LOG("Overlay geometry changed. update it");
      if (GST_STATE(xvimagesink) == GST_STATE_PAUSED || xvimagesink->eos_received) {
        gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->xvimage);
      }
      break;
    case PROP_FLIP:
      xvimagesink->flip = g_value_get_enum(value);
      break;
    case PROP_ROTATE_ANGLE:
      xvimagesink->rotate_angle = g_value_get_enum (value);
      if (GST_STATE(xvimagesink) == GST_STATE_PAUSED || xvimagesink->eos_received) {
        gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->xvimage);
      }
      break;
    case PROP_VISIBLE:
      g_mutex_lock( xvimagesink->flow_lock );
      g_mutex_lock( xvimagesink->x_lock );

      GST_WARNING_OBJECT(xvimagesink, "set visible %d", g_value_get_boolean(value));

      if (xvimagesink->visible && (g_value_get_boolean(value) == FALSE)) {
        if (xvimagesink->xcontext) {
#if 0
          Atom atom_stream = XInternAtom( xvimagesink->xcontext->disp,
                                          "_USER_WM_PORT_ATTRIBUTE_STREAM_OFF", False );
          if (atom_stream != None) {
            GST_WARNING_OBJECT(xvimagesink, "Visible FALSE -> CALL STREAM_OFF");
            if (XvSetPortAttribute(xvimagesink->xcontext->disp,
                                   xvimagesink->xcontext->xv_port_id,
                                   atom_stream, 0 ) != Success) {
              GST_WARNING_OBJECT( xvimagesink, "Set visible FALSE failed" );
            }
          }
#endif
          xvimagesink->visible = g_value_get_boolean (value);
          if ( xvimagesink->get_pixmap_cb ) {
            if (xvimagesink->xpixmap[0] && xvimagesink->xpixmap[0]->pixmap) {
              XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xpixmap[0]->pixmap);
              }
          } else {
            if(xvimagesink->xwindow->win)
              XvStopVideo(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xwindow->win);
          }
          XSync( xvimagesink->xcontext->disp, FALSE );
        } else {
          GST_WARNING_OBJECT( xvimagesink, "xcontext is null");
          xvimagesink->visible = g_value_get_boolean (value);
        }
      } else if (!xvimagesink->visible && (g_value_get_boolean(value) == TRUE)) {
        xvimagesink->visible = g_value_get_boolean (value);
        g_mutex_unlock( xvimagesink->x_lock );
        g_mutex_unlock( xvimagesink->flow_lock );
        gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->xvimage);
        g_mutex_lock( xvimagesink->flow_lock );
        g_mutex_lock( xvimagesink->x_lock );
      }

      GST_INFO("set visible(%d) done", xvimagesink->visible);

      g_mutex_unlock( xvimagesink->x_lock );
      g_mutex_unlock( xvimagesink->flow_lock );
      break;
    case PROP_ZOOM:
      xvimagesink->zoom = g_value_get_float (value);
      break;
    case PROP_ZOOM_POS_X:
      xvimagesink->zoom_pos_x = g_value_get_int (value);
      break;
    case PROP_ZOOM_POS_Y:
      xvimagesink->zoom_pos_y = g_value_get_int (value);
      if (GST_STATE(xvimagesink) == GST_STATE_PAUSED || xvimagesink->eos_received) {
        gst_xvimagesink_xvimage_put (xvimagesink, xvimagesink->xvimage);
      }
      break;
    case PROP_ORIENTATION:
      xvimagesink->orientation = g_value_get_enum (value);
      GST_INFO("Orientation(%d) is changed", xvimagesink->orientation);
      break;
    case PROP_DST_ROI_MODE:
      xvimagesink->dst_roi_mode = g_value_get_enum (value);
      GST_INFO("Overlay geometry(%d) for ROI is changed", xvimagesink->dst_roi_mode);
      break;
    case PROP_DST_ROI_X:
      xvimagesink->dst_roi.x = g_value_get_int (value);
      break;
    case PROP_DST_ROI_Y:
      xvimagesink->dst_roi.y = g_value_get_int (value);
      break;
    case PROP_DST_ROI_W:
      xvimagesink->dst_roi.w = g_value_get_int (value);
      break;
    case PROP_DST_ROI_H:
      xvimagesink->dst_roi.h = g_value_get_int (value);
      break;
    case PROP_SRC_CROP_X:
      xvimagesink->src_crop.x = g_value_get_int (value);
      break;
    case PROP_SRC_CROP_Y:
      xvimagesink->src_crop.y = g_value_get_int (value);
      break;
    case PROP_SRC_CROP_W:
      xvimagesink->src_crop.w = g_value_get_int (value);
      break;
    case PROP_SRC_CROP_H:
      xvimagesink->src_crop.h = g_value_get_int (value);
      break;
    case PROP_STOP_VIDEO:
      xvimagesink->stop_video = g_value_get_int (value);
      g_mutex_lock( xvimagesink->flow_lock );

      if( xvimagesink->stop_video )
      {
        if ( xvimagesink->get_pixmap_cb ) {
          if (xvimagesink->xpixmap[0] && xvimagesink->xpixmap[0]->pixmap) {
            g_mutex_lock (xvimagesink->x_lock);
            GST_WARNING_OBJECT( xvimagesink, "calling XvStopVideo()" );
            XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xpixmap[0]->pixmap);
            g_mutex_unlock (xvimagesink->x_lock);
          }
        } else {
          GST_INFO_OBJECT( xvimagesink, "Xwindow CLEAR when set video-stop property" );
          gst_xvimagesink_xwindow_clear (xvimagesink, xvimagesink->xwindow);
        }
      }

      g_mutex_unlock( xvimagesink->flow_lock );
      break;
    case PROP_PIXMAP_CB:
    {
      void *cb_func;
      cb_func = g_value_get_pointer(value);
      if (cb_func) {
        if (xvimagesink->get_pixmap_cb) {
          int i = 0;
          if (xvimagesink->xpixmap[0] && xvimagesink->xpixmap[0]->pixmap) {
            g_mutex_lock (xvimagesink->x_lock);
            GST_WARNING_OBJECT( xvimagesink, "calling XvStopVideo()" );
            XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xpixmap[0]->pixmap);
            g_mutex_unlock (xvimagesink->x_lock);
          }
          for (i = 0; i < MAX_PIXMAP_NUM; i++) {
            if (xvimagesink->xpixmap[i]) {
              gst_xvimagesink_xpixmap_destroy (xvimagesink, xvimagesink->xpixmap[i]);
              xvimagesink->xpixmap[i] = NULL;
            }
          }
        }
        xvimagesink->get_pixmap_cb = cb_func;
        GST_INFO_OBJECT (xvimagesink, "Set callback(0x%x) for getting pixmap id", xvimagesink->get_pixmap_cb);
      }
      break;
    }
    case PROP_PIXMAP_CB_USER_DATA:
    {
      void *user_data;
      user_data = g_value_get_pointer(value);
      if (user_data) {
        xvimagesink->get_pixmap_cb_user_data = user_data;
        GST_INFO_OBJECT (xvimagesink, "Set user data(0x%x) for getting pixmap id", xvimagesink->get_pixmap_cb_user_data);
      }
      break;
    }
    case PROP_SUBPICTURE:
      xvimagesink->subpicture = g_value_get_boolean (value);
    break;
    case PROP_EXTERNAL_WIDTH:
    {
      xvimagesink->external_width = g_value_get_int (value);
      GST_LOG("[set property] xvimagesink->external_width : %d", xvimagesink->external_width);
      break;
    }
    case PROP_EXTERNAL_HEIGHT:
    {
      xvimagesink->external_height = g_value_get_int (value);
      GST_LOG("[set property] xvimagesink->external_height : %d", xvimagesink->external_height);
      break;
    }
    case PROP_ENABLE_FLUSH_BUFFER:
      xvimagesink->enable_flush_buffer = g_value_get_boolean(value);
      break;
    case PROP_PIXMAP:
      xvimagesink->is_pixmap = g_value_get_boolean(value);
      break;
    case PROP_HIDED_WINDOW:
    {
      xvimagesink->is_hided_subpicture = g_value_get_boolean(value);
      GST_WARNING_OBJECT(xvimagesink, "update hided_window %d", xvimagesink->is_hided_subpicture);
      break;
    }
    case PROP_QUICKPANEL_ON:
    {
      xvimagesink->is_quick_panel_on_subpicture = g_value_get_boolean(value);
      GST_WARNING_OBJECT(xvimagesink, "update quick panel status %d", xvimagesink->is_quick_panel_on_subpicture);
      break;
    }
    case PROP_MULTIWINDOW_ACTIVE:
    {
      xvimagesink->is_multi_window_subpicture = g_value_get_boolean(value);
      GST_WARNING_OBJECT(xvimagesink, "update multi-window status %d", xvimagesink->is_multi_window_subpicture);
      break;
    }
    case PROP_KEEP_EXTERNAL_FULLSCREEN_POST:
    {
      xvimagesink->keep_external_fullscreen_post = g_value_get_boolean(value);
      GST_WARNING_OBJECT(xvimagesink, "set property %d for setting _USER_WM_PORT_ATTRIBUTE_KEEP_EXT", xvimagesink->keep_external_fullscreen_post);
      if(xvimagesink->keep_external_fullscreen_post) {
        Atom atom_keep_ext;
        atom_keep_ext = XInternAtom (xvimagesink->xcontext->disp, "_USER_WM_PORT_ATTRIBUTE_KEEP_EXT", False);
        if(XvSetPortAttribute (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, atom_keep_ext , 1) != Success)
        {
          GST_WARNING("set atom_keep_ext fail");
        }
      }
      break;
    }
    case PROP_KEEP_EXTERNAL_FULLSCREEN_PREV:
    {
      xvimagesink->keep_external_fullscreen_prev = g_value_get_boolean(value);
      GST_WARNING_OBJECT(xvimagesink, "set property %d for keeping external display to full screen", xvimagesink->keep_external_fullscreen_prev);
      break;
    }

#endif /* GST_EXT_XV_ENHANCEMENT */
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_xvimagesink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstXvImageSink *xvimagesink;

  g_return_if_fail (GST_IS_XVIMAGESINK (object));

  xvimagesink = GST_XVIMAGESINK (object);

  switch (prop_id) {
    case PROP_HUE:
      g_value_set_int (value, xvimagesink->hue);
      break;
    case PROP_CONTRAST:
      g_value_set_int (value, xvimagesink->contrast);
      break;
    case PROP_BRIGHTNESS:
      g_value_set_int (value, xvimagesink->brightness);
      break;
    case PROP_SATURATION:
      g_value_set_int (value, xvimagesink->saturation);
      break;
    case PROP_DISPLAY:
      g_value_set_string (value, xvimagesink->display_name);
      break;
    case PROP_SYNCHRONOUS:
      g_value_set_boolean (value, xvimagesink->synchronous);
      break;
    case PROP_PIXEL_ASPECT_RATIO:
      if (xvimagesink->par)
        g_value_transform (xvimagesink->par, value);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      g_value_set_boolean (value, xvimagesink->keep_aspect);
      break;
    case PROP_HANDLE_EVENTS:
      g_value_set_boolean (value, xvimagesink->handle_events);
      break;
    case PROP_DEVICE:
    {
      char *adaptor_no_s = g_strdup_printf ("%u", xvimagesink->adaptor_no);

      g_value_set_string (value, adaptor_no_s);
      g_free (adaptor_no_s);
      break;
    }
    case PROP_DEVICE_NAME:
      if (xvimagesink->xcontext && xvimagesink->xcontext->adaptors) {
        g_value_set_string (value,
            xvimagesink->xcontext->adaptors[xvimagesink->adaptor_no]);
      } else {
        g_value_set_string (value, NULL);
      }
      break;
    case PROP_HANDLE_EXPOSE:
      g_value_set_boolean (value, xvimagesink->handle_expose);
      break;
    case PROP_DOUBLE_BUFFER:
      g_value_set_boolean (value, xvimagesink->double_buffer);
      break;
    case PROP_AUTOPAINT_COLORKEY:
      g_value_set_boolean (value, xvimagesink->autopaint_colorkey);
      break;
    case PROP_COLORKEY:
      g_value_set_int (value, xvimagesink->colorkey);
      break;
    case PROP_DRAW_BORDERS:
      g_value_set_boolean (value, xvimagesink->draw_borders);
      break;
    case PROP_WINDOW_WIDTH:
      if (xvimagesink->xwindow)
        g_value_set_uint64 (value, xvimagesink->xwindow->width);
      else
        g_value_set_uint64 (value, 0);
      break;
    case PROP_WINDOW_HEIGHT:
      if (xvimagesink->xwindow)
        g_value_set_uint64 (value, xvimagesink->xwindow->height);
      else
        g_value_set_uint64 (value, 0);
      break;
#ifdef GST_EXT_XV_ENHANCEMENT
    case PROP_DISPLAY_MODE:
      g_value_set_enum (value, xvimagesink->display_mode);
      break;
    case PROP_CSC_RANGE:
      g_value_set_enum (value, xvimagesink->csc_range);
      break;
    case PROP_DISPLAY_GEOMETRY_METHOD:
      g_value_set_enum (value, xvimagesink->display_geometry_method);
      break;
    case PROP_FLIP:
      g_value_set_enum(value, xvimagesink->flip);
      break;
    case PROP_ROTATE_ANGLE:
      g_value_set_enum (value, xvimagesink->rotate_angle);
      break;
    case PROP_VISIBLE:
      g_value_set_boolean (value, xvimagesink->visible);
      break;
    case PROP_ZOOM:
      g_value_set_float (value, xvimagesink->zoom);
      break;
    case PROP_ZOOM_POS_X:
      g_value_set_int (value, xvimagesink->zoom_pos_x);
      break;
    case PROP_ZOOM_POS_Y:
      g_value_set_int (value, xvimagesink->zoom_pos_y);
      break;
    case PROP_ORIENTATION:
      g_value_set_enum (value, xvimagesink->orientation);
      break;
    case PROP_DST_ROI_MODE:
      g_value_set_enum (value, xvimagesink->dst_roi_mode);
      break;
    case PROP_DST_ROI_X:
      g_value_set_int (value, xvimagesink->dst_roi.x);
      break;
    case PROP_DST_ROI_Y:
      g_value_set_int (value, xvimagesink->dst_roi.y);
      break;
    case PROP_DST_ROI_W:
      g_value_set_int (value, xvimagesink->dst_roi.w);
      break;
    case PROP_DST_ROI_H:
      g_value_set_int (value, xvimagesink->dst_roi.h);
      break;
    case PROP_SRC_CROP_X:
      g_value_set_int (value, xvimagesink->src_crop.x);
      break;
    case PROP_SRC_CROP_Y:
      g_value_set_int (value, xvimagesink->src_crop.y);
      break;
    case PROP_SRC_CROP_W:
      g_value_set_int (value, xvimagesink->src_crop.w);
      break;
    case PROP_SRC_CROP_H:
      g_value_set_int (value, xvimagesink->src_crop.h);
      break;
    case PROP_STOP_VIDEO:
      g_value_set_int (value, xvimagesink->stop_video);
      break;
    case PROP_PIXMAP_CB:
      g_value_set_pointer (value, xvimagesink->get_pixmap_cb);
      break;
    case PROP_PIXMAP_CB_USER_DATA:
      g_value_set_pointer (value, xvimagesink->get_pixmap_cb_user_data);
      break;
    case PROP_SUBPICTURE:
      g_value_set_boolean (value, xvimagesink->subpicture);
      break;
    case PROP_EXTERNAL_WIDTH:
      g_value_set_int (value, xvimagesink->external_width);
      break;
    case PROP_EXTERNAL_HEIGHT:
      g_value_set_int (value, xvimagesink->external_height);
      break;
    case PROP_ENABLE_FLUSH_BUFFER:
      g_value_set_boolean(value, xvimagesink->enable_flush_buffer);
      break;
    case PROP_PIXMAP:
      g_value_set_boolean(value, xvimagesink->is_pixmap);
      break;
    case PROP_HIDED_WINDOW:
      g_value_set_boolean(value, xvimagesink->is_hided_subpicture);
      break;
    case PROP_QUICKPANEL_ON:
      g_value_set_boolean(value, xvimagesink->is_quick_panel_on_subpicture);
      break;
    case PROP_MULTIWINDOW_ACTIVE:
      g_value_set_boolean(value, xvimagesink->is_multi_window_subpicture);
      break;
    case PROP_KEEP_EXTERNAL_FULLSCREEN_POST:
      g_value_set_boolean(value, xvimagesink->keep_external_fullscreen_post);
     break;
    case PROP_KEEP_EXTERNAL_FULLSCREEN_PREV:
      g_value_set_boolean(value, xvimagesink->keep_external_fullscreen_prev);
     break;
#endif /* GST_EXT_XV_ENHANCEMENT */
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_xvimagesink_reset (GstXvImageSink * xvimagesink)
{
  GThread *thread;

  GST_OBJECT_LOCK (xvimagesink);
  xvimagesink->running = FALSE;
  /* grab thread and mark it as NULL */
  thread = xvimagesink->event_thread;
  xvimagesink->event_thread = NULL;
  GST_OBJECT_UNLOCK (xvimagesink);

  /* invalidate the pool, current allocations continue, new buffer_alloc fails
   * with wrong_state */
  g_mutex_lock (xvimagesink->pool_lock);
  xvimagesink->pool_invalid = TRUE;
  g_mutex_unlock (xvimagesink->pool_lock);

  /* Wait for our event thread to finish before we clean up our stuff. */
  if (thread)
    g_thread_join (thread);

  if (xvimagesink->cur_image) {
    gst_buffer_unref (GST_BUFFER_CAST (xvimagesink->cur_image));
    xvimagesink->cur_image = NULL;
  }
  if (xvimagesink->xvimage) {
    gst_buffer_unref (GST_BUFFER_CAST (xvimagesink->xvimage));
    xvimagesink->xvimage = NULL;
  }

  gst_xvimagesink_imagepool_clear (xvimagesink);

  if (xvimagesink->xwindow) {
    gst_xvimagesink_xwindow_clear (xvimagesink, xvimagesink->xwindow);
    gst_xvimagesink_xwindow_destroy (xvimagesink, xvimagesink->xwindow);
    xvimagesink->xwindow = NULL;
  }
#ifdef GST_EXT_XV_ENHANCEMENT
  if(xvimagesink->is_subpicture_format && xvimagesink->pixmap_for_subpicture) {
      GST_INFO("calling XvStopVideo() for %d port [pixmap : %p]", xvimagesink->xcontext->xv_port_id, xvimagesink->pixmap_for_subpicture);
      XvStopVideo(xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->pixmap_for_subpicture);
      xvimagesink->pixmap_for_subpicture = 0;
  }

  if (xvimagesink->get_pixmap_cb) {
    int i = 0;
    if (xvimagesink->xpixmap[0] && xvimagesink->xpixmap[0]->pixmap) {
      g_mutex_lock (xvimagesink->x_lock);
      GST_WARNING_OBJECT( xvimagesink, "calling XvStopVideo()" );
      XvStopVideo (xvimagesink->xcontext->disp, xvimagesink->xcontext->xv_port_id, xvimagesink->xpixmap[0]->pixmap);
      g_mutex_unlock (xvimagesink->x_lock);
    }
    for (i = 0; i < MAX_PIXMAP_NUM; i++) {
      if (xvimagesink->xpixmap[i]) {
        gst_xvimagesink_xpixmap_destroy (xvimagesink, xvimagesink->xpixmap[i]);
        xvimagesink->xpixmap[i] = NULL;
      }
    }
    xvimagesink->get_pixmap_cb = NULL;
    xvimagesink->get_pixmap_cb_user_data = NULL;
  }
#endif /* GST_EXT_XV_ENHANCEMENT */
  xvimagesink->render_rect.x = xvimagesink->render_rect.y =
      xvimagesink->render_rect.w = xvimagesink->render_rect.h = 0;
  xvimagesink->have_render_rect = FALSE;

  gst_xvimagesink_xcontext_clear (xvimagesink);
}

/* Finalize is called only once, dispose can be called multiple times.
 * We use mutexes and don't reset stuff to NULL here so let's register
 * as a finalize. */
static void
gst_xvimagesink_finalize (GObject * object)
{
  GstXvImageSink *xvimagesink;

  xvimagesink = GST_XVIMAGESINK (object);

  gst_xvimagesink_reset (xvimagesink);

  if (xvimagesink->display_name) {
    g_free (xvimagesink->display_name);
    xvimagesink->display_name = NULL;
  }

  if (xvimagesink->par) {
    g_free (xvimagesink->par);
    xvimagesink->par = NULL;
  }
  if (xvimagesink->x_lock) {
    g_mutex_free (xvimagesink->x_lock);
    xvimagesink->x_lock = NULL;
  }
  if (xvimagesink->flow_lock) {
    g_mutex_free (xvimagesink->flow_lock);
    xvimagesink->flow_lock = NULL;
  }
  if (xvimagesink->pool_lock) {
    g_mutex_free (xvimagesink->pool_lock);
    xvimagesink->pool_lock = NULL;
  }
#ifdef GST_EXT_XV_ENHANCEMENT
  if (xvimagesink->display_buffer_lock) {
    g_mutex_free (xvimagesink->display_buffer_lock);
    xvimagesink->display_buffer_lock = NULL;
  }
#endif /* GST_EXT_XV_ENHANCEMENT */

  g_free (xvimagesink->media_title);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_xvimagesink_init (GstXvImageSink * xvimagesink,
    GstXvImageSinkClass * xvimagesinkclass)
{
#ifdef GST_EXT_XV_ENHANCEMENT
  int i;
  int j;
#endif /* GST_EXT_XV_ENHANCEMENT */

  xvimagesink->display_name = NULL;
  xvimagesink->adaptor_no = 0;
  xvimagesink->xcontext = NULL;
  xvimagesink->xwindow = NULL;
  xvimagesink->xvimage = NULL;
  xvimagesink->cur_image = NULL;

  xvimagesink->hue = xvimagesink->saturation = 0;
  xvimagesink->contrast = xvimagesink->brightness = 0;
  xvimagesink->cb_changed = FALSE;

  xvimagesink->fps_n = 0;
  xvimagesink->fps_d = 0;
  xvimagesink->video_width = 0;
  xvimagesink->video_height = 0;

  xvimagesink->x_lock = g_mutex_new ();
  xvimagesink->flow_lock = g_mutex_new ();

  xvimagesink->image_pool = NULL;
  xvimagesink->pool_lock = g_mutex_new ();

  xvimagesink->synchronous = FALSE;
  xvimagesink->double_buffer = TRUE;
  xvimagesink->running = FALSE;
  xvimagesink->keep_aspect = FALSE;
  xvimagesink->handle_events = TRUE;
  xvimagesink->par = NULL;
  xvimagesink->handle_expose = TRUE;
  xvimagesink->autopaint_colorkey = TRUE;

  /* on 16bit displays this becomes r,g,b = 1,2,3
   * on 24bit displays this becomes r,g,b = 8,8,16
   * as a port atom value
   */
  xvimagesink->colorkey = (8 << 16) | (8 << 8) | 16;
  xvimagesink->draw_borders = TRUE;

#ifdef GST_EXT_XV_ENHANCEMENT
  xvimagesink->xid_updated = FALSE;
  xvimagesink->display_mode = DISPLAY_MODE_PRI_VIDEO_ON_AND_SEC_VIDEO_CLONE;
  xvimagesink->csc_range = CSC_RANGE_NARROW;
  xvimagesink->display_geometry_method = DEF_DISPLAY_GEOMETRY_METHOD;
  xvimagesink->flip = DEF_DISPLAY_FLIP;
  xvimagesink->rotate_angle = DEGREE_270;
  xvimagesink->visible = TRUE;
  xvimagesink->zoom = 1.0;
  xvimagesink->zoom_pos_x = -1;
  xvimagesink->zoom_pos_y = -1;
  xvimagesink->rotation = -1;
  xvimagesink->dst_roi_mode = DEF_ROI_DISPLAY_GEOMETRY_METHOD;
  xvimagesink->orientation = DEGREE_0;
  xvimagesink->dst_roi.x = 0;
  xvimagesink->dst_roi.y = 0;
  xvimagesink->dst_roi.w = 0;
  xvimagesink->dst_roi.h = 0;
  xvimagesink->src_crop.x = 0;
  xvimagesink->src_crop.y = 0;
  xvimagesink->src_crop.w = 0;
  xvimagesink->src_crop.h = 0;
  xvimagesink->xim_transparenter = NULL;
  xvimagesink->scr_w = 0;
  xvimagesink->scr_h = 0;
  xvimagesink->aligned_width = 0;
  xvimagesink->aligned_height = 0;
  xvimagesink->stop_video = FALSE;
  xvimagesink->is_hided = FALSE;
  xvimagesink->is_quick_panel_on = FALSE;
  xvimagesink->is_multi_window = FALSE;
  xvimagesink->drm_fd = -1;
  xvimagesink->current_pixmap_idx = -1;
  xvimagesink->get_pixmap_cb = NULL;
  xvimagesink->get_pixmap_cb_user_data = NULL;

  for (i = 0 ; i < DISPLAYING_BUFFERS_MAX_NUM ; i++) {
    xvimagesink->displaying_buffers[i].buffer = NULL;
    for (j = 0 ; j < XV_BUF_PLANE_NUM ; j++) {
      xvimagesink->displaying_buffers[i].gem_name[j] = 0;
      xvimagesink->displaying_buffers[i].gem_handle[j] = 0;
      xvimagesink->displaying_buffers[i].dmabuf_fd[j] = 0;
      xvimagesink->displaying_buffers[i].ref_count = 0;
    }
  }

  xvimagesink->display_buffer_lock = g_mutex_new ();

  xvimagesink->displayed_buffer_count = 0;
  xvimagesink->displaying_buffer_count = 0;
  xvimagesink->is_zero_copy_format = FALSE;
  xvimagesink->secure_path = SECURE_PATH_INIT;
  xvimagesink->drm_level = DRM_LEVEL_0;
  xvimagesink->last_added_buffer_index = -1;
  xvimagesink->bufmgr = NULL;
  xvimagesink->flush_buffer = NULL;
  xvimagesink->enable_flush_buffer = TRUE;
  xvimagesink->pixmap_for_subpicture = 0;
  xvimagesink->is_subpicture_format = FALSE;
  xvimagesink->set_overlay_for_subpicture_just_once = FALSE;
  xvimagesink->subpicture = FALSE;
  xvimagesink->external_width = 0;
  xvimagesink->external_height = 0;
  xvimagesink->skip_frame_due_to_external_dev = FALSE;
  xvimagesink->is_hided_subpicture = FALSE;
  xvimagesink->is_quick_panel_on_subpicture = FALSE;
  xvimagesink->is_multi_window_subpicture = FALSE;
  xvimagesink->keep_external_fullscreen_post = FALSE;
  xvimagesink->keep_external_fullscreen_prev = FALSE;
  if(!XInitThreads())
    GST_WARNING("FAIL to call XInitThreads()");
  if(vconf_set_int(VCONFKEY_XV_STATE, (xvimagesink->eos_received << 2) | XV_STATUS_NULL))
    GST_WARNING("vconf set fail");
#endif /* GST_EXT_XV_ENHANCEMENT */
}

static void
gst_xvimagesink_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details_simple (element_class,
      "Video sink", "Sink/Video",
      "A Xv based videosink", "Julien Moutte <julien@moutte.net>");

  gst_element_class_add_static_pad_template (element_class,
      &gst_xvimagesink_sink_template_factory);
}

static void
gst_xvimagesink_class_init (GstXvImageSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstVideoSinkClass *videosink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  videosink_class = (GstVideoSinkClass *) klass;

  gobject_class->set_property = gst_xvimagesink_set_property;
  gobject_class->get_property = gst_xvimagesink_get_property;

  g_object_class_install_property (gobject_class, PROP_CONTRAST,
      g_param_spec_int ("contrast", "Contrast", "The contrast of the video",
          -1000, 1000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BRIGHTNESS,
      g_param_spec_int ("brightness", "Brightness",
          "The brightness of the video", -1000, 1000, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HUE,
      g_param_spec_int ("hue", "Hue", "The hue of the video", -1000, 1000, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SATURATION,
      g_param_spec_int ("saturation", "Saturation",
          "The saturation of the video", -1000, 1000, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DISPLAY,
      g_param_spec_string ("display", "Display", "X Display name", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SYNCHRONOUS,
      g_param_spec_boolean ("synchronous", "Synchronous",
          "When enabled, runs the X display in synchronous mode. "
          "(unrelated to A/V sync, used only for debugging)", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PIXEL_ASPECT_RATIO,
      g_param_spec_string ("pixel-aspect-ratio", "Pixel Aspect Ratio",
          "The pixel aspect ratio of the device", "1/1",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio", "Force aspect ratio",
          "When enabled, scaling will respect original aspect ratio", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HANDLE_EVENTS,
      g_param_spec_boolean ("handle-events", "Handle XEvents",
          "When enabled, XEvents will be selected and handled", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "Adaptor number",
          "The number of the video adaptor", "0",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE_NAME,
      g_param_spec_string ("device-name", "Adaptor name",
          "The name of the video adaptor", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_EXTERNAL_WIDTH,
      g_param_spec_int ("external-width", "external width",
          "width of external display", 0, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_EXTERNAL_HEIGHT,
      g_param_spec_int ("external-height", "external height",
          "height of external display", 0, G_MAXINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXvImageSink:handle-expose
   *
   * When enabled, the current frame will always be drawn in response to X
   * Expose.
   *
   * Since: 0.10.14
   */
  g_object_class_install_property (gobject_class, PROP_HANDLE_EXPOSE,
      g_param_spec_boolean ("handle-expose", "Handle expose",
          "When enabled, "
          "the current frame will always be drawn in response to X Expose "
          "events", TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:double-buffer
   *
   * Whether to double-buffer the output.
   *
   * Since: 0.10.14
   */
  g_object_class_install_property (gobject_class, PROP_DOUBLE_BUFFER,
      g_param_spec_boolean ("double-buffer", "Double-buffer",
          "Whether to double-buffer the output", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXvImageSink:autopaint-colorkey
   *
   * Whether to autofill overlay with colorkey
   *
   * Since: 0.10.21
   */
  g_object_class_install_property (gobject_class, PROP_AUTOPAINT_COLORKEY,
      g_param_spec_boolean ("autopaint-colorkey", "Autofill with colorkey",
          "Whether to autofill overlay with colorkey", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstXvImageSink:colorkey
   *
   * Color to use for the overlay mask.
   *
   * Since: 0.10.21
   */
  g_object_class_install_property (gobject_class, PROP_COLORKEY,
      g_param_spec_int ("colorkey", "Colorkey",
          "Color to use for the overlay mask", G_MININT, G_MAXINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:draw-borders
   *
   * Draw black borders when using GstXvImageSink:force-aspect-ratio to fill
   * unused parts of the video area.
   *
   * Since: 0.10.21
   */
  g_object_class_install_property (gobject_class, PROP_DRAW_BORDERS,
      g_param_spec_boolean ("draw-borders", "Colorkey",
          "Draw black borders to fill unused area in force-aspect-ratio mode",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:window-width
   *
   * Actual width of the video window.
   *
   * Since: 0.10.32
   */
  g_object_class_install_property (gobject_class, PROP_WINDOW_WIDTH,
      g_param_spec_uint64 ("window-width", "window-width",
          "Width of the window", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:window-height
   *
   * Actual height of the video window.
   *
   * Since: 0.10.32
   */
  g_object_class_install_property (gobject_class, PROP_WINDOW_HEIGHT,
      g_param_spec_uint64 ("window-height", "window-height",
          "Height of the window", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

#ifdef GST_EXT_XV_ENHANCEMENT
  /**
   * GstXvImageSink:display-mode
   *
   * select display mode
   */
  g_object_class_install_property(gobject_class, PROP_DISPLAY_MODE,
    g_param_spec_enum("display-mode", "Display Mode",
      "Display device setting",
      GST_TYPE_XVIMAGESINK_DISPLAY_MODE, DISPLAY_MODE_PRI_VIDEO_ON_AND_SEC_VIDEO_CLONE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:csc-range
   *
   * select color space range
   */
  g_object_class_install_property(gobject_class, PROP_CSC_RANGE,
    g_param_spec_enum("csc-range", "Color Space Range",
      "Color space range setting",
      GST_TYPE_XVIMAGESINK_CSC_RANGE, CSC_RANGE_NARROW,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:display-geometry-method
   *
   * Display geometrical method setting
   */
  g_object_class_install_property(gobject_class, PROP_DISPLAY_GEOMETRY_METHOD,
    g_param_spec_enum("display-geometry-method", "Display geometry method",
      "Geometrical method for display",
      GST_TYPE_XVIMAGESINK_DISPLAY_GEOMETRY_METHOD, DEF_DISPLAY_GEOMETRY_METHOD,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:display-flip
   *
   * Display flip setting
   */
  g_object_class_install_property(gobject_class, PROP_FLIP,
    g_param_spec_enum("flip", "Display flip",
      "Flip for display",
      GST_TYPE_XVIMAGESINK_FLIP, DEF_DISPLAY_FLIP,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:rotate
   *
   * Draw rotation angle setting
   */
  g_object_class_install_property(gobject_class, PROP_ROTATE_ANGLE,
    g_param_spec_enum("rotate", "Rotate angle",
      "Rotate angle of display output",
      GST_TYPE_XVIMAGESINK_ROTATE_ANGLE, DEGREE_270,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:visible
   *
   * Whether reserve original src size or not
   */
  g_object_class_install_property (gobject_class, PROP_VISIBLE,
      g_param_spec_boolean ("visible", "Visible",
          "Draws screen or blacks out, true means visible, false blacks out",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:zoom
   *
   * Scale small area of screen to 1X~ 9X
   */
  g_object_class_install_property (gobject_class, PROP_ZOOM,
      g_param_spec_float ("zoom", "Zoom",
          "Zooms screen as nX", 1.0, 9.0, 1.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:zoom-pos-x
   *
   * Standard x-position of zoom
   */
  g_object_class_install_property (gobject_class, PROP_ZOOM_POS_X,
      g_param_spec_int ("zoom-pos-x", "Zoom Position X",
          "Standard x-position of zoom", -1, 3840, -1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:zoom-pos-y
   *
   * Standard y-position of zoom
   */
  g_object_class_install_property (gobject_class, PROP_ZOOM_POS_Y,
      g_param_spec_int ("zoom-pos-y", "Zoom Position Y",
          "Standard y-position of zoom", -1, 3840, -1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:dst-roi-mode
   *
   * Display geometrical method of ROI setting
   */
  g_object_class_install_property(gobject_class, PROP_DST_ROI_MODE,
    g_param_spec_enum("dst-roi-mode", "Display geometry method of ROI",
      "Geometrical method of ROI for display",
      GST_TYPE_XVIMAGESINK_ROI_DISPLAY_GEOMETRY_METHOD, DEF_ROI_DISPLAY_GEOMETRY_METHOD,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:orientation
   *
   * Orientation information which will be used for ROI/ZOOM
   */
  g_object_class_install_property(gobject_class, PROP_ORIENTATION,
    g_param_spec_enum("orientation", "Orientation information used for ROI/ZOOM",
      "Orientation information for display",
      GST_TYPE_XVIMAGESINK_ROTATE_ANGLE, DEGREE_0,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:dst-roi-x
   *
   * X value of Destination ROI
   */
  g_object_class_install_property (gobject_class, PROP_DST_ROI_X,
      g_param_spec_int ("dst-roi-x", "Dst-ROI-X",
          "X value of Destination ROI(only effective \"CUSTOM_ROI\")", 0, XV_SCREEN_SIZE_WIDTH, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:dst-roi-y
   *
   * Y value of Destination ROI
   */
  g_object_class_install_property (gobject_class, PROP_DST_ROI_Y,
      g_param_spec_int ("dst-roi-y", "Dst-ROI-Y",
          "Y value of Destination ROI(only effective \"CUSTOM_ROI\")", 0, XV_SCREEN_SIZE_HEIGHT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:dst-roi-w
   *
   * W value of Destination ROI
   */
  g_object_class_install_property (gobject_class, PROP_DST_ROI_W,
      g_param_spec_int ("dst-roi-w", "Dst-ROI-W",
          "W value of Destination ROI(only effective \"CUSTOM_ROI\")", 0, XV_SCREEN_SIZE_WIDTH, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:dst-roi-h
   *
   * H value of Destination ROI
   */
  g_object_class_install_property (gobject_class, PROP_DST_ROI_H,
      g_param_spec_int ("dst-roi-h", "Dst-ROI-H",
          "H value of Destination ROI(only effective \"CUSTOM_ROI\")", 0, XV_SCREEN_SIZE_HEIGHT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:stop-video
   *
   * Stop video for releasing video source buffer
   */
  g_object_class_install_property (gobject_class, PROP_STOP_VIDEO,
      g_param_spec_int ("stop-video", "Stop-Video",
          "Stop video for releasing video source buffer", 0, 1, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PIXMAP_CB,
      g_param_spec_pointer("pixmap-id-callback", "Pixmap-Id-Callback",
          "pointer of callback function for getting pixmap id", G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_PIXMAP_CB_USER_DATA,
      g_param_spec_pointer("pixmap-id-callback-userdata", "Pixmap-Id-Callback-Userdata",
          "pointer of user data of callback function for getting pixmap id", G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SUBPICTURE,
      g_param_spec_boolean ("subpicture", "Subpicture",
          "identifier of player for supporting subpicture", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:src-crop-x
   *
   * X value of video source crop
   */
  g_object_class_install_property (gobject_class, PROP_SRC_CROP_X,
      g_param_spec_int ("src-crop-x", "Source Crop X",
          "X value of Video source crop(only available if LETTER_BOX or FULL_SCREEN mode)", 0, XV_SCREEN_SIZE_WIDTH, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:src-crop-y
   *
   * Y value of video source crop
   */
  g_object_class_install_property (gobject_class, PROP_SRC_CROP_Y,
      g_param_spec_int ("src-crop-y", "Source Crop X",
          "Y value of Video source crop(only available if LETTER_BOX or FULL_SCREEN mode)", 0, XV_SCREEN_SIZE_WIDTH, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:src-crop-w
   *
   * Width value of video source crop
   */
  g_object_class_install_property (gobject_class, PROP_SRC_CROP_W,
      g_param_spec_int ("src-crop-w", "Source Crop Width",
          "Width value of Video source crop(only available if LETTER_BOX or FULL_SCREEN mode)", 0, XV_SCREEN_SIZE_WIDTH, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:src-crop-h
   *
   * Height value of video source crop
   */
  g_object_class_install_property (gobject_class, PROP_SRC_CROP_H,
      g_param_spec_int ("src-crop-h", "Source Crop Height",
          "Height value of Video source crop(only available if LETTER_BOX or FULL_SCREEN mode)", 0, XV_SCREEN_SIZE_WIDTH, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:enable-flush-buffer
   *
   * Enable flush buffer mechanism when state change(PAUSED_TO_READY)
   */
  g_object_class_install_property (gobject_class, PROP_ENABLE_FLUSH_BUFFER,
      g_param_spec_boolean("enable-flush-buffer", "Enable flush buffer mechanism",
          "Enable flush buffer mechanism when state change(PAUSED_TO_READY)",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:is-pixmap
   *
   * Check using pixmap id for blocking X api
   */
  g_object_class_install_property (gobject_class, PROP_PIXMAP,
      g_param_spec_boolean("is-pixmap", "Check if use pixmap",
          "Check using pixmap id for blocking X api",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:hided-window
   *
   * check window status for hiding subtitle
   */
  g_object_class_install_property (gobject_class, PROP_HIDED_WINDOW,
      g_param_spec_boolean("hided-window", "Hided window",
          "check window status for hiding subtitle",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:quick-panel-on
   *
   * check quick-panel status for hiding subtitle
   */
  g_object_class_install_property (gobject_class, PROP_QUICKPANEL_ON,
      g_param_spec_boolean("quick-panel-on", "Quick panel On",
          "check quick-panel status for hiding subtitle",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:multiwindow-activated
   *
   * check multiwindow-activated status for hiding subtitle
   */
  g_object_class_install_property (gobject_class, PROP_MULTIWINDOW_ACTIVE,
      g_param_spec_boolean("multiwindow-active", "Multiwindow Activate",
          "check multiwindow status for hiding subtitle",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:keep-external-fullscreen-post
   *
   * keep video-only mode for special case, it is set immediately.
   */
  g_object_class_install_property (gobject_class, PROP_KEEP_EXTERNAL_FULLSCREEN_POST,
      g_param_spec_boolean("keep-external-fullscreen-post", "Keep external display to full screen",
          "set video-only mode forcedly for special case",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink:keep-external-fullscreen-prev
   *
   * keep video-only mode for special case, it is set in advance.
   */
  g_object_class_install_property (gobject_class, PROP_KEEP_EXTERNAL_FULLSCREEN_PREV,
      g_param_spec_boolean("keep-external-fullscreen-prev", "Keep external display to full screen",
          "set video-only mode forcedly for special case",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstXvImageSink::frame-render-error
   */
  gst_xvimagesink_signals[SIGNAL_FRAME_RENDER_ERROR] = g_signal_new (
          "frame-render-error",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST,
          0,
          NULL,
          NULL,
          gst_xvimagesink_BOOLEAN__POINTER,
          G_TYPE_BOOLEAN,
          1,
          G_TYPE_POINTER);
  /**
   * GstXvImageSink::display-status
   */
    gst_xvimagesink_signals[SIGNAL_DISPLAY_STATUS] = g_signal_new (
          "display-status",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
          0,
          NULL,
          NULL,
          g_cclosure_marshal_VOID__INT,
          G_TYPE_NONE,
          1,
          G_TYPE_INT);

  /**
   * GstXvImageSink::external-resolution
   */
  gst_xvimagesink_signals[SIGNAL_EXTERNAL_RESOLUTION] = g_signal_new (
          "external-resolution",
         G_TYPE_FROM_CLASS (klass),
         G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
         0,
         NULL,
         NULL,
         gst_marshal_VOID__INT_INT,
         G_TYPE_NONE,
         2,
         G_TYPE_INT,
         G_TYPE_INT);

  /**
   * GstXvImageSink::hided-window
   */
    gst_xvimagesink_signals[SIGNAL_WINDOW_STATUS] = g_signal_new (
          "hided-window",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
          0,
          NULL,
          NULL,
          g_cclosure_marshal_VOID__BOOLEAN,
          G_TYPE_NONE,
          1,
          G_TYPE_BOOLEAN);

  /**
   * GstXvImageSink::quick-panel-on
   */
    gst_xvimagesink_signals[SIGNAL_QUICKPANEL_STATUS] = g_signal_new (
          "quick-panel-on",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
          0,
          NULL,
          NULL,
          g_cclosure_marshal_VOID__BOOLEAN,
          G_TYPE_NONE,
          1,
          G_TYPE_BOOLEAN);

  /**
   * GstXvImageSink::multiwindow-active
   */
    gst_xvimagesink_signals[SIGNAL_MULTIWINDOW_STATUS] = g_signal_new (
          "multiwindow-active",
          G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
          0,
          NULL,
          NULL,
          g_cclosure_marshal_VOID__BOOLEAN,
          G_TYPE_NONE,
          1,
          G_TYPE_BOOLEAN);

#endif /* GST_EXT_XV_ENHANCEMENT */

  gobject_class->finalize = gst_xvimagesink_finalize;

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_xvimagesink_change_state);

  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_xvimagesink_getcaps);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_xvimagesink_setcaps);
  gstbasesink_class->buffer_alloc =
      GST_DEBUG_FUNCPTR (gst_xvimagesink_buffer_alloc);
  gstbasesink_class->get_times = GST_DEBUG_FUNCPTR (gst_xvimagesink_get_times);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_xvimagesink_event);

  videosink_class->show_frame = GST_DEBUG_FUNCPTR (gst_xvimagesink_show_frame);
}

/* ============================================================= */
/*                                                               */
/*                       Public Methods                          */
/*                                                               */
/* ============================================================= */

/* =========================================== */
/*                                             */
/*          Object typing & Creation           */
/*                                             */
/* =========================================== */
static void
gst_xvimagesink_init_interfaces (GType type)
{
  static const GInterfaceInfo iface_info = {
    (GInterfaceInitFunc) gst_xvimagesink_interface_init,
    NULL,
    NULL,
  };
  static const GInterfaceInfo navigation_info = {
    (GInterfaceInitFunc) gst_xvimagesink_navigation_init,
    NULL,
    NULL,
  };
  static const GInterfaceInfo overlay_info = {
    (GInterfaceInitFunc) gst_xvimagesink_xoverlay_init,
    NULL,
    NULL,
  };
  static const GInterfaceInfo colorbalance_info = {
    (GInterfaceInitFunc) gst_xvimagesink_colorbalance_init,
    NULL,
    NULL,
  };
  static const GInterfaceInfo propertyprobe_info = {
    (GInterfaceInitFunc) gst_xvimagesink_property_probe_interface_init,
    NULL,
    NULL,
  };

  g_type_add_interface_static (type,
      GST_TYPE_IMPLEMENTS_INTERFACE, &iface_info);
  g_type_add_interface_static (type, GST_TYPE_NAVIGATION, &navigation_info);
  g_type_add_interface_static (type, GST_TYPE_X_OVERLAY, &overlay_info);
  g_type_add_interface_static (type, GST_TYPE_COLOR_BALANCE,
      &colorbalance_info);
  g_type_add_interface_static (type, GST_TYPE_PROPERTY_PROBE,
      &propertyprobe_info);

  /* register type and create class in a more safe place instead of at
   * runtime since the type registration and class creation is not
   * threadsafe. */
  g_type_class_ref (gst_xvimage_buffer_get_type ());
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "xvimagesink",
          GST_RANK_PRIMARY, GST_TYPE_XVIMAGESINK))
    return FALSE;

  GST_DEBUG_CATEGORY_INIT (gst_debug_xvimagesink, "xvimagesink", 0,
      "xvimagesink element");
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "xvimagesink",
    "XFree86 video output plugin using Xv extension",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)



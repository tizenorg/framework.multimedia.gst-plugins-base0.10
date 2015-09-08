/* GStreamer
 * Copyright (C) <2005> Julien Moutte <julien@moutte.net>
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

#ifndef __GST_XVIMAGESINK_H__
#define __GST_XVIMAGESINK_H__

#include <gst/video/gstvideosink.h>

#ifdef HAVE_XSHM
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif /* HAVE_XSHM */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifdef HAVE_XSHM
#include <X11/extensions/XShm.h>
#endif /* HAVE_XSHM */

#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#ifdef GST_EXT_XV_ENHANCEMENT
#include <X11/Xatom.h>
#include <stdio.h>
#include "xv_types.h"
#include <utilX.h>
#include <utilX_ext.h>
#endif

#include <string.h>
#include <math.h>
#include <stdlib.h>

G_BEGIN_DECLS

#define GST_TYPE_XVIMAGESINK \
  (gst_xvimagesink_get_type())
#define GST_XVIMAGESINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_XVIMAGESINK, GstXvImageSink))
#define GST_XVIMAGESINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_XVIMAGESINK, GstXvImageSinkClass))
#define GST_IS_XVIMAGESINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_XVIMAGESINK))
#define GST_IS_XVIMAGESINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_XVIMAGESINK))

#ifdef GST_EXT_XV_ENHANCEMENT
#define XV_SCREEN_SIZE_WIDTH            4096
#define XV_SCREEN_SIZE_HEIGHT           4096
#define DISPLAYING_BUFFERS_MAX_NUM      10

#define MAX_PIXMAP_NUM 10
typedef uint (*get_pixmap_callback)(void *user_data);
typedef struct _GstXPixmap GstXPixmap;
typedef struct _GstXvImageDisplayingBuffer GstXvImageDisplayingBuffer;
typedef struct _GstXvImageFlushBuffer GstXvImageFlushBuffer;
#endif /* GST_EXT_XV_ENHANCEMENT */

typedef struct _GstXContext GstXContext;
typedef struct _GstXWindow GstXWindow;
typedef struct _GstXvImageFormat GstXvImageFormat;
typedef struct _GstXvImageBuffer GstXvImageBuffer;
typedef struct _GstXvImageBufferClass GstXvImageBufferClass;

typedef struct _GstXvImageSink GstXvImageSink;
typedef struct _GstXvImageSinkClass GstXvImageSinkClass;

/*
 * GstXContext:
 * @disp: the X11 Display of this context
 * @screen: the default Screen of Display @disp
 * @screen_num: the Screen number of @screen
 * @visual: the default Visual of Screen @screen
 * @root: the root Window of Display @disp
 * @white: the value of a white pixel on Screen @screen
 * @black: the value of a black pixel on Screen @screen
 * @depth: the color depth of Display @disp
 * @bpp: the number of bits per pixel on Display @disp
 * @endianness: the endianness of image bytes on Display @disp
 * @width: the width in pixels of Display @disp
 * @height: the height in pixels of Display @disp
 * @widthmm: the width in millimeters of Display @disp
 * @heightmm: the height in millimeters of Display @disp
 * @par: the pixel aspect ratio calculated from @width, @widthmm and @height,
 * @heightmm ratio
 * @use_xshm: used to known wether of not XShm extension is usable or not even
 * if the Extension is present
 * @xv_port_id: the XVideo port ID
 * @im_format: used to store at least a valid format for XShm calls checks
 * @formats_list: list of supported image formats on @xv_port_id
 * @channels_list: list of #GstColorBalanceChannels
 * @caps: the #GstCaps that Display @disp can accept
 *
 * Structure used to store various informations collected/calculated for a
 * Display.
 */
struct _GstXContext {
  Display *disp;

  Screen *screen;
  gint screen_num;

  Visual *visual;

  Window root;

  gulong white, black;

  gint depth;
  gint bpp;
  gint endianness;

  gint width, height;
  gint widthmm, heightmm;
  GValue *par;                  /* calculated pixel aspect ratio */

  gboolean use_xshm;

  XvPortID xv_port_id;
  guint nb_adaptors;
  gchar ** adaptors;
  gint im_format;

  GList *formats_list;
  GList *channels_list;

  GstCaps *caps;

  /* Optimisation storage for buffer_alloc return */
  GstCaps *last_caps;
  gint last_format;
  gint last_width;
  gint last_height;
};

/*
 * GstXWindow:
 * @win: the Window ID of this X11 window
 * @width: the width in pixels of Window @win
 * @height: the height in pixels of Window @win
 * @internal: used to remember if Window @win was created internally or passed
 * through the #GstXOverlay interface
 * @gc: the Graphical Context of Window @win
 *
 * Structure used to store informations about a Window.
 */
struct _GstXWindow {
  Window win;
#ifdef GST_EXT_XV_ENHANCEMENT
  gint x, y;
#endif
  gint width, height;
  gboolean internal;
  GC gc;
};

#ifdef GST_EXT_XV_ENHANCEMENT
struct _GstXPixmap {
	Window pixmap;
	gint x, y;
	gint width, height;
	GC gc;
};

struct _GstXvImageDisplayingBuffer {
	GstBuffer *buffer;
	unsigned int dmabuf_fd[XV_BUF_PLANE_NUM];
	unsigned int gem_name[XV_BUF_PLANE_NUM];
	unsigned int gem_handle[XV_BUF_PLANE_NUM];
	void *bo[XV_BUF_PLANE_NUM];
	unsigned int ref_count;
};

struct _GstXvImageFlushBuffer {
	unsigned int gem_name[XV_BUF_PLANE_NUM];
	void *bo[XV_BUF_PLANE_NUM];
};
#endif

/**
 * GstXvImageFormat:
 * @format: the image format
 * @caps: generated #GstCaps for this image format
 *
 * Structure storing image format to #GstCaps association.
 */
struct _GstXvImageFormat {
  gint format;
  GstCaps *caps;
};

/**
 * GstXImageBuffer:
 * @xvimagesink: a reference to our #GstXvImageSink
 * @xvimage: the XvImage of this buffer
 * @width: the width in pixels of XvImage @xvimage
 * @height: the height in pixels of XvImage @xvimage
 * @im_format: the image format of XvImage @xvimage
 * @size: the size in bytes of XvImage @xvimage
 *
 * Subclass of #GstBuffer containing additional information about an XvImage.
 */
struct _GstXvImageBuffer {
  GstBuffer   buffer;

  /* Reference to the xvimagesink we belong to */
  GstXvImageSink *xvimagesink;

  XvImage *xvimage;

#ifdef HAVE_XSHM
  XShmSegmentInfo SHMInfo;
#endif /* HAVE_XSHM */

  gint width, height, im_format;
  size_t size;
#ifdef GST_EXT_XV_ENHANCEMENT
  GstBuffer *current_buffer;
#endif /* GST_EXT_XV_ENHANCEMENT */
};

#ifdef GST_EXT_XV_ENHANCEMENT
#define MAX_PLANE_NUM          4
#endif /* GST_EXT_XV_ENHANCEMENT */

/**
 * GstXvImageSink:
 * @display_name: the name of the Display we want to render to
 * @xcontext: our instance's #GstXContext
 * @xwindow: the #GstXWindow we are rendering to
 * @xvimage: internal #GstXvImage used to store incoming buffers and render when
 * not using the buffer_alloc optimization mechanism
 * @cur_image: a reference to the last #GstXvImage that was put to @xwindow. It
 * is used when Expose events are received to redraw the latest video frame
 * @event_thread: a thread listening for events on @xwindow and handling them
 * @running: used to inform @event_thread if it should run/shutdown
 * @fps_n: the framerate fraction numerator
 * @fps_d: the framerate fraction denominator
 * @x_lock: used to protect X calls as we are not using the XLib in threaded
 * mode
 * @flow_lock: used to protect data flow routines from external calls such as
 * events from @event_thread or methods from the #GstXOverlay interface
 * @par: used to override calculated pixel aspect ratio from @xcontext
 * @pool_lock: used to protect the buffer pool
 * @image_pool: a list of #GstXvImageBuffer that could be reused at next buffer
 * allocation call
 * @synchronous: used to store if XSynchronous should be used or not (for
 * debugging purpose only)
 * @keep_aspect: used to remember if reverse negotiation scaling should respect
 * aspect ratio
 * @handle_events: used to know if we should handle select XEvents or not
 * @brightness: used to store the user settings for color balance brightness
 * @contrast: used to store the user settings for color balance contrast
 * @hue: used to store the user settings for color balance hue
 * @saturation: used to store the user settings for color balance saturation
 * @cb_changed: used to store if the color balance settings where changed
 * @video_width: the width of incoming video frames in pixels
 * @video_height: the height of incoming video frames in pixels
 *
 * The #GstXvImageSink data structure.
 */
struct _GstXvImageSink {
  /* Our element stuff */
  GstVideoSink videosink;

  char *display_name;
  guint adaptor_no;

  GstXContext *xcontext;
  GstXWindow *xwindow;
  GstXvImageBuffer *xvimage;
  GstXvImageBuffer *cur_image;
#ifdef GST_EXT_XV_ENHANCEMENT
  GstXvImageBuffer *last_image;
  unsigned int last_image_vaddr[4];
#endif

  GThread *event_thread;
  gboolean running;

  gint fps_n;
  gint fps_d;

  GMutex *x_lock;
  GMutex *flow_lock;

  /* object-set pixel aspect ratio */
  GValue *par;

  GMutex *pool_lock;
  gboolean pool_invalid;
  GSList *image_pool;

  gboolean synchronous;
  gboolean double_buffer;
  gboolean keep_aspect;
  gboolean redraw_border;
  gboolean handle_events;
  gboolean handle_expose;

  gint brightness;
  gint contrast;
  gint hue;
  gint saturation;
  gboolean cb_changed;

  /* size of incoming video, used as the size for XvImage */
  guint video_width, video_height;

  /* display sizes, used for clipping the image */
  gint disp_x, disp_y;
  gint disp_width, disp_height;

  /* port attributes */
  gboolean autopaint_colorkey;
  gint colorkey;

  gboolean draw_borders;

  /* port features */
  gboolean have_autopaint_colorkey;
  gboolean have_colorkey;
  gboolean have_double_buffer;

  /* stream metadata */
  gchar *media_title;

  /* target video rectangle */
  GstVideoRectangle render_rect;
  gboolean have_render_rect;

#ifdef GST_EXT_XV_ENHANCEMENT
  /* display mode */
  gboolean is_pixmap;
  gboolean xid_updated;
  guint display_mode;
  guint csc_range;
  guint display_geometry_method;
  guint flip;
  guint rotate_angle;
  gboolean visible;
  gfloat zoom;
  guint zoom_pos_x;
  guint zoom_pos_y;
  guint rotation;
  guint rotate_cnt;
  guint orientation;
  guint dst_roi_mode;
  GstVideoRectangle dst_roi;
  XImage* xim_transparenter;
  guint scr_w, scr_h;
  gboolean stop_video;
  gboolean is_hided;
  gboolean is_quick_panel_on;
  gboolean is_multi_window;
  gboolean is_during_seek;
  GstVideoRectangle src_crop;

  /* needed if fourcc is one if S series */
  guint aligned_width;
  guint aligned_height;
  gint drm_fd;
  void *bufmgr;

  /* for using multiple pixmaps */
  GstXPixmap *xpixmap[MAX_PIXMAP_NUM];
  gint current_pixmap_idx;
  get_pixmap_callback get_pixmap_cb;
  void* get_pixmap_cb_user_data;

  /* for sync displaying buffers */
  GstXvImageDisplayingBuffer displaying_buffers[DISPLAYING_BUFFERS_MAX_NUM];
  GMutex *display_buffer_lock;

  /* buffer count check */
  guint displayed_buffer_count;
  guint displaying_buffer_count;

  /* zero copy format */
  gboolean is_zero_copy_format;

  /* secure contents path */
  gint secure_path;

  /* DRM level */
  gint drm_level;

#ifdef GST_EXT_ENABLE_HEVC
  /* if needed combine planes data */
  gint need_combine_data;
#endif

  /* display request time */
  struct timeval request_time[DISPLAYING_BUFFERS_MAX_NUM];

  /* last added buffer index */
  gint last_added_buffer_index;

  /* flush buffer */
  GstXvImageFlushBuffer *flush_buffer;
  gboolean enable_flush_buffer;

  /* subtitle format */
  Pixmap pixmap_for_subpicture;
  gboolean is_subpicture_format;
  gboolean set_overlay_for_subpicture_just_once;
  gboolean subpicture;
  gboolean is_hided_subpicture;
  gboolean is_quick_panel_on_subpicture;
  gboolean is_multi_window_subpicture;

  /* external display */
  gint external_width;
  gint external_height;
  gboolean skip_frame_due_to_external_dev;
  gboolean keep_external_fullscreen_post;
  gboolean keep_external_fullscreen_prev;

  gboolean eos_received;

#endif /* GST_EXT_XV_ENHANCEMENT */
};

#ifdef GST_EXT_XV_ENHANCEMENT
/* max plane count *********************************************************/
#define MPLANE_IMGB_MAX_COUNT         (4)

/* image buffer definition ***************************************************

    +------------------------------------------+ ---
    |                                          |  ^
    |     uaddr[], index[]                     |  |
    |     +---------------------------+ ---    |  |
    |     |                           |  ^     |  |
    |     |<-------- width[] -------->|  |     |  |
    |     |                           |  |     |  |
    |     |                           |        |
    |     |                           |height[]|elevation[]
    |     |                           |        |
    |     |                           |  |     |  |
    |     |                           |  |     |  |
    |     |                           |  v     |  |
    |     +---------------------------+ ---    |  |
    |                                          |  v
    +------------------------------------------+ ---

    |<----------------- stride[] ------------------>|
*/
typedef struct _GstMultiPlaneImageBuffer GstMultiPlaneImageBuffer;
struct _GstMultiPlaneImageBuffer
{
    GstBuffer buffer;

    /* width of each image plane */
    gint      width[MPLANE_IMGB_MAX_COUNT];
    /* height of each image plane */
    gint      height[MPLANE_IMGB_MAX_COUNT];
    /* stride of each image plane */
    gint      stride[MPLANE_IMGB_MAX_COUNT];
    /* elevation of each image plane */
    gint      elevation[MPLANE_IMGB_MAX_COUNT];
    /* user space address of each image plane */
    gpointer uaddr[MPLANE_IMGB_MAX_COUNT];
    /* Index of real address of each image plane, if needs */
    gpointer index[MPLANE_IMGB_MAX_COUNT];
    /* left postion, if needs */
    gint      x;
    /* top position, if needs */
    gint      y;
    /* to align memory */
    gint      __dummy2;
    /* arbitrary data */
    gint      data[16];
};
#endif /* GST_EXT_XV_ENHANCEMENT */

struct _GstXvImageSinkClass {
  GstVideoSinkClass parent_class;
};

GType gst_xvimagesink_get_type(void);

G_END_DECLS

#endif /* __GST_XVIMAGESINK_H__ */

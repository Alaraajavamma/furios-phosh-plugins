/*
 * Copyright (C) 2026 alaraajavamma
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: alaraajavamma <aki@urheiluaki.fi>
 */

#include "camera-lockscreen.h"

#include <gio/gunixfdlist.h>
#include <glib/gi18n.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <unistd.h>

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_CAMERA_IFACE "org.freedesktop.portal.Camera"
#define PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define FLASHLIGHT_BUS "io.furios.Flashlightd"
#define FLASHLIGHT_PATH "/io/furios/Flashlightd"
#define FLASHLIGHT_IFACE "io.furios.Flashlightd"
#define PORTAL_CALL_TIMEOUT_MS 5000
#define FLASHLIGHT_CALL_TIMEOUT_MS 2000
#define ACCESS_RESPONSE_TIMEOUT_MS 15000
#define DEVICE_POLL_INTERVAL_MS 150
#define DEVICE_POLL_TIMEOUT_MS 6000
#define CAMERA_BACK 0
#define CAMERA_FRONT 1
#define CAMERA_COUNT 2
#define VIEWFINDER_WIDTH 640
#define VIEWFINDER_FRAMERATE 15
#define VIEWFINDER_CORNER_RADIUS 12.0
#define ASPECT_EPSILON 0.001
#define CARD_WIDTH_REQUEST 96
#define CARD_HEIGHT_FRACTION 0.78
#define CARD_HEIGHT_FALLBACK 400
#define PIPELINE_DRAIN_TIMEOUT_NS (2 * GST_SECOND)
#define SNAP_TIMEOUT_MS 4000
#define CAPTURE_FEEDBACK_MS 900
#define CAPTURE_NAME_FORMAT "IMG_%Y%m%d_%H%M%S.jpg"
#define RECORD_NAME_FORMAT "VID_%Y%m%d_%H%M%S.mkv"
#define RECORD_TIMER_INTERVAL_MS 500
#define RECORD_JPEG_QUALITY 70
#define EOS_TIMEOUT_MS 3000
#define IDLE_TIMEOUT_SECONDS 30

/**
 * PhoshCameraLockscreen
 *
 * A camera widget for the lock screen.
 *
 * The camera is off until the card is tapped: a viewfinder running on an
 * idle lock screen is both a battery drain and a privacy problem. The
 * pipeline lives in the compositor's own process, so every failure path
 * has to tear it down rather than leave the shell holding a broken
 * element, and every way out of the widget has to put the torch out.
 */
struct _PhoshCameraLockscreen {
  GtkBox             parent;

  GtkStack          *stack;
  GtkOverlay        *viewfinder;
  GtkButton         *shutter;
  GtkImage          *shutter_icon;
  GtkToggleButton   *light;
  GtkImage          *light_icon;
  GtkButton         *camera_switch;
  GtkToggleButton   *mode;
  GtkWidget         *recording_badge;
  GtkLabel          *timer_label;

  GCancellable      *cancel;
  GDBusConnection   *bus;
  guint              response_id;
  guint              response_timeout_id;
  gboolean           access_granted;

  GstDeviceProvider *provider;
  int                provider_fd;
  guint              poll_id;
  gint64             poll_deadline;
  int                target[CAMERA_COUNT];
  int                camera;

  GtkDrawingArea    *preview;
  GMutex             frame_lock;
  GstSample         *frame;

  GstElement        *pipeline;
  guint              watch_id;
  int                portal_fd;

  int                capture_pending;
  guint              snap_timeout_id;
  guint              feedback_id;

  GstElement        *record_bin;
  GstPad            *tee_record_pad;
  gboolean           recording;
  gboolean           eos_sent;
  gint64             record_start;
  guint              timer_id;
  guint              eos_timeout_id;
  guint              idle_id;

  guint              max_brightness;

  GtkWidget         *deck;
  GtkWidget         *inner_carousel;
  GtkWidget         *outer_carousel;
  gulong             deck_handler;
  gulong             inner_handler;
  gulong             outer_handler;
  gboolean           leave_when_saved;

  GtkCssProvider    *css_provider;
  GdkMonitor        *monitor;
  gulong             monitor_handler;
  int                card_height;
};

G_DEFINE_TYPE (PhoshCameraLockscreen, phosh_camera_lockscreen, GTK_TYPE_BOX);

static guint request_serial;

static void request_camera_access (PhoshCameraLockscreen *self);
static void open_next_remote (PhoshCameraLockscreen *self);
static void set_brightness (PhoshCameraLockscreen *self, guint brightness);


static void
drop_pending_request (PhoshCameraLockscreen *self)
{
  if (self->response_timeout_id) {
    g_source_remove (self->response_timeout_id);
    self->response_timeout_id = 0;
  }

  if (self->response_id) {
    g_dbus_connection_signal_unsubscribe (self->bus, self->response_id);
    self->response_id = 0;
  }
}


static void
stop_provider (PhoshCameraLockscreen *self)
{
  if (self->poll_id) {
    g_source_remove (self->poll_id);
    self->poll_id = 0;
  }

  if (self->provider) {
    gst_device_provider_stop (self->provider);
    gst_clear_object (&self->provider);
  }

  if (self->provider_fd >= 0) {
    close (self->provider_fd);
    self->provider_fd = -1;
  }
}


static void
update_shutter_icon (PhoshCameraLockscreen *self)
{
  const char *icon = "camera-photo-symbolic";

  if (gtk_toggle_button_get_active (self->mode))
    icon = self->recording ? "media-playback-stop-symbolic" : "media-record-symbolic";

  gtk_image_set_from_icon_name (self->shutter_icon, icon, GTK_ICON_SIZE_BUTTON);
}


static void
end_capture (PhoshCameraLockscreen *self)
{
  g_atomic_int_set (&self->capture_pending, FALSE);

  if (self->snap_timeout_id) {
    g_source_remove (self->snap_timeout_id);
    self->snap_timeout_id = 0;
  }

  if (self->feedback_id) {
    g_source_remove (self->feedback_id);
    self->feedback_id = 0;
  }

  update_shutter_icon (self);
  gtk_widget_set_sensitive (GTK_WIDGET (self->shutter), TRUE);
}


/*
 * The bin belongs to the pipeline once it is added, so only the requested
 * tee pad is ours to drop here.
 */
static void
detach_record_bin (PhoshCameraLockscreen *self)
{
  GstElement *record_bin = self->record_bin;
  g_autoptr (GstPad) pad = self->tee_record_pad;
  g_autoptr (GstElement) tee = NULL;

  self->record_bin = NULL;
  self->tee_record_pad = NULL;

  if (record_bin == NULL || self->pipeline == NULL)
    return;

  gst_element_set_state (record_bin, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->pipeline), record_bin);

  tee = gst_bin_get_by_name (GST_BIN (self->pipeline), "t");
  if (pad && tee)
    gst_element_release_request_pad (tee, pad);
}


static void
show_recording (PhoshCameraLockscreen *self, gboolean recording)
{
  GtkStyleContext *style = gtk_widget_get_style_context (GTK_WIDGET (self->shutter));

  self->recording = recording;
  gtk_widget_set_visible (self->recording_badge, recording);
  gtk_widget_set_sensitive (GTK_WIDGET (self->camera_switch), !recording);
  gtk_widget_set_sensitive (GTK_WIDGET (self->mode), !recording);

  if (recording)
    gtk_style_context_add_class (style, "recording");
  else
    gtk_style_context_remove_class (style, "recording");

  update_shutter_icon (self);
}


static void
end_recording (PhoshCameraLockscreen *self)
{
  if (self->timer_id) {
    g_source_remove (self->timer_id);
    self->timer_id = 0;
  }

  if (self->eos_timeout_id) {
    g_source_remove (self->eos_timeout_id);
    self->eos_timeout_id = 0;
  }

  detach_record_bin (self);
  show_recording (self, FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->shutter), TRUE);
}


static void
tear_down_pipeline (PhoshCameraLockscreen *self)
{
  end_capture (self);
  end_recording (self);

  if (self->watch_id) {
    g_source_remove (self->watch_id);
    self->watch_id = 0;
  }

  if (self->pipeline) {
    gst_element_set_state (self->pipeline, GST_STATE_NULL);
    gst_element_get_state (self->pipeline, NULL, NULL, PIPELINE_DRAIN_TIMEOUT_NS);
    g_clear_object (&self->pipeline);
  }

  if (self->portal_fd >= 0) {
    close (self->portal_fd);
    self->portal_fd = -1;
  }

  g_mutex_lock (&self->frame_lock);
  g_clear_pointer (&self->frame, gst_sample_unref);
  g_mutex_unlock (&self->frame_lock);
  gtk_widget_queue_draw (GTK_WIDGET (self->preview));
}


/*
 * The torch is owned by a daemon, so it outlives this widget: a card that
 * goes away with the light lit leaves the phone shining in a pocket. Every
 * exit runs through here.
 */
static void
put_torch_out (PhoshCameraLockscreen *self)
{
  gtk_toggle_button_set_active (self->light, FALSE);
  set_brightness (self, 0);
}


static void
stop_camera (PhoshCameraLockscreen *self)
{
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  self->cancel = g_cancellable_new ();

  if (self->idle_id) {
    g_source_remove (self->idle_id);
    self->idle_id = 0;
  }

  drop_pending_request (self);
  stop_provider (self);
  put_torch_out (self);
  tear_down_pipeline (self);
}


static void
show_idle (PhoshCameraLockscreen *self)
{
  stop_camera (self);
  gtk_stack_set_visible_child_name (self->stack, "idle");
}


static gboolean
on_idle_timeout (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  if (self->recording)
    return G_SOURCE_CONTINUE;

  self->idle_id = 0;
  show_idle (self);

  return G_SOURCE_REMOVE;
}


/*
 * Nothing tells a lock screen widget that the display went dark, so an
 * untouched viewfinder puts itself away rather than running on in a pocket.
 */
static void
bump_idle_timeout (PhoshCameraLockscreen *self)
{
  if (self->idle_id)
    g_source_remove (self->idle_id);

  self->idle_id = g_timeout_add_seconds (IDLE_TIMEOUT_SECONDS, on_idle_timeout, self);
}


static void
show_error (PhoshCameraLockscreen *self, const char *reason)
{
  g_warning ("Camera unavailable: %s", reason);
  stop_camera (self);
  gtk_stack_set_visible_child_name (self->stack, "error");
}


static void
on_brightness_set (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GError) err = NULL;

  (void) user_data;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (reply == NULL)
    g_warning ("Torch brightness refused: %s", err->message);
}


static void
set_brightness (PhoshCameraLockscreen *self, guint brightness)
{
  if (self->bus == NULL)
    return;

  g_dbus_connection_call (self->bus, FLASHLIGHT_BUS, FLASHLIGHT_PATH, FLASHLIGHT_IFACE,
                          "SetBrightness", g_variant_new ("(u)", brightness),
                          NULL, G_DBUS_CALL_FLAGS_NONE, FLASHLIGHT_CALL_TIMEOUT_MS,
                          NULL, on_brightness_set, NULL);
}


static void
on_max_brightness_read (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) value = NULL;
  g_autoptr (GError) err = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (reply == NULL) {
    g_warning ("Torch ceiling unreadable: %s", err->message);
    gtk_toggle_button_set_active (self->light, FALSE);
    return;
  }

  g_variant_get (reply, "(v)", &value);
  self->max_brightness = g_variant_get_uint32 (value);

  if (gtk_toggle_button_get_active (self->light))
    set_brightness (self, self->max_brightness);
}


static void
on_light_toggled (PhoshCameraLockscreen *self)
{
  if (self->bus == NULL)
    return;

  bump_idle_timeout (self);

  if (!gtk_toggle_button_get_active (self->light)) {
    set_brightness (self, 0);
    return;
  }

  if (self->max_brightness) {
    set_brightness (self, self->max_brightness);
    return;
  }

  g_dbus_connection_call (self->bus, FLASHLIGHT_BUS, FLASHLIGHT_PATH, PROPERTIES_IFACE,
                          "Get", g_variant_new ("(ss)", FLASHLIGHT_IFACE, "MaxBrightness"),
                          G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE,
                          FLASHLIGHT_CALL_TIMEOUT_MS, self->cancel,
                          on_max_brightness_read, g_object_ref (self));
}



static gboolean
queue_preview_draw (gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;

  gtk_widget_queue_draw (GTK_WIDGET (self->preview));

  return G_SOURCE_REMOVE;
}


/* Runs on the streaming thread; the draw happens on the main loop. */
static GstFlowReturn
on_preview_sample (GstElement *sink, gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  GstSample *sample = NULL;

  g_signal_emit_by_name (sink, "pull-sample", &sample);
  if (sample == NULL)
    return GST_FLOW_ERROR;

  g_mutex_lock (&self->frame_lock);
  g_clear_pointer (&self->frame, gst_sample_unref);
  self->frame = sample;
  g_mutex_unlock (&self->frame_lock);

  g_idle_add (queue_preview_draw, g_object_ref (self));

  return GST_FLOW_OK;
}


/*
 * Painting the frame here rather than handing the sink its own widget is what
 * makes the preview itself the rounded thing: a video widget owns its window,
 * paints a square letterbox and survives any clip from outside it.
 */
static gboolean
on_preview_draw (PhoshCameraLockscreen *self, cairo_t *cr)
{
  double width = gtk_widget_get_allocated_width (GTK_WIDGET (self->preview));
  double height = gtk_widget_get_allocated_height (GTK_WIDGET (self->preview));
  double radius = VIEWFINDER_CORNER_RADIUS;
  cairo_surface_t *surface;
  GstVideoFrame video_frame;
  GstVideoInfo info;
  GstCaps *caps;
  double scale, frame_width, frame_height, x, y, aspect, gap = 0;

  g_mutex_lock (&self->frame_lock);

  if (self->frame == NULL) {
    g_mutex_unlock (&self->frame_lock);
    return GDK_EVENT_PROPAGATE;
  }

  caps = gst_sample_get_caps (self->frame);
  if (!gst_video_info_from_caps (&info, caps) ||
      !gst_video_frame_map (&video_frame, &info, gst_sample_get_buffer (self->frame),
                            GST_MAP_READ)) {
    g_mutex_unlock (&self->frame_lock);
    return GDK_EVENT_PROPAGATE;
  }

  /*
   * Fitting the frame leaves all the slack on one axis, which reads as a
   * preview that is too narrow for its card. Solve for the inset that makes
   * the gap the same on all four sides instead.
   */
  aspect = (double) GST_VIDEO_FRAME_WIDTH (&video_frame) /
           GST_VIDEO_FRAME_HEIGHT (&video_frame);
  if (ABS (1.0 - aspect) > ASPECT_EPSILON)
    gap = CLAMP ((width - aspect * height) / (2.0 - 2.0 * aspect), 0.0,
                 MIN (width, height) / 2.0);

  scale = MIN ((width - 2 * gap) / GST_VIDEO_FRAME_WIDTH (&video_frame),
               (height - 2 * gap) / GST_VIDEO_FRAME_HEIGHT (&video_frame));
  frame_width = GST_VIDEO_FRAME_WIDTH (&video_frame) * scale;
  frame_height = GST_VIDEO_FRAME_HEIGHT (&video_frame) * scale;
  x = (width - frame_width) / 2;
  y = (height - frame_height) / 2;

  cairo_new_path (cr);
  cairo_arc (cr, x + frame_width - radius, y + radius, radius, -G_PI_2, 0);
  cairo_arc (cr, x + frame_width - radius, y + frame_height - radius, radius, 0, G_PI_2);
  cairo_arc (cr, x + radius, y + frame_height - radius, radius, G_PI_2, G_PI);
  cairo_arc (cr, x + radius, y + radius, radius, G_PI, 3 * G_PI_2);
  cairo_close_path (cr);
  cairo_clip (cr);

  surface = cairo_image_surface_create_for_data (GST_VIDEO_FRAME_PLANE_DATA (&video_frame, 0),
                                                 CAIRO_FORMAT_RGB24,
                                                 GST_VIDEO_FRAME_WIDTH (&video_frame),
                                                 GST_VIDEO_FRAME_HEIGHT (&video_frame),
                                                 GST_VIDEO_FRAME_PLANE_STRIDE (&video_frame, 0));
  cairo_translate (cr, x, y);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  cairo_surface_destroy (surface);

  gst_video_frame_unmap (&video_frame);
  g_mutex_unlock (&self->frame_lock);

  return GDK_EVENT_PROPAGATE;
}


static gboolean
on_capture_shown (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  self->feedback_id = 0;
  end_capture (self);

  return G_SOURCE_REMOVE;
}


static gboolean
on_capture_saved (gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;

  if (self->snap_timeout_id) {
    g_source_remove (self->snap_timeout_id);
    self->snap_timeout_id = 0;
  }

  gtk_image_set_from_icon_name (self->shutter_icon, "object-select-symbolic", GTK_ICON_SIZE_BUTTON);
  self->feedback_id = g_timeout_add (CAPTURE_FEEDBACK_MS, on_capture_shown, self);

  return G_SOURCE_REMOVE;
}


static void
close_capture_valve (PhoshCameraLockscreen *self)
{
  g_autoptr (GstElement) valve = NULL;

  if (self->pipeline == NULL)
    return;

  valve = gst_bin_get_by_name (GST_BIN (self->pipeline), "snap");
  g_object_set (valve, "drop", TRUE, NULL);
}


/*
 * Runs on the streaming thread: the first frame through the valve wins the
 * capture and shuts the valve behind itself, so the viewfinder never stalls
 * on a second encode.
 */
static GstFlowReturn
on_snap_sample (GstElement *sink, gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  g_autoptr (GstSample) sample = NULL;
  g_autoptr (GDateTime) now = NULL;
  g_autoptr (GError) err = NULL;
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  GstBuffer *buffer;
  GstMapInfo info;

  g_signal_emit_by_name (sink, "pull-sample", &sample);
  if (sample == NULL)
    return GST_FLOW_ERROR;

  if (!g_atomic_int_compare_and_exchange (&self->capture_pending, TRUE, FALSE))
    return GST_FLOW_OK;

  close_capture_valve (self);

  buffer = gst_sample_get_buffer (sample);
  if (!gst_buffer_map (buffer, &info, GST_MAP_READ)) {
    g_warning ("Capture buffer could not be read");
    return GST_FLOW_OK;
  }

  now = g_date_time_new_now_local ();
  name = g_date_time_format (now, CAPTURE_NAME_FORMAT);
  path = g_build_filename (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES), name, NULL);

  if (g_file_set_contents (path, (const char *) info.data, info.size, &err))
    g_idle_add (on_capture_saved, g_object_ref (self));
  else
    g_warning ("Capture could not be saved: %s", err->message);

  gst_buffer_unmap (buffer, &info);

  return GST_FLOW_OK;
}


static gboolean
on_snap_timed_out (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  self->snap_timeout_id = 0;
  close_capture_valve (self);
  end_capture (self);
  g_warning ("Capture produced no frame");

  return G_SOURCE_REMOVE;
}


static void
take_picture (PhoshCameraLockscreen *self)
{
  g_autoptr (GstElement) valve = NULL;

  if (!g_atomic_int_compare_and_exchange (&self->capture_pending, FALSE, TRUE))
    return;

  gtk_widget_set_sensitive (GTK_WIDGET (self->shutter), FALSE);
  self->snap_timeout_id = g_timeout_add (SNAP_TIMEOUT_MS, on_snap_timed_out, self);

  valve = gst_bin_get_by_name (GST_BIN (self->pipeline), "snap");
  g_object_set (valve, "drop", FALSE, NULL);
}


static gboolean
on_timer_tick (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  gint64 elapsed = (g_get_monotonic_time () - self->record_start) / G_USEC_PER_SEC;
  g_autofree char *text = NULL;

  text = g_strdup_printf ("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, elapsed / 60, elapsed % 60);
  gtk_label_set_label (self->timer_label, text);

  return G_SOURCE_CONTINUE;
}


/*
 * The take taps the tee the viewfinder is already running on, so the file
 * starts on a frame the screen has shown: a pipeline rebuilt for recording
 * reopens the sensor and spends its first second ramping exposure, which
 * opens every file with black.
 */
static void
start_recording (PhoshCameraLockscreen *self)
{
  g_autoptr (GstElement) tee = NULL;
  g_autoptr (GstPad) sink_pad = NULL;
  g_autoptr (GDateTime) now = NULL;
  g_autoptr (GError) err = NULL;
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  g_autofree char *desc = NULL;

  now = g_date_time_new_now_local ();
  name = g_date_time_format (now, RECORD_NAME_FORMAT);
  path = g_build_filename (g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS), name, NULL);

  desc = g_strdup_printf ("queue name=record_queue max-size-bytes=0 max-size-time=2000000000 ! "
                          "videorate skip-to-first=true ! video/x-raw,framerate=30/1 ! "
                          "videoconvert ! videoflip video-direction=auto ! "
                          "jpegenc quality=%d ! matroskamux name=mux offset-to-zero=true ! "
                          "filesink name=record_sink location=%s "
                          "autoaudiosrc ! queue name=audio_queue ! audioconvert ! "
                          "avenc_aac ! mux.",
                          RECORD_JPEG_QUALITY, path);

  self->record_bin = gst_parse_bin_from_description (desc, TRUE, &err);
  if (self->record_bin == NULL) {
    g_warning ("Recording branch could not be built: %s", err->message);
    return;
  }

  self->eos_sent = FALSE;
  gst_bin_add (GST_BIN (self->pipeline), self->record_bin);
  gst_element_sync_state_with_parent (self->record_bin);

  tee = gst_bin_get_by_name (GST_BIN (self->pipeline), "t");
  self->tee_record_pad = gst_element_request_pad_simple (tee, "src_%u");
  sink_pad = gst_element_get_static_pad (self->record_bin, "sink");

  if (gst_pad_link (self->tee_record_pad, sink_pad) != GST_PAD_LINK_OK) {
    g_warning ("Recording branch could not be linked");
    detach_record_bin (self);
    return;
  }

  self->record_start = g_get_monotonic_time ();
  gtk_label_set_label (self->timer_label, "0:00");
  self->timer_id = g_timeout_add (RECORD_TIMER_INTERVAL_MS, on_timer_tick, self);
  show_recording (self, TRUE);
}


static gboolean
finish_take (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  end_recording (self);

  if (self->leave_when_saved) {
    self->leave_when_saved = FALSE;
    show_idle (self);
  }

  return G_SOURCE_REMOVE;
}


static gboolean
on_eos_timed_out (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  self->eos_timeout_id = 0;
  g_warning ("Recording EOS never landed, closing the file anyway");
  end_recording (self);

  return G_SOURCE_REMOVE;
}


static GstPadProbeReturn
on_record_sink_event (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  (void) pad;

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS)
    g_idle_add (finish_take, user_data);

  return GST_PAD_PROBE_OK;
}


static GstPadProbeReturn
on_tee_blocked (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  const char *queues[] = {"record_queue", "audio_queue"};

  (void) pad;
  (void) info;

  if (self->eos_sent || self->record_bin == NULL)
    return GST_PAD_PROBE_OK;

  self->eos_sent = TRUE;

  for (guint i = 0; i < G_N_ELEMENTS (queues); i++) {
    g_autoptr (GstElement) queue = gst_bin_get_by_name (GST_BIN (self->record_bin), queues[i]);
    g_autoptr (GstPad) queue_pad = NULL;

    if (queue == NULL)
      continue;

    queue_pad = gst_element_get_static_pad (queue, "sink");
    gst_pad_send_event (queue_pad, gst_event_new_eos ());
  }

  return GST_PAD_PROBE_OK;
}


/*
 * The tee pad is blocked before the branch is drained: a branch answering
 * EOS to further buffers poisons the tee's combined flow and takes the
 * viewfinder down with it. The finished file is spotted from a pad probe
 * because a sink inside a still-playing pipeline posts no bus message.
 */
static void
stop_recording (PhoshCameraLockscreen *self)
{
  g_autoptr (GstElement) record_sink = NULL;
  g_autoptr (GstPad) sink_pad = NULL;

  gtk_widget_set_sensitive (GTK_WIDGET (self->shutter), FALSE);

  if (self->timer_id) {
    g_source_remove (self->timer_id);
    self->timer_id = 0;
  }

  if (self->record_bin == NULL || self->pipeline == NULL) {
    end_recording (self);
    return;
  }

  record_sink = gst_bin_get_by_name (GST_BIN (self->record_bin), "record_sink");
  sink_pad = gst_element_get_static_pad (record_sink, "sink");
  gst_pad_add_probe (sink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     on_record_sink_event, self, NULL);
  gst_pad_add_probe (self->tee_record_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                     on_tee_blocked, self, NULL);
  self->eos_timeout_id = g_timeout_add (EOS_TIMEOUT_MS, on_eos_timed_out, self);
}


static void
on_shutter_clicked (PhoshCameraLockscreen *self)
{
  if (self->pipeline == NULL)
    return;

  bump_idle_timeout (self);

  if (!gtk_toggle_button_get_active (self->mode)) {
    take_picture (self);
    return;
  }

  if (self->recording)
    stop_recording (self);
  else
    start_recording (self);
}


static void
on_mode_toggled (PhoshCameraLockscreen *self)
{
  GtkStyleContext *style = gtk_widget_get_style_context (GTK_WIDGET (self->shutter));
  gboolean video = gtk_toggle_button_get_active (self->mode);

  bump_idle_timeout (self);
  gtk_style_context_remove_class (style, video ? "shutter-button" : "record-button");
  gtk_style_context_add_class (style, video ? "record-button" : "shutter-button");
  update_shutter_icon (self);
}


/*
 * Only the back camera has a light next to it, so the front one turns the
 * torch out and greys the toggle rather than pretending.
 */
static void
on_camera_switch_clicked (PhoshCameraLockscreen *self)
{
  bump_idle_timeout (self);
  self->camera = self->camera == CAMERA_BACK ? CAMERA_FRONT : CAMERA_BACK;

  if (self->camera != CAMERA_BACK)
    put_torch_out (self);
  gtk_widget_set_sensitive (GTK_WIDGET (self->light), self->camera == CAMERA_BACK);

  gtk_widget_set_sensitive (GTK_WIDGET (self->camera_switch), FALSE);
  tear_down_pipeline (self);
  open_next_remote (self);
}


static gboolean
on_pipeline_message (GstBus *bus, GstMessage *message, gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  g_autoptr (GError) err = NULL;
  g_autofree char *debug = NULL;
  GstState state;

  (void) bus;

  switch (GST_MESSAGE_TYPE (message)) {
  case GST_MESSAGE_ERROR:
    gst_message_parse_error (message, &err, &debug);
    g_warning ("Viewfinder error: %s (%s)", err->message, debug ?: "");
    show_error (self, err->message);
    return G_SOURCE_REMOVE;
  case GST_MESSAGE_EOS:
    show_idle (self);
    return G_SOURCE_REMOVE;
  case GST_MESSAGE_STATE_CHANGED:
    if (GST_MESSAGE_SRC (message) != GST_OBJECT (self->pipeline))
      return G_SOURCE_CONTINUE;
    gst_message_parse_state_changed (message, NULL, &state, NULL);
    if (state == GST_STATE_PLAYING)
      gtk_widget_set_sensitive (GTK_WIDGET (self->camera_switch), TRUE);
    return G_SOURCE_CONTINUE;
  default:
    return G_SOURCE_CONTINUE;
  }
}


/*
 * The still is a second branch of the viewfinder behind a shut valve rather
 * than a pipeline of its own: opening the valve for one frame costs nothing
 * while the camera keeps running, where tearing the viewfinder down to open
 * a capture pipeline costs seconds and a visible blink. The branch is async
 * so the graph does not wait on a preroll buffer the shut valve will never
 * deliver.
 */
static void
start_pipeline (PhoshCameraLockscreen *self, int fd)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GstElement) sink = NULL;
  g_autoptr (GstElement) photosink = NULL;
  g_autoptr (GstBus) bus = NULL;
  g_autofree char *desc = NULL;

  desc = g_strdup_printf ("pipewiresrc name=src fd=%d target-object=%d ! tee name=t "
                          "t. ! queue leaky=downstream max-size-buffers=2 ! "
                          "videorate drop-only=true ! video/x-raw,framerate=%d/1 ! "
                          "videoscale ! video/x-raw,width=%d,pixel-aspect-ratio=1/1 ! "
                          "videoconvert ! videoflip video-direction=auto ! "
                          "video/x-raw,format=BGRx ! "
                          "appsink name=sink emit-signals=true max-buffers=1 "
                          "drop=true sync=false "
                          "t. ! valve name=snap drop=true ! queue ! videoconvert ! "
                          "videoflip video-direction=auto ! jpegenc ! "
                          "appsink name=photosink emit-signals=true max-buffers=1 "
                          "drop=true async=false sync=false",
                          fd, self->target[self->camera], VIEWFINDER_FRAMERATE,
                          VIEWFINDER_WIDTH);

  /* pipewiresrc dups the remote, so the portal's fd stays ours to close. */
  self->portal_fd = fd;

  self->pipeline = gst_parse_launch (desc, &err);
  if (self->pipeline == NULL) {
    show_error (self, err->message);
    return;
  }

  sink = gst_bin_get_by_name (GST_BIN (self->pipeline), "sink");
  g_signal_connect (sink, "new-sample", G_CALLBACK (on_preview_sample), self);

  photosink = gst_bin_get_by_name (GST_BIN (self->pipeline), "photosink");
  g_signal_connect (photosink, "new-sample", G_CALLBACK (on_snap_sample), self);

  bus = gst_element_get_bus (self->pipeline);
  self->watch_id = gst_bus_add_watch (bus, on_pipeline_message, self);

  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  gtk_stack_set_visible_child_name (self->stack, "camera");
  bump_idle_timeout (self);
}


static void
read_camera_targets (PhoshCameraLockscreen *self, GList *devices)
{
  int index = 0;

  for (GList *elem = devices; elem; elem = elem->next, index++) {
    g_autoptr (GstStructure) props = gst_device_get_properties (GST_DEVICE (elem->data));
    const char *serial, *location;
    int target;

    if (props == NULL)
      continue;

    serial = gst_structure_get_string (props, "object.serial");
    location = gst_structure_get_string (props, "api.libcamera.location");
    target = serial ? (int) g_ascii_strtoll (serial, NULL, 10) : index;

    if (g_strcmp0 (location, "front") == 0) {
      if (self->target[CAMERA_FRONT] < 0)
        self->target[CAMERA_FRONT] = target;
    } else if (self->target[CAMERA_BACK] < 0) {
      self->target[CAMERA_BACK] = target;
    }
  }

  if (self->target[CAMERA_BACK] < 0)
    self->target[CAMERA_BACK] = CAMERA_BACK;

  gtk_widget_set_visible (GTK_WIDGET (self->camera_switch), self->target[CAMERA_FRONT] >= 0);
}


static gboolean
on_devices_polled (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  GList *devices = gst_device_provider_get_devices (self->provider);

  if (devices == NULL) {
    if (g_get_monotonic_time () < self->poll_deadline)
      return G_SOURCE_CONTINUE;
    self->poll_id = 0;
    show_error (self, "no camera nodes on the portal remote");
    return G_SOURCE_REMOVE;
  }

  self->poll_id = 0;
  read_camera_targets (self, devices);
  g_list_free_full (devices, gst_object_unref);
  stop_provider (self);
  open_next_remote (self);

  return G_SOURCE_REMOVE;
}


/*
 * The provider populates asynchronously with no completion signal worth the
 * name, so a short poll stands in; cameras that exist show up within the
 * first few ticks.
 */
static void
start_provider (PhoshCameraLockscreen *self, int fd)
{
  g_autoptr (GstDeviceProviderFactory) factory = NULL;

  factory = gst_device_provider_factory_find ("pipewiredeviceprovider");
  if (factory == NULL) {
    close (fd);
    show_error (self, "pipewiredeviceprovider is missing");
    return;
  }

  self->provider = gst_device_provider_factory_get (factory);
  self->provider_fd = fd;
  g_object_set (self->provider, "fd", fd, NULL);
  gst_device_provider_start (self->provider);

  self->poll_deadline = g_get_monotonic_time () + DEVICE_POLL_TIMEOUT_MS * 1000;
  self->poll_id = g_timeout_add (DEVICE_POLL_INTERVAL_MS, on_devices_polled, self);
}


static int
take_remote_fd (PhoshCameraLockscreen *self, GObject *source, GAsyncResult *res)
{
  g_autoptr (GUnixFDList) fds = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GError) err = NULL;
  int handle, fd;

  reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source),
                                                           &fds, res, &err);
  if (reply == NULL) {
    if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      show_error (self, err->message);
    return -1;
  }

  g_variant_get (reply, "(h)", &handle);
  fd = g_unix_fd_list_get (fds, handle, &err);
  if (fd < 0)
    show_error (self, err->message);

  return fd;
}


static void
on_provider_remote_opened (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;
  int fd = take_remote_fd (self, source, res);

  if (fd < 0)
    return;

  start_provider (self, fd);
}


static void
on_pipeline_remote_opened (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;
  int fd = take_remote_fd (self, source, res);

  if (fd < 0)
    return;

  start_pipeline (self, fd);
}


/*
 * A PipeWire remote is one client's conversation, so the enumerator and the
 * pipeline each get their own from the portal: a dup of the first is the same
 * conversation twice, and the second speaker's node choice is ignored.
 */
static void
open_next_remote (PhoshCameraLockscreen *self)
{
  GAsyncReadyCallback callback;
  GVariantBuilder options;

  callback = self->target[CAMERA_BACK] < 0 ? on_provider_remote_opened
                                           : on_pipeline_remote_opened;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call_with_unix_fd_list (self->bus, PORTAL_BUS, PORTAL_PATH,
                                            PORTAL_CAMERA_IFACE, "OpenPipeWireRemote",
                                            g_variant_new ("(a{sv})", &options),
                                            G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE,
                                            PORTAL_CALL_TIMEOUT_MS, NULL, self->cancel,
                                            callback, g_object_ref (self));
}


static void
on_access_response (GDBusConnection *connection,
                    const char      *sender_name,
                    const char      *object_path,
                    const char      *interface_name,
                    const char      *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);
  g_autoptr (GVariant) results = NULL;
  guint32 response;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  drop_pending_request (self);

  g_variant_get (parameters, "(u@a{sv})", &response, &results);
  if (response != 0) {
    show_error (self, "access request refused");
    return;
  }

  self->access_granted = TRUE;
  open_next_remote (self);
}


static gboolean
on_access_timed_out (gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  self->response_timeout_id = 0;
  show_error (self, "no response to the access request");

  return G_SOURCE_REMOVE;
}


static void
on_access_called (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GError) err = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);
  if (reply == NULL && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    show_error (self, err->message);
}


static void
on_bus_got (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (PhoshCameraLockscreen) self = user_data;
  g_autoptr (GError) err = NULL;

  (void) source;

  self->bus = g_bus_get_finish (res, &err);
  if (self->bus == NULL) {
    if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      show_error (self, err->message);
    return;
  }

  request_camera_access (self);
}


/*
 * A portal request answers on a request object whose path is derivable up
 * front, so the subscription has to exist before the call: a portal that
 * answers immediately would otherwise answer into nobody.
 */
static void
request_camera_access (PhoshCameraLockscreen *self)
{
  g_autofree char *token = NULL;
  g_autofree char *sender = NULL;
  g_autofree char *request_path = NULL;
  GVariantBuilder options;

  token = g_strdup_printf ("phosh_camera_lockscreen_%d_%u", getpid (), ++request_serial);
  sender = g_strdup (g_dbus_connection_get_unique_name (self->bus) + 1);
  g_strdelimit (sender, ".", '_');
  request_path = g_strdup_printf (PORTAL_PATH "/request/%s/%s", sender, token);

  self->response_id = g_dbus_connection_signal_subscribe (self->bus, PORTAL_BUS,
                                                          PORTAL_REQUEST_IFACE, "Response",
                                                          request_path, NULL,
                                                          G_DBUS_SIGNAL_FLAGS_NONE,
                                                          on_access_response, self, NULL);
  self->response_timeout_id = g_timeout_add (ACCESS_RESPONSE_TIMEOUT_MS,
                                             on_access_timed_out, self);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token", g_variant_new_string (token));
  g_dbus_connection_call (self->bus, PORTAL_BUS, PORTAL_PATH, PORTAL_CAMERA_IFACE,
                          "AccessCamera", g_variant_new ("(a{sv})", &options),
                          G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE,
                          PORTAL_CALL_TIMEOUT_MS, self->cancel,
                          on_access_called, g_object_ref (self));
}


static void
on_start_clicked (PhoshCameraLockscreen *self)
{
  g_autoptr (GError) err = NULL;

  if (self->pipeline)
    return;

  gtk_stack_set_visible_child_name (self->stack, "starting");

  if (!gst_init_check (NULL, NULL, &err)) {
    show_error (self, err->message);
    return;
  }

  if (self->bus == NULL) {
    g_bus_get (G_BUS_TYPE_SESSION, self->cancel, on_bus_got, g_object_ref (self));
    return;
  }

  if (!self->access_granted) {
    request_camera_access (self);
    return;
  }

  open_next_remote (self);
}


/*
 * The card keeps one size whatever it is showing, so it stays the shape of
 * every other lock screen widget and the preview scales into it. The height
 * is a share of the monitor rather than a number, since a figure that suits
 * this screen is taller than a shorter one has to give.
 *
 * It is worked out on realize and kept, never asked for while measuring: an
 * unrealized widget has no window to find a monitor through, Wayland has no
 * primary monitor to fall back on, and a size request that answered with the
 * fallback would be cached as the card's size until something else happened
 * to invalidate it.
 */
static void
update_card_height (PhoshCameraLockscreen *self)
{
  GtkWidget *widget = GTK_WIDGET (self);
  GdkWindow *window = gtk_widget_get_window (widget);
  GdkRectangle area;
  int height = CARD_HEIGHT_FALLBACK;

  if (window)
    self->monitor = gdk_display_get_monitor_at_window (gtk_widget_get_display (widget), window);

  if (self->monitor) {
    gdk_monitor_get_workarea (self->monitor, &area);
    height = area.height * CARD_HEIGHT_FRACTION;
  }

  if (height == self->card_height)
    return;

  self->card_height = height;
  gtk_widget_queue_resize (widget);
}


static void
on_workarea_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void) object;
  (void) pspec;

  update_card_height (PHOSH_CAMERA_LOCKSCREEN (user_data));
}


static void
phosh_camera_lockscreen_realize (GtkWidget *widget)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (widget);

  GTK_WIDGET_CLASS (phosh_camera_lockscreen_parent_class)->realize (widget);

  update_card_height (self);

  if (self->monitor && self->monitor_handler == 0)
    self->monitor_handler = g_signal_connect (self->monitor, "notify::workarea",
                                              G_CALLBACK (on_workarea_changed), self);
}


static void
phosh_camera_lockscreen_unrealize (GtkWidget *widget)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (widget);

  g_clear_signal_handler (&self->monitor_handler, self->monitor);
  self->monitor = NULL;

  GTK_WIDGET_CLASS (phosh_camera_lockscreen_parent_class)->unrealize (widget);
}


static void
phosh_camera_lockscreen_get_preferred_width (GtkWidget *widget, int *minimum, int *natural)
{
  GTK_WIDGET_CLASS (phosh_camera_lockscreen_parent_class)->get_preferred_width (widget,
                                                                                minimum,
                                                                                natural);
  *minimum = MIN (*minimum, CARD_WIDTH_REQUEST);
  *natural = MIN (*natural, CARD_WIDTH_REQUEST);
}


static void
phosh_camera_lockscreen_get_preferred_height (GtkWidget *widget, int *minimum, int *natural)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (widget);

  GTK_WIDGET_CLASS (phosh_camera_lockscreen_parent_class)->get_preferred_height (widget,
                                                                                 minimum,
                                                                                 natural);
  *minimum = MIN (*minimum, self->card_height);
  *natural = self->card_height;
}



static GtkWidget *
find_ancestor_by_type_name (GtkWidget *widget, const char *type_name)
{
  for (GtkWidget *parent = gtk_widget_get_parent (widget); parent;
       parent = gtk_widget_get_parent (parent)) {
    if (g_strcmp0 (G_OBJECT_TYPE_NAME (parent), type_name) == 0)
      return parent;
  }

  return NULL;
}


static gboolean
is_page_showing (GtkWidget *carousel, GtkWidget *widget)
{
  g_autoptr (GList) children = NULL;
  GtkWidget *page = NULL;
  double position;
  int index;

  for (GtkWidget *parent = widget; parent; parent = gtk_widget_get_parent (parent)) {
    if (gtk_widget_get_parent (parent) == carousel) {
      page = parent;
      break;
    }
  }

  if (page == NULL)
    return TRUE;

  children = gtk_container_get_children (GTK_CONTAINER (carousel));
  index = g_list_index (children, page);
  if (index < 0)
    return TRUE;

  g_object_get (carousel, "position", &position, NULL);

  return ABS (position - index) < 0.5;
}


/*
 * Nothing unmaps the card when the lock screen swipes to another widget, the
 * clock or the keypad, so the camera would go on running behind whatever the
 * user moved to. The deck and both carousels are asked directly rather than
 * through libphosh, which exposes none of this.
 */
static gboolean
is_card_on_screen (PhoshCameraLockscreen *self)
{
  if (self->deck) {
    g_autoptr (GtkWidget) visible = NULL;

    g_object_get (self->deck, "visible-child", &visible, NULL);
    if (visible == NULL || !gtk_widget_is_ancestor (GTK_WIDGET (self), visible))
      return FALSE;
  }

  if (self->inner_carousel && !is_page_showing (self->inner_carousel, GTK_WIDGET (self)))
    return FALSE;

  if (self->outer_carousel && self->deck &&
      !is_page_showing (self->outer_carousel, self->deck))
    return FALSE;

  return TRUE;
}


/*
 * A take in progress is finished rather than dropped: the file is drained to
 * its end first and the camera goes away when the sink reports it written.
 */
static void
leave_camera (PhoshCameraLockscreen *self)
{
  if (self->pipeline == NULL)
    return;

  if (self->recording) {
    self->leave_when_saved = TRUE;
    stop_recording (self);
    return;
  }

  show_idle (self);
}


static void
on_navigated (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (user_data);

  (void) object;
  (void) pspec;

  if (!is_card_on_screen (self))
    leave_camera (self);
}


static void
watch_navigation (PhoshCameraLockscreen *self)
{
  self->deck = find_ancestor_by_type_name (GTK_WIDGET (self), "HdyDeck");
  self->inner_carousel = find_ancestor_by_type_name (GTK_WIDGET (self), "HdyCarousel");
  self->outer_carousel = self->deck ? find_ancestor_by_type_name (self->deck, "HdyCarousel")
                                    : NULL;

  if (self->deck)
    self->deck_handler = g_signal_connect (self->deck, "notify::visible-child",
                                           G_CALLBACK (on_navigated), self);
  if (self->inner_carousel)
    self->inner_handler = g_signal_connect (self->inner_carousel, "notify::position",
                                            G_CALLBACK (on_navigated), self);
  if (self->outer_carousel && self->outer_carousel != self->inner_carousel)
    self->outer_handler = g_signal_connect (self->outer_carousel, "notify::position",
                                            G_CALLBACK (on_navigated), self);
}


static void
unwatch_navigation (PhoshCameraLockscreen *self)
{
  g_clear_signal_handler (&self->deck_handler, self->deck);
  g_clear_signal_handler (&self->inner_handler, self->inner_carousel);
  g_clear_signal_handler (&self->outer_handler, self->outer_carousel);
  self->deck = NULL;
  self->inner_carousel = NULL;
  self->outer_carousel = NULL;
}


static void
phosh_camera_lockscreen_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (phosh_camera_lockscreen_parent_class)->map (widget);

  watch_navigation (PHOSH_CAMERA_LOCKSCREEN (widget));
}


static void
phosh_camera_lockscreen_unmap (GtkWidget *widget)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (widget);

  unwatch_navigation (self);
  show_idle (self);

  GTK_WIDGET_CLASS (phosh_camera_lockscreen_parent_class)->unmap (widget);
}


static void
phosh_camera_lockscreen_dispose (GObject *object)
{
  PhoshCameraLockscreen *self = PHOSH_CAMERA_LOCKSCREEN (object);

  unwatch_navigation (self);
  stop_camera (self);
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  g_clear_object (&self->bus);

  if (self->css_provider) {
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                  GTK_STYLE_PROVIDER (self->css_provider));
    g_clear_object (&self->css_provider);
  }

  g_mutex_clear (&self->frame_lock);

  G_OBJECT_CLASS (phosh_camera_lockscreen_parent_class)->dispose (object);
}


static void
phosh_camera_lockscreen_class_init (PhoshCameraLockscreenClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = phosh_camera_lockscreen_dispose;
  widget_class->get_preferred_width = phosh_camera_lockscreen_get_preferred_width;
  widget_class->get_preferred_height = phosh_camera_lockscreen_get_preferred_height;
  widget_class->realize = phosh_camera_lockscreen_realize;
  widget_class->unrealize = phosh_camera_lockscreen_unrealize;
  widget_class->map = phosh_camera_lockscreen_map;
  widget_class->unmap = phosh_camera_lockscreen_unmap;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/mobi/phosh/plugins/camera-lockscreen/camera-lockscreen.ui");
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, stack);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, viewfinder);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, preview);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, shutter);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, shutter_icon);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, light);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, light_icon);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, camera_switch);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, mode);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, recording_badge);
  gtk_widget_class_bind_template_child (widget_class, PhoshCameraLockscreen, timer_label);
  gtk_widget_class_bind_template_callback (widget_class, on_start_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_shutter_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_light_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_camera_switch_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_mode_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_preview_draw);

  gtk_widget_class_set_css_name (widget_class, "phosh-camera-lockscreen");
}


static void
phosh_camera_lockscreen_init (PhoshCameraLockscreen *self)
{
  GtkIconTheme *icons;

  g_mutex_init (&self->frame_lock);
  self->card_height = CARD_HEIGHT_FALLBACK;
  self->cancel = g_cancellable_new ();
  self->portal_fd = -1;
  self->provider_fd = -1;
  self->camera = CAMERA_BACK;
  self->target[CAMERA_BACK] = -1;
  self->target[CAMERA_FRONT] = -1;

  gtk_widget_init_template (GTK_WIDGET (self));

  icons = gtk_icon_theme_get_default ();
  if (!gtk_icon_theme_has_icon (icons, "flashlight-symbolic"))
    gtk_image_set_from_icon_name (self->light_icon, "display-brightness-symbolic",
                                  GTK_ICON_SIZE_BUTTON);

  /*
   * A provider added to one style context does not reach that widget's
   * children, so the buttons and the viewfinder would go unstyled. Every
   * rule in the sheet is scoped under the widget's own css name, so the
   * screen-wide provider cannot reach anything else in the shell.
   */
  self->css_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (self->css_provider,
                                       "/mobi/phosh/plugins/camera-lockscreen/stylesheet/common.css");
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (self->css_provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

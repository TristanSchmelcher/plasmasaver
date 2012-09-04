// A daemon to detect areas of the screen that are static and display a moving
// bar in their place.
//
// Copyright (c) 2012 Tristan Schmelcher <tristan_schmelcher@alumni.uwaterloo.ca>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
// USA.

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <X11/X.h>

// Number of milliseconds for the bar to move across the screen (approximate).
static const guint PERIOD_MS = 4000;
// Number of pixels that the bar moves at once.
static const guint BAR_PIXEL_INCREMENT = 10;
// Bar's width as a fraction of the screen width.
static const double BAR_FRACTION = 3.0/8;
// How often to capture the screen.
static const guint SCREEN_CAPTURE_PERIOD_MS = 2000;
// Maximum allowed pixel age before anti-IR is engaged, in capture periods.
static const unsigned char MAX_AGE = 150;
// Period of polling the pointer position.
static const guint POINTER_POLL_PERIOD_MS = 10;
// Radius of the circle around the pointer that "cleans" the anti-IR pattern, in
// pixels.
static const double POINTER_CLEANING_RADIUS = 100;
// The amount by which a colour component must change to be considered not
// static.
static const int MIN_CHANGE_THRESHOLD = 2;

// Colour of the bar (slightly blue tint).
static const double BAR_COLOUR_R = 0.9;
static const double BAR_COLOUR_G = 0.9;
static const double BAR_COLOUR_B = 1.0;

struct data_t {
  // Initialized once in main().
  GtkWidget *window;
  GdkWindow *root;
  cairo_pattern_t *pattern;

  // Initialized dynamically upon size changes.
  gboolean initialized;
  guint draw_timeout_id;
  cairo_surface_t *mask;
  cairo_surface_t *mask_image;
  cairo_surface_t *pixel_ages;
  cairo_surface_t *captures[2];
  int width;
  int height;

  int capture_index;
  guint bar_x;
  gint pointer_x;
  gint pointer_y;
};

static void release(struct data_t *data) {
  if (!data->initialized) {
    return;
  }
  g_source_remove(data->draw_timeout_id);
  cairo_surface_destroy(data->mask);
  cairo_surface_destroy(data->mask_image);
  cairo_surface_destroy(data->pixel_ages);
  for (int i = 0; i < 2; i++) {
    cairo_surface_destroy(data->captures[i]);
  }
}

static gboolean on_draw_timer(gpointer user_data) {
  struct data_t *data = (struct data_t *)user_data;
  assert(data->width);
  data->bar_x = (data->bar_x + BAR_PIXEL_INCREMENT) % data->width;
  gtk_widget_queue_draw(data->window);
  return TRUE;
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
  struct data_t *data = (struct data_t *)user_data;

  int width = gtk_widget_get_allocated_width(widget);
  int height = gtk_widget_get_allocated_height(widget);
  assert(width);
  assert(height);

  if (!data->initialized || data->width != width || data->height != height) {
    release(data);

    // (Re-)register draw timeout.
    data->draw_timeout_id = g_timeout_add(
        PERIOD_MS * BAR_PIXEL_INCREMENT / width, &on_draw_timer, data);

    // (Re-)create mask.
    data->mask = gdk_window_create_similar_surface(
        gtk_widget_get_window(data->window), CAIRO_CONTENT_ALPHA, width,
        height);
    data->mask_image = cairo_image_surface_create(CAIRO_FORMAT_A1, width,
        height);

    // (Re-)create surface to store pixel ages.
    data->pixel_ages = cairo_image_surface_create(CAIRO_FORMAT_A8, width,
        height);

    // (Re-)create screen capture images.
    for (int i = 0; i < 2; i++) {
      data->captures[i] = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width,
        height);
    }

    // Scale x if this is a resize.
    if (data->initialized) {
      data->bar_x = data->bar_x * width / data->width;
    }

    data->width = width;
    data->height = height;
    data->initialized = TRUE;
  }

  // Copy the mask.
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, data->mask, 0.0, 0.0);
  cairo_paint(cr);

  // Draw within the masked area.
  cairo_translate(cr, data->bar_x, 0.0);
  cairo_scale(cr, width, 1.0);
  cairo_set_operator(cr, CAIRO_OPERATOR_IN);
  cairo_set_source(cr, data->pattern);
  cairo_paint(cr);

  return TRUE;
}

static void on_destroy(GtkWidget *widget, gpointer user_data) {
  struct data_t *data = (struct data_t *)user_data;
  release(data);
  data->initialized = FALSE;
  gtk_main_quit();
}

static gboolean on_screen_capture_timer(gpointer user_data) {
  struct data_t *data = (struct data_t *)user_data;
  if (!data->initialized) {
    return TRUE;
  }
  int other_capture_index = 1 - data->capture_index;
  cairo_surface_t *current_surface = data->captures[data->capture_index];
  cairo_surface_t *last_surface = data->captures[other_capture_index];
  data->capture_index = other_capture_index;

  // Capture the screen.
  cairo_t *cr = cairo_create(current_surface);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  gdk_cairo_set_source_window(cr, data->root, 0.0, 0.0);
  cairo_paint(cr);
  cairo_destroy(cr);

  // Compute mask by diff'ing to previous capture.
  cairo_surface_flush(data->mask_image);
  cairo_surface_flush(data->pixel_ages);
  cairo_surface_flush(current_surface);
  cairo_surface_flush(last_surface);
  unsigned char *mask = cairo_image_surface_get_data(data->mask_image);
  unsigned char *pixel_ages = cairo_image_surface_get_data(data->pixel_ages);
  unsigned char *current = cairo_image_surface_get_data(current_surface);
  unsigned char *last = cairo_image_surface_get_data(last_surface);
  int mask_stride = cairo_image_surface_get_stride(data->mask_image);
  int pixel_ages_stride = cairo_image_surface_get_stride(data->pixel_ages);
  int current_stride = cairo_image_surface_get_stride(current_surface);
  int last_stride = cairo_image_surface_get_stride(last_surface);
  gboolean mask_changed = FALSE;
  for (int y = 0; y < data->height; y++) {
    for (int x = 0; x < data->width; x++) {
      int byte = x / 8;
      int bit = 1 << (x % 8);
      if (mask[byte] & bit) {
        // Already masked out.
        continue;
      }
      unsigned char age = pixel_ages[x];
      uint32_t current_pix = ((uint32_t *)current)[x];
      uint32_t last_pix = ((uint32_t *)last)[x];
      gboolean different = FALSE;
      for (int i = 0; i < 3; i++) {
        int diff = ((int)(current_pix & 0xFF)) - (int)(last_pix & 0xFF);
        current_pix >>= 8;
        last_pix >>= 8;
        if (abs(diff) >= MIN_CHANGE_THRESHOLD) {
          different = TRUE;
        }
      }
      if (!different) {
        if (++age >= MAX_AGE) {
          mask[byte] = mask[byte] | bit;
          age = 0;
          mask_changed = TRUE;
        }
      } else {
        age = 0;
      }
      pixel_ages[x] = age;
    }
    mask += mask_stride;
    pixel_ages += pixel_ages_stride;
    current += current_stride;
    last += last_stride;
  }
  cairo_surface_mark_dirty(data->pixel_ages);

  if (mask_changed) {
    // Upload the modified mask.
    cairo_surface_mark_dirty(data->mask_image);
    cr = cairo_create(data->mask);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, data->mask_image, 0.0, 0.0);
    cairo_paint(cr);
    cairo_destroy(cr);
  }

  return TRUE;
}

static void clear_arc(struct data_t *data, cairo_surface_t *surface) {
  cairo_t *cr = cairo_create(surface);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_arc(cr, data->pointer_x, data->pointer_y, POINTER_CLEANING_RADIUS, 0.0,
      2 * M_PI);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_destroy(cr);
}

static gboolean on_mouse_poll_timer(gpointer user_data) {
  struct data_t *data = (struct data_t *)user_data;
  if (!data->initialized) {
    return TRUE;
  }

  GdkDeviceManager *device_manager = gdk_display_get_device_manager(
      gdk_window_get_display(data->root));
  GdkDevice *client_pointer = gdk_device_manager_get_client_pointer(
      device_manager);
  gint x, y;
  gdk_window_get_device_position(data->root, client_pointer, &x, &y, NULL);

  if (data->pointer_x == x && data->pointer_y == y) {
    return TRUE;
  }
  data->pointer_x = x;
  data->pointer_y = y;

  clear_arc(data, data->mask);
  clear_arc(data, data->mask_image);
  clear_arc(data, data->pixel_ages);

  return TRUE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);

  struct data_t data = {0};

  data.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  assert(data.window);
  GdkScreen *screen = gtk_widget_get_screen(data.window);
  assert(screen);
  GdkVisual *rgba_visual = gdk_screen_get_rgba_visual(screen);
  assert(rgba_visual);
  gtk_widget_set_visual(data.window, rgba_visual);
  g_object_unref(rgba_visual);
  gtk_window_set_title(GTK_WINDOW(data.window), "Anti Image Retention Overlay");
  gtk_window_set_keep_above(GTK_WINDOW(data.window), TRUE);
  gtk_window_set_accept_focus(GTK_WINDOW(data.window), FALSE);
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(data.window), TRUE);
  gtk_window_set_skip_pager_hint(GTK_WINDOW(data.window), TRUE);
  gtk_window_set_decorated(GTK_WINDOW(data.window), FALSE);
  // Set the input region to the empty region so that all input events go to the
  // windows beneath us.
  cairo_region_t *region = cairo_region_create();
  gtk_widget_input_shape_combine_region(data.window, region);
  cairo_region_destroy(region);
  gtk_window_fullscreen(GTK_WINDOW(data.window));
  g_signal_connect(G_OBJECT(data.window), "draw", G_CALLBACK(&on_draw), &data);
  g_signal_connect(G_OBJECT(data.window), "destroy", G_CALLBACK(&on_destroy),
      &data);
  gtk_window_present(GTK_WINDOW(data.window));
  assert(gtk_widget_is_composited(data.window));

  data.root = gdk_screen_get_root_window(screen);
  assert(data.root);

  data.pattern = cairo_pattern_create_linear(0.0, 0.0, 1.0, 0.0);
  cairo_pattern_add_color_stop_rgb(data.pattern, 0.0, BAR_COLOUR_R,
      BAR_COLOUR_G, BAR_COLOUR_B);
  cairo_pattern_add_color_stop_rgb(data.pattern, BAR_FRACTION, BAR_COLOUR_R,
      BAR_COLOUR_G, BAR_COLOUR_B);
  cairo_pattern_add_color_stop_rgb(data.pattern, BAR_FRACTION, 0.0, 0.0, 0.0);
  cairo_pattern_add_color_stop_rgb(data.pattern, 1.0, 0.0, 0.0, 0.0);
  cairo_pattern_set_extend(data.pattern, CAIRO_EXTEND_REPEAT);

  guint screen_capture_timeout_id = g_timeout_add(SCREEN_CAPTURE_PERIOD_MS,
      &on_screen_capture_timer, &data);
  guint mouse_poll_timeout_id = g_timeout_add(POINTER_POLL_PERIOD_MS,
      &on_mouse_poll_timer, &data);

  gtk_main();

  g_source_remove(mouse_poll_timeout_id);
  g_source_remove(screen_capture_timeout_id);
  cairo_pattern_destroy(data.pattern);

  return 0;
}

/*
    This file is part of Lumivell, a fork of Ansel.
    Copyright (C) 2026 Juscelino Be.

    Lumivell is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Lumivell is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Lumivell.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * Compare: the darkroom's before / after mode, entered from "Compare" in the mode switcher.
 *
 * The picture is compared with ANY step of its history ("Original" is step 0, before any edit),
 * shown three ways:
 *
 *   Side by side  the chosen step left, the current edit right, both fitted into half the view
 *   Split         the chosen step left of a divider that is dragged (or set with the slider)
 *   Toggle        one picture at a time; "\" or a click swaps before and after
 *
 * Each picture is a dt_dev_snapshot_t (develop/dev_snapshot.h), the same engine the Snapshots
 * module compares with: a frozen copy of the history, rendered for the current viewport by its
 * own pipe, recomputed in the background on pan and zoom. The history is read under
 * dev->history_mutex and copied (rule 4 of CLAUDE.md); nothing here writes to the edit.
 *
 * While comparing, the side panels and the filmstrip are hidden (and restored on leaving), the
 * focused module loses focus so its on-canvas tools do not draw over the comparison, and left
 * clicks on the picture belong to the comparison. Nothing is removed: "Back to editing", "Develop"
 * in the switcher, Escape or leaving the darkroom all return to the edit exactly as it was.
 */

#include "common/logging.h"
#include "common/module_versioning.h"
#include "control/control.h"
#include "control/redraw.h"
#include "control/signal.h"
#include "develop/dev_history.h"
#include "develop/dev_snapshot.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/iop_order.h"
#include "gui/application.h"
#include "gui/window_manager.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/dev_backbuf.h"
#include "views/view.h"
#include "widgets/bauhaus.h"
#include "widgets/draw.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>

DT_MODULE(1)

typedef enum dt_lib_compare_mode_t
{
  DT_COMPARE_SIDE_BY_SIDE = 0,
  DT_COMPARE_SPLIT = 1,
  DT_COMPARE_TOGGLE = 2,
  DT_COMPARE_MODES
} dt_lib_compare_mode_t;

#define DT_COMPARE_MODE_CONF "plugins/darkroom/compare/mode"
#define DT_COMPARE_SPLIT_CONF "plugins/darkroom/compare/split"

typedef struct dt_lib_compare_t
{
  gboolean active;
  gboolean pending;         // asked from another view: enter once the picture is loaded
  dt_lib_compare_mode_t mode;
  double split;             // divider position in the image, 0..1
  gboolean show_before;     // toggle mode: which one is on screen
  int32_t step;             // history end of "before"; 0 is the original
  int32_t steps;            // history end of the current edit

  dt_dev_snapshot_t before;
  dt_dev_snapshot_t after;  // drawn in side by side only: elsewhere the live picture is "after"

  gboolean dragging;
  gboolean hover;
  float box[4];             // image box in the view at the last draw (x, y, w, h)

  gboolean panel_visible[DT_UI_PANEL_SIZE];

  GtkWidget *toolbar;
  GtkWidget *mode_button[DT_COMPARE_MODES];
  GtkWidget *split_slider;
  GtkWidget *history_bar;
  GtkWidget *chips_box;
  GList *chips;             // GtkWidget*, index = history step
  gboolean syncing;
} dt_lib_compare_t;

static dt_lib_module_t *_self = NULL; // the switcher's handler carries no user data

const char *name(dt_lib_module_t *self)
{
  return _("Compare");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "darkroom", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  // no panel content of its own: its controls float over the picture
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1;
}

/* -- snapshots ---------------------------------------------------------------------------------- */

/* Render `dev`'s history as it stood after `end` items (-1: as it stands now). */
static gboolean _capture(dt_dev_snapshot_t *snap, dt_develop_t *dev, const int32_t end)
{
  dt_dev_snapshot_clear(snap);
  if(IS_NULL_PTR(dev) || dev->image_storage.id <= 0) return FALSE;

  dt_pthread_rwlock_rdlock(&dev->history_mutex);
  GList *history = dt_history_duplicate(dev->history);
  GList *iop_order = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
  const int32_t current = dt_dev_get_history_end_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  const int32_t history_end = (end < 0) ? current : MIN(end, current);
  dt_control_change_cursor_by_name_and_flush("progress");
  // ownership of both lists goes to the capture, win or lose
  const gboolean ok
      = dt_dev_snapshot_capture(snap, dev, dev->image_storage.id, history, iop_order, history_end);
  dt_control_commit_cursor();
  return ok;
}

/* -- history chips ------------------------------------------------------------------------------ */

static void _chips_sync(dt_lib_compare_t *d)
{
  int k = 0;
  for(GList *l = d->chips; l; l = g_list_next(l), k++)
  {
    if(k == d->step)
      dt_gui_add_class(GTK_WIDGET(l->data), "active");
    else
      dt_gui_remove_class(GTK_WIDGET(l->data), "active");
  }
}

static void _chip_clicked(GtkWidget *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  const int32_t step = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "step"));
  if(step == d->step && dt_dev_snapshot_is_valid(&d->before)) return;
  d->step = step;
  _capture(&d->before, dt_dev_get_global(), d->step);
  _chips_sync(d);
  dt_control_queue_redraw_center();
}

static GtkWidget *_chip_new(dt_lib_module_t *self, const char *label, const int32_t step)
{
  GtkWidget *chip = gtk_button_new_with_label(label);
  dt_gui_add_class(chip, "compare-chip");
  g_object_set_data(G_OBJECT(chip), "step", GINT_TO_POINTER(step));
  g_signal_connect(G_OBJECT(chip), "clicked", G_CALLBACK(_chip_clicked), self);
  return chip;
}

/* One chip per history step before the current one: "Original", then each item by its module. */
static void _chips_rebuild(dt_lib_module_t *self)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  dt_develop_t *dev = dt_dev_get_global();

  g_list_free(d->chips);
  d->chips = NULL;
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->chips_box));
  for(GList *l = children; l; l = g_list_next(l)) gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);

  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  if(dev)
  {
    dt_pthread_rwlock_rdlock(&dev->history_mutex);
    d->steps = dt_dev_get_history_end_ext(dev);
    int k = 0;
    for(const GList *l = dev->history; l && k < d->steps; l = g_list_next(l), k++)
    {
      const dt_dev_history_item_t *item = (const dt_dev_history_item_t *)l->data;
      gchar *item_name = (item && item->module) ? dt_history_item_get_name(item->module) : NULL;
      g_ptr_array_add(names, item_name ? item_name : g_strdup(_("unknown")));
    }
    dt_pthread_rwlock_unlock(&dev->history_mutex);
  }
  if(d->step > d->steps) d->step = 0;

  d->chips = g_list_append(d->chips, _chip_new(self, _("Original"), 0));
  // the last item is the current edit itself: comparing it with itself shows nothing
  for(guint k = 0; k + 1 < names->len; k++)
  {
    gchar *label = g_strdup_printf("%u · %s", k + 1, (const char *)g_ptr_array_index(names, k));
    d->chips = g_list_append(d->chips, _chip_new(self, label, (int32_t)k + 1));
    dt_free(label);
  }
  g_ptr_array_free(names, TRUE);

  for(GList *l = d->chips; l; l = g_list_next(l))
  {
    gtk_box_pack_start(GTK_BOX(d->chips_box), GTK_WIDGET(l->data), FALSE, FALSE, 0);
    gtk_widget_show(GTK_WIDGET(l->data));
  }
  _chips_sync(d);
}

/* -- modes -------------------------------------------------------------------------------------- */

static void _modes_sync(dt_lib_compare_t *d)
{
  d->syncing = TRUE;
  for(int k = 0; k < DT_COMPARE_MODES; k++)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->mode_button[k]), k == (int)d->mode);
  gtk_widget_set_visible(d->split_slider, d->mode == DT_COMPARE_SPLIT);
  d->syncing = FALSE;
}

static void _set_mode(dt_lib_module_t *self, const dt_lib_compare_mode_t mode)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  d->mode = mode;
  dt_conf_set_int(DT_COMPARE_MODE_CONF, mode);
  if(mode == DT_COMPARE_SIDE_BY_SIDE && !dt_dev_snapshot_is_valid(&d->after))
    _capture(&d->after, dt_dev_get_global(), -1);
  _modes_sync(d);
  dt_control_queue_redraw_center();
}

static void _mode_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(d->syncing) return;
  if(!gtk_toggle_button_get_active(button))
  {
    _modes_sync(d); // radio behavior: the active one cannot be switched off
    return;
  }
  _set_mode(self, (dt_lib_compare_mode_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mode")));
}

static void _split_changed(GtkWidget *slider, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(d->syncing) return;
  d->split = CLAMP(dt_bauhaus_slider_get(slider) / 100.0, 0.0, 1.0);
  dt_control_queue_redraw_center();
}

static void _set_split(dt_lib_compare_t *d, const double split)
{
  d->split = CLAMP(split, 0.0, 1.0);
  d->syncing = TRUE;
  dt_bauhaus_slider_set(d->split_slider, d->split * 100.0);
  d->syncing = FALSE;
  dt_control_queue_redraw_center();
}

/* -- enter / leave ------------------------------------------------------------------------------ */

static void _enter(dt_lib_module_t *self)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  dt_develop_t *dev = dt_dev_get_global();
  dt_ui_t *ui = dt_gui_get_ui();
  if(d->active || IS_NULL_PTR(dev) || IS_NULL_PTR(ui) || dev->image_storage.id <= 0) return;

  // the focused module's on-canvas tools would draw over the comparison
  dt_iop_request_focus(NULL);

  // the picture gets the whole window; the panels come back as they were
  const dt_ui_panel_t hide[] = { DT_UI_PANEL_LEFT, DT_UI_PANEL_RIGHT, DT_UI_PANEL_BOTTOM };
  for(size_t k = 0; k < G_N_ELEMENTS(hide); k++)
  {
    d->panel_visible[hide[k]] = ui->panels[hide[k]] && gtk_widget_get_visible(ui->panels[hide[k]]);
    if(d->panel_visible[hide[k]]) dt_ui_panel_show(ui, hide[k], FALSE, FALSE);
  }
  GtkWidget *floatbar = g_object_get_data(G_OBJECT(dt_gui_main_window()), "lumivell-floatbar");
  if(floatbar) gtk_widget_hide(floatbar);

  d->step = 0;
  d->show_before = FALSE;
  d->dragging = FALSE;
  _chips_rebuild(self);
  _capture(&d->before, dev, d->step);
  dt_dev_snapshot_clear(&d->after);
  if(d->mode == DT_COMPARE_SIDE_BY_SIDE) _capture(&d->after, dev, -1);

  _modes_sync(d);
  gtk_widget_show(d->toolbar);
  gtk_widget_show(d->history_bar);

  d->active = TRUE;
  dt_ui_set_compare_active(TRUE);
  dt_control_queue_redraw_center();
}

static void _leave(dt_lib_module_t *self)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  d->pending = FALSE;
  if(!d->active) return;
  d->active = FALSE;
  d->dragging = FALSE;

  gtk_widget_hide(d->toolbar);
  gtk_widget_hide(d->history_bar);
  dt_dev_snapshot_clear(&d->before);
  dt_dev_snapshot_clear(&d->after);

  dt_ui_t *ui = dt_gui_get_ui();
  const dt_ui_panel_t restore[] = { DT_UI_PANEL_LEFT, DT_UI_PANEL_RIGHT, DT_UI_PANEL_BOTTOM };
  for(size_t k = 0; ui && k < G_N_ELEMENTS(restore); k++)
    if(d->panel_visible[restore[k]]) dt_ui_panel_show(ui, restore[k], TRUE, FALSE);
  GtkWidget *floatbar = g_object_get_data(G_OBJECT(dt_gui_main_window()), "lumivell-floatbar");
  if(floatbar) gtk_widget_show(floatbar);

  dt_ui_set_compare_active(FALSE);
  dt_control_queue_cursor_by_name("left_ptr");
  dt_control_queue_redraw_center();
}

static void _switcher_handler(gboolean active)
{
  if(IS_NULL_PTR(_self) || IS_NULL_PTR(_self->data)) return;
  if(active)
    _enter(_self);
  else
    _leave(_self);
}

static void _done_clicked(GtkWidget *button, gpointer user_data)
{
  _leave((dt_lib_module_t *)user_data);
}

static void _develop_changed(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  dt_develop_t *dev = dt_dev_get_global();
  if(IS_NULL_PTR(d) || IS_NULL_PTR(dev)) return;

  if(d->pending && dev->image_storage.id > 0)
  {
    d->pending = FALSE;
    _enter(self);
    return;
  }
  if(!d->active) return;

  // the edit moved under us (undo, redo, another picture): follow it
  _chips_rebuild(self); // also brings the chosen step back into range
  _capture(&d->before, dev, d->step);
  if(d->mode == DT_COMPARE_SIDE_BY_SIDE)
    _capture(&d->after, dev, -1);
  else
    dt_dev_snapshot_clear(&d->after);
  dt_control_queue_redraw_center();
}

static void _image_changed(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(IS_NULL_PTR(d)) return;
  if(d->active) d->step = 0; // a new picture starts from its original
  _develop_changed(instance, user_data);
}

/* -- drawing ------------------------------------------------------------------------------------ */

static void _draw_badge(cairo_t *cr, const char *text, const double x, const double y,
                        const gboolean right_aligned, const gboolean accent)
{
  PangoFontDescription *desc = pango_font_description_copy_static(dt_bauhaus_get_global()->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_SEMIBOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(13) * PANGO_SCALE);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, text, -1);
  PangoRectangle ink, logical;
  pango_layout_get_pixel_extents(layout, &ink, &logical);

  const double pad_x = DT_PIXEL_APPLY_DPI(12), pad_y = DT_PIXEL_APPLY_DPI(5);
  const double w = logical.width + 2 * pad_x, h = logical.height + 2 * pad_y;
  const double bx = right_aligned ? x - w : x;

  dt_draw_rounded_rectangle_path(cr, bx, y, w, h, h / 2.0);
  if(accent)
  {
    const GdkRGBA c = dt_bauhaus_get_global()->color_value;
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, 1.0);
  }
  else
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_fill(cr);

  if(accent)
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, 1.0);
  else
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
  cairo_move_to(cr, bx + pad_x, y + pad_y);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  pango_font_description_free(desc);
}

static void _draw_divider(cairo_t *cr, const double x, const double y0, const double y1)
{
  cairo_save(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  cairo_move_to(cr, x, y0);
  cairo_line_to(cr, x, y1);
  cairo_stroke(cr);

  // round handle with "<>" in the middle
  const double cy = 0.5 * (y0 + y1);
  const double r = DT_PIXEL_APPLY_DPI(18);
  cairo_arc(cr, x, cy + DT_PIXEL_APPLY_DPI(1), r + DT_PIXEL_APPLY_DPI(1), 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
  cairo_fill(cr);
  cairo_arc(cr, x, cy, r, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 0.93, 0.92, 0.90, 1.0);
  cairo_fill(cr);

  const double a = DT_PIXEL_APPLY_DPI(4), gap = DT_PIXEL_APPLY_DPI(3);
  cairo_set_source_rgba(cr, 0.08, 0.08, 0.09, 1.0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.8));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_move_to(cr, x - gap, cy - a);
  cairo_line_to(cr, x - gap - a, cy);
  cairo_line_to(cr, x - gap, cy + a);
  cairo_move_to(cr, x + gap, cy - a);
  cairo_line_to(cr, x + gap + a, cy);
  cairo_line_to(cr, x + gap, cy + a);
  cairo_stroke(cr);
  cairo_restore(cr);
}

/* Draw a snapshot fitted into (tx, ty, tw, th): the snapshot paints as the live picture would,
 * inside `box`; scaling the context maps that box onto the target. */
static void _draw_fitted(cairo_t *cr, dt_dev_snapshot_t *snap, dt_develop_t *dev, const int32_t width,
                         const int32_t height, const float box[4], const double tx, const double ty,
                         const double scale)
{
  cairo_save(cr);
  cairo_translate(cr, tx, ty);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -box[0], -box[1]);
  dt_dev_snapshot_draw(snap, cr, dev, width, height, box[0], box[1], box[2], box[3]);
  cairo_restore(cr);
}

void gui_post_expose(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(IS_NULL_PTR(d) || !d->active) return;
  dt_develop_t *dev = dt_dev_get_global();
  if(IS_NULL_PTR(dev)) return;

  float box[4] = { 0.0f };
  dt_dev_get_image_box_in_widget(dev, width, height, box);
  if(box[2] <= 0.0f || box[3] <= 0.0f) return;
  memcpy(d->box, box, sizeof(box));

  const double margin = DT_PIXEL_APPLY_DPI(14);
  const char *before_label = (d->step == 0) ? _("Before") : _("Step");

  switch(d->mode)
  {
    case DT_COMPARE_SPLIT:
    {
      const double sx = box[0] + box[2] * d->split;
      if(dt_dev_snapshot_is_valid(&d->before))
        dt_dev_snapshot_draw(&d->before, cr, dev, width, height, box[0], box[1], sx - box[0], box[3]);
      _draw_divider(cr, sx, box[1], box[1] + box[3]);
      _draw_badge(cr, before_label, box[0] + margin, box[1] + margin, FALSE, FALSE);
      _draw_badge(cr, _("After"), box[0] + box[2] - margin, box[1] + margin, TRUE, TRUE);
      dt_control_queue_cursor_by_name(d->dragging ? "grabbing" : (d->hover ? "col-resize" : "left_ptr"));
      break;
    }
    case DT_COMPARE_TOGGLE:
    {
      if(d->show_before && dt_dev_snapshot_is_valid(&d->before))
        dt_dev_snapshot_draw(&d->before, cr, dev, width, height, box[0], box[1], box[2], box[3]);
      if(d->show_before)
        _draw_badge(cr, before_label, box[0] + margin, box[1] + margin, FALSE, FALSE);
      else
        _draw_badge(cr, _("After"), box[0] + box[2] - margin, box[1] + margin, TRUE, TRUE);
      dt_control_queue_cursor_by_name("hand2");
      break;
    }
    case DT_COMPARE_SIDE_BY_SIDE:
    default:
    {
      // both pictures fitted into one half each, over the whole view
      dt_aligned_pixel_t bg = { 0.0f };
      dt_dev_get_background_color(dev, bg);
      cairo_save(cr);
      cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
      cairo_paint(cr);
      cairo_restore(cr);

      const double gap = DT_PIXEL_APPLY_DPI(16);
      const double half = (width - 3.0 * gap) / 2.0;
      const double scale = fmin(half / box[2], (height - 2.0 * gap) / box[3]);
      const double tw = box[2] * scale, th = box[3] * scale;
      const double ty = (height - th) / 2.0;
      const double lx = gap + (half - tw) / 2.0;
      const double rx = 2.0 * gap + half + (half - tw) / 2.0;

      if(dt_dev_snapshot_is_valid(&d->before)) _draw_fitted(cr, &d->before, dev, width, height, box, lx, ty, scale);
      if(dt_dev_snapshot_is_valid(&d->after)) _draw_fitted(cr, &d->after, dev, width, height, box, rx, ty, scale);
      _draw_badge(cr, before_label, lx + margin, ty + margin, FALSE, FALSE);
      _draw_badge(cr, _("After"), rx + tw - margin, ty + margin, TRUE, TRUE);
      dt_control_queue_cursor_by_name("left_ptr");
      break;
    }
  }
}

/* -- mouse and keys ----------------------------------------------------------------------------- */

static gboolean _in_box(const dt_lib_compare_t *d, const double x, const double y)
{
  return x >= d->box[0] && x <= d->box[0] + d->box[2] && y >= d->box[1] && y <= d->box[1] + d->box[3];
}

int button_pressed(dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(IS_NULL_PTR(d) || !d->active) return 0;
  if(which != 1) return 1; // no module tool answers clicks while comparing

  if(d->mode == DT_COMPARE_SPLIT && _in_box(d, x, y) && d->box[2] > 0.0f)
  {
    d->dragging = TRUE;
    _set_split(d, (x - d->box[0]) / d->box[2]);
  }
  else if(d->mode == DT_COMPARE_TOGGLE && _in_box(d, x, y))
  {
    d->show_before = !d->show_before;
    dt_control_queue_redraw_center();
  }
  return 1;
}

int button_released(dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(IS_NULL_PTR(d) || !d->active) return 0;
  if(d->dragging)
  {
    d->dragging = FALSE;
    dt_conf_set_float(DT_COMPARE_SPLIT_CONF, (float)d->split);
    dt_control_queue_redraw_center();
  }
  return 1;
}

int mouse_moved(dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(IS_NULL_PTR(d) || !d->active) return 0;
  if(d->mode != DT_COMPARE_SPLIT) return 1;

  if(d->dragging && d->box[2] > 0.0f)
  {
    _set_split(d, (x - d->box[0]) / d->box[2]);
    return 1;
  }
  const double sx = d->box[0] + d->box[2] * d->split;
  const gboolean hover = _in_box(d, x, y) && fabs(x - sx) <= DT_PIXEL_APPLY_DPI(24);
  if(hover != d->hover)
  {
    d->hover = hover;
    dt_control_queue_redraw_center();
  }
  return 1;
}

int key_pressed(dt_lib_module_t *self, GdkEventKey *event)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(IS_NULL_PTR(d) || !d->active) return 0;
  if(event->keyval == GDK_KEY_backslash)
  {
    if(d->mode != DT_COMPARE_TOGGLE)
    {
      d->show_before = TRUE;
      _set_mode(self, DT_COMPARE_TOGGLE);
    }
    else
    {
      d->show_before = !d->show_before;
      dt_control_queue_redraw_center();
    }
    return 1;
  }
  if(event->keyval == GDK_KEY_Escape)
  {
    _leave(self);
    return 1;
  }
  return 0;
}

/* -- lifecycle ---------------------------------------------------------------------------------- */

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(dt_ui_take_compare_pending())
  {
    // the picture is loaded after the darkroom opens: enter when it is there
    d->pending = TRUE;
  }
}

void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  _leave(self);
}

static GtkWidget *_mode_button_new(dt_lib_module_t *self, const char *label, const char *tooltip,
                                   const dt_lib_compare_mode_t mode)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  GtkWidget *b = gtk_toggle_button_new_with_label(label);
  gtk_widget_set_tooltip_text(b, tooltip);
  g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(mode));
  g_signal_connect(G_OBJECT(b), "toggled", G_CALLBACK(_mode_toggled), self);
  d->mode_button[mode] = b;
  return b;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_compare_t *d = (dt_lib_compare_t *)calloc(1, sizeof(dt_lib_compare_t));
  self->data = d;
  _self = self;

  d->mode = dt_conf_key_exists(DT_COMPARE_MODE_CONF)
      ? (dt_lib_compare_mode_t)CLAMP(dt_conf_get_int(DT_COMPARE_MODE_CONF), 0, DT_COMPARE_MODES - 1)
      : DT_COMPARE_SPLIT;
  d->split = dt_conf_key_exists(DT_COMPARE_SPLIT_CONF)
      ? CLAMP(dt_conf_get_float(DT_COMPARE_SPLIT_CONF), 0.0f, 1.0f)
      : 0.5;

  // nothing in the panel
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  // toolbar over the top of the picture: modes, divider, back to editing
  d->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->toolbar, "compare-toolbar");
  gtk_widget_set_halign(d->toolbar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(d->toolbar, GTK_ALIGN_START);

  GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(modes, "compare-modes");
  gtk_box_pack_start(GTK_BOX(modes),
                     _mode_button_new(self, _("Side by side"), _("Before left, after right"),
                                      DT_COMPARE_SIDE_BY_SIDE),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(modes),
                     _mode_button_new(self, _("Split"), _("Before and after on each side of a divider you drag"),
                                      DT_COMPARE_SPLIT),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(modes),
                     _mode_button_new(self, _("Toggle \\"),
                                      _("One at a time: press \\ or click the picture to swap"), DT_COMPARE_TOGGLE),
                     FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->toolbar), modes, FALSE, FALSE, 0);

  d->split_slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(self), 0.f, 100.f,
                                                     1.f, 50.f, 0);
  dt_bauhaus_widget_set_label(d->split_slider, N_("Split position"));
  dt_bauhaus_slider_set_format(d->split_slider, "%");
  dt_bauhaus_slider_set(d->split_slider, d->split * 100.0);
  gtk_widget_set_name(d->split_slider, "compare-split");
  gtk_widget_set_size_request(d->split_slider, DT_PIXEL_APPLY_DPI(240), -1);
  gtk_widget_set_hexpand(d->split_slider, FALSE);
  gtk_widget_set_valign(d->split_slider, GTK_ALIGN_CENTER);
  g_signal_connect(G_OBJECT(d->split_slider), "value-changed", G_CALLBACK(_split_changed), self);
  gtk_widget_set_no_show_all(d->split_slider, TRUE);
  gtk_box_pack_start(GTK_BOX(d->toolbar), d->split_slider, FALSE, FALSE, 0);

  GtkWidget *done = gtk_button_new_with_label(_("Back to editing"));
  gtk_widget_set_name(done, "compare-done");
  gtk_widget_set_tooltip_text(done, _("Leave the comparison (Escape)"));
  g_signal_connect(G_OBJECT(done), "clicked", G_CALLBACK(_done_clicked), self);
  gtk_box_pack_start(GTK_BOX(d->toolbar), done, FALSE, FALSE, 0);

  // history steps along the bottom
  d->history_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->history_bar, "compare-history");
  gtk_widget_set_halign(d->history_bar, GTK_ALIGN_FILL);
  gtk_widget_set_valign(d->history_bar, GTK_ALIGN_END);
  GtkWidget *with = gtk_label_new(_("Compare with"));
  gtk_widget_set_name(with, "compare-with");
  gtk_box_pack_start(GTK_BOX(d->history_bar), with, FALSE, FALSE, 0);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  d->chips_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->chips_box, "compare-chips");
  gtk_container_add(GTK_CONTAINER(scroll), d->chips_box);
  gtk_box_pack_start(GTK_BOX(d->history_bar), scroll, TRUE, TRUE, 0);

  GtkWidget *overlay = dt_ui_center_base(dt_gui_get_ui());
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->toolbar);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->history_bar);
  gtk_widget_show_all(d->toolbar);
  gtk_widget_show_all(d->history_bar);
  gtk_widget_set_no_show_all(d->toolbar, TRUE);
  gtk_widget_set_no_show_all(d->history_bar, TRUE);
  gtk_widget_hide(d->toolbar);
  gtk_widget_hide(d->history_bar);

  dt_ui_set_compare_handler(_switcher_handler);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                                  G_CALLBACK(_develop_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_develop_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_image_changed), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_develop_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_image_changed), self);
  dt_ui_set_compare_handler(NULL);
  dt_lib_compare_t *d = (dt_lib_compare_t *)self->data;
  if(d)
  {
    dt_dev_snapshot_clear(&d->before);
    dt_dev_snapshot_clear(&d->after);
    g_list_free(d->chips);
  }
  _self = NULL;
  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

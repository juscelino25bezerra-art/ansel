/*
    This file is part of Lumivell, a fork of Ansel.
    Copyright (C) 2026 Juscelino Be.

    Lumivell is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Lumivell is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

#include "views/darkroom_rails.h"

#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/actions/menu.h"
#include "gui/application.h"
#include "gui/window_manager.h"
#include "libs/lib.h"
#include "views/view.h"
#include "widgets/button.h"
#include "widgets/paint.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include "system/macros.h"

#include <glib/gi18n.h>
#include <string.h>

typedef enum dt_rail_tool_t
{
  DT_RAIL_TOOL_ADJUST = 0,
  DT_RAIL_TOOL_CROP,
  DT_RAIL_TOOL_RETOUCH,
  DT_RAIL_TOOL_MASKS,
  DT_RAIL_TOOL_PRESETS,
  DT_RAIL_TOOL_COUNT
} dt_rail_tool_t;

typedef struct dt_darkroom_rails_t
{
  struct dt_develop_t *dev;
  gboolean built;
  GtkWidget *tools[DT_RAIL_TOOL_COUNT];
  GList *left_buttons; // GtkWidget*, each with its dt_lib_module_t* as "rail-lib"
} dt_darkroom_rails_t;

static dt_darkroom_rails_t _rails = { 0 };

/* -- left rail: the left panel's modules -------------------------------------------------------- */

typedef struct dt_rail_lib_icon_t
{
  const char *plugin;
  const char *path;
} dt_rail_lib_icon_t;

static const dt_rail_lib_icon_t _lib_icons[] = {
  { "navigation", DTGTK_LINE_ICON_NAVIGATOR },
  { "history", DTGTK_LINE_ICON_HISTORY },
  { "blending", DTGTK_LINE_ICON_BLEND },
  { "duplicate", DTGTK_LINE_ICON_DUPLICATES },
  { "snapshots", DTGTK_LINE_ICON_SNAPSHOTS },
  { "textnotes", DTGTK_LINE_ICON_NOTES },
  { "metadata_view", DTGTK_LINE_ICON_INFO },
};

static const char *_lib_icon(const dt_lib_module_t *module)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_lib_icons); i++)
    if(!strcmp(module->plugin_name, _lib_icons[i].plugin)) return _lib_icons[i].path;
  return DTGTK_LINE_ICON_PANEL;
}

static gboolean _is_left_lib(dt_lib_module_t *module, const dt_view_t *view)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->container) || IS_NULL_PTR(module->widget)) return FALSE;
  const uint32_t c = module->container(module);
  if(c != DT_UI_CONTAINER_PANEL_LEFT_TOP && c != DT_UI_CONTAINER_PANEL_LEFT_CENTER) return FALSE;
  return dt_lib_is_visible_in_view(module, view);
}

/* An expandable module is "open" when expanded; the navigation thumbnail has no header and is
 * open when shown. */
static gboolean _lib_is_open(dt_lib_module_t *module)
{
  if(module->expandable && module->expandable(module))
    return dt_lib_is_visible(module) && dt_lib_gui_get_expanded(module);
  return dt_lib_is_visible(module);
}

static void _left_sync(void)
{
  for(GList *l = _rails.left_buttons; l; l = g_list_next(l))
  {
    GtkWidget *button = GTK_WIDGET(l->data);
    dt_lib_module_t *module = g_object_get_data(G_OBJECT(button), "rail-lib");
    if(module && _lib_is_open(module))
      dt_gui_add_class(button, "active");
    else
      dt_gui_remove_class(button, "active");
  }
}

static void _left_clicked(GtkWidget *button, gpointer user_data)
{
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;
  const gboolean expandable = module->expandable && module->expandable(module);

  if(!expandable)
  {
    dt_lib_set_visible(module, !dt_lib_is_visible(module));
  }
  else if(_lib_is_open(module))
  {
    dt_lib_gui_set_expanded(module, FALSE);
  }
  else
  {
    // one open at a time, like a drawer: fold the other expandable modules of the left panel
    for(GList *l = _rails.left_buttons; l; l = g_list_next(l))
    {
      dt_lib_module_t *other = g_object_get_data(G_OBJECT(l->data), "rail-lib");
      if(other && other != module && other->expandable && other->expandable(other)
         && dt_lib_gui_get_expanded(other))
        dt_lib_gui_set_expanded(other, FALSE);
    }
    if(!dt_lib_is_visible(module)) dt_lib_set_visible(module, TRUE);
    dt_lib_gui_set_expanded(module, TRUE);
    if(module->expander) dt_gui_get_global()->scroll_to[0] = module->expander;
  }
  _left_sync();
}

static gint _sort_left(gconstpointer a, gconstpointer b)
{
  dt_lib_module_t *ma = (dt_lib_module_t *)a, *mb = (dt_lib_module_t *)b;
  const uint32_t ca = ma->container(ma), cb = mb->container(mb);
  if(ca != cb) return (ca == DT_UI_CONTAINER_PANEL_LEFT_TOP) ? -1 : 1;
  // panels stack modules by decreasing position
  const int pa = ma->position ? ma->position() : 0, pb = mb->position ? mb->position() : 0;
  return pb - pa;
}

static void _build_left(GtkWidget *rail, const dt_view_t *view)
{
  dt_lib_t *lib = dt_lib_get_global();
  if(IS_NULL_PTR(lib)) return;

  GList *mods = NULL;
  for(GList *l = lib->plugins; l; l = g_list_next(l))
    if(_is_left_lib((dt_lib_module_t *)l->data, view)) mods = g_list_prepend(mods, l->data);
  mods = g_list_sort(mods, _sort_left);

  for(GList *l = mods; l; l = g_list_next(l))
  {
    dt_lib_module_t *module = (dt_lib_module_t *)l->data;
    GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_line_icon, 0, (void *)_lib_icon(module));
    dt_gui_add_class(button, "rail-button");
    gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(40), DT_PIXEL_APPLY_DPI(40));
    gchar *label = g_strdup(module->name(module));
    dt_capitalize_label(label);
    gtk_widget_set_tooltip_text(button, label);
    dt_free(label);
    g_object_set_data(G_OBJECT(button), "rail-lib", module);
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_left_clicked), module);
    gtk_box_pack_start(GTK_BOX(rail), button, FALSE, FALSE, 0);
    gtk_widget_show(button);
    _rails.left_buttons = g_list_append(_rails.left_buttons, button);
  }
  g_list_free(mods);
}

/* -- right rail: tools ------------------------------------------------------------------------- */

static dt_iop_module_t *_base_module(const char *op)
{
  if(IS_NULL_PTR(_rails.dev)) return NULL;
  for(const GList *l = _rails.dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)l->data;
    if(m->multi_priority == 0 && !strcmp(m->op, op) && !IS_NULL_PTR(m->gui)) return m;
  }
  return NULL;
}

static void _tools_set_active(dt_rail_tool_t tool)
{
  for(int k = 0; k < DT_RAIL_TOOL_COUNT; k++)
  {
    if(IS_NULL_PTR(_rails.tools[k])) continue;
    if(k == (int)tool)
      dt_gui_add_class(_rails.tools[k], "active");
    else
      dt_gui_remove_class(_rails.tools[k], "active");
  }
}

static void _scroll_right_panel_to_top(void)
{
  GtkWidget *center = GTK_WIDGET(dt_ui_get_container(dt_gui_get_ui(), DT_UI_CONTAINER_PANEL_RIGHT_CENTER));
  GtkWidget *sw = center ? gtk_widget_get_ancestor(center, GTK_TYPE_SCROLLED_WINDOW) : NULL;
  if(sw)
  {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_lower(adj));
  }
}

static void _open_module(const char *op)
{
  dt_iop_module_t *module = _base_module(op);
  if(IS_NULL_PTR(module)) return;
  // focusing a module also switches to a module tab that holds it
  dt_iop_request_focus(module);
  dt_iop_gui_set_expanded(module, TRUE, TRUE);
  if(module->gui->expander) dt_gui_get_global()->scroll_to[1] = module->gui->expander;
}

static void _open_left_lib(const char *plugin)
{
  for(GList *l = _rails.left_buttons; l; l = g_list_next(l))
  {
    dt_lib_module_t *module = g_object_get_data(G_OBJECT(l->data), "rail-lib");
    if(module && !strcmp(module->plugin_name, plugin))
    {
      if(!_lib_is_open(module)) _left_clicked(GTK_WIDGET(l->data), module);
      return;
    }
  }
}

static void _tool_clicked(GtkWidget *button, gpointer user_data)
{
  const dt_rail_tool_t tool = (dt_rail_tool_t)GPOINTER_TO_INT(user_data);
  switch(tool)
  {
    case DT_RAIL_TOOL_ADJUST:
      dt_iop_request_focus(NULL);
      _scroll_right_panel_to_top();
      break;
    case DT_RAIL_TOOL_CROP:
      _open_module("crop");
      break;
    case DT_RAIL_TOOL_RETOUCH:
      _open_module("retouch");
      break;
    case DT_RAIL_TOOL_MASKS:
      _open_left_lib("blending");
      break;
    case DT_RAIL_TOOL_PRESETS:
      // a menu, not a mode: the highlighted tool does not change
      dt_ui_popup_global_menu(dt_gui_get_ui(), DT_MENU_STYLES, button);
      return;
    default:
      return;
  }
  _tools_set_active(tool);
}

static void _build_right(GtkWidget *rail)
{
  static const struct
  {
    const char *path;
    const char *tooltip;
  } defs[DT_RAIL_TOOL_COUNT] = {
    [DT_RAIL_TOOL_ADJUST] = { DTGTK_LINE_ICON_ADJUST, N_("Adjust") },
    [DT_RAIL_TOOL_CROP] = { DTGTK_LINE_ICON_CROP, N_("Crop and straighten") },
    [DT_RAIL_TOOL_RETOUCH] = { DTGTK_LINE_ICON_HEAL, N_("Retouch") },
    [DT_RAIL_TOOL_MASKS] = { DTGTK_LINE_ICON_MASK, N_("Masking & Blending") },
    [DT_RAIL_TOOL_PRESETS] = { DTGTK_LINE_ICON_PRESETS, N_("Styles") },
  };

  for(int k = 0; k < DT_RAIL_TOOL_COUNT; k++)
  {
    GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_line_icon, 0, (void *)defs[k].path);
    dt_gui_add_class(button, "rail-button");
    gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(40), DT_PIXEL_APPLY_DPI(40));
    gtk_widget_set_tooltip_text(button, _(defs[k].tooltip));
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_tool_clicked), GINT_TO_POINTER(k));
    gtk_box_pack_start(GTK_BOX(rail), button, FALSE, FALSE, 0);
    gtk_widget_show(button);
    _rails.tools[k] = button;
  }
}

/* -- lifecycle ---------------------------------------------------------------------------------- */

void dt_darkroom_rails_enter(struct dt_develop_t *dev, const struct dt_view_t *view)
{
  dt_ui_t *ui = dt_gui_get_ui();
  if(IS_NULL_PTR(ui)) return;
  _rails.dev = dev;

  // built on the first visit: the lib modules are loaded by then
  if(!_rails.built)
  {
    if(IS_NULL_PTR(view)) return;
    _build_left(dt_ui_rail(ui, FALSE), view);
    _build_right(dt_ui_rail(ui, TRUE));
    _rails.built = TRUE;
  }

  _left_sync();
  _tools_set_active(DT_RAIL_TOOL_ADJUST);
  dt_ui_rail_set_enabled(ui, FALSE, TRUE);
  dt_ui_rail_set_enabled(ui, TRUE, TRUE);
}

void dt_darkroom_rails_leave(void)
{
  dt_ui_t *ui = dt_gui_get_ui();
  if(IS_NULL_PTR(ui)) return;
  dt_ui_rail_set_enabled(ui, FALSE, FALSE);
  dt_ui_rail_set_enabled(ui, TRUE, FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

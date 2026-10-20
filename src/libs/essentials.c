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
 * Essentials: the darkroom's "Light" and "Color" sliders.
 *
 * A short list of the adjustments people reach for first, each one a REMOTE CONTROL for a
 * control that already exists in a scene-referred module:
 *
 *   Light   exposure       -> exposure / exposure
 *           contrast       -> color balance rgb / contrast
 *           highlights     -> tone equalizer / highlights   (-2 EV band)
 *           shadows        -> tone equalizer / blacks       (-5 EV band, "light shadows")
 *           whites         -> tone equalizer / whites       (-1 EV band)
 *           blacks         -> tone equalizer / deep_blacks  (-6 EV band, "shadows")
 *   Color   temperature    -> white balance / temperature
 *           tint           -> white balance / tint
 *           vibrance       -> color balance rgb / vibrance
 *           saturation     -> color balance rgb / saturation_global
 *
 * Moving one of these sliders sets the module's OWN slider (dt_bauhaus_slider_set()), so the
 * change goes through that module's regular callback: params updated on the GUI thread, the
 * module enabled if it was off, one history item, undo, XMP -- exactly as if the user had
 * dragged the module's slider (rule 4 of CLAUDE.md is honored by construction: nothing here
 * touches params or the pipeline directly). The panel reads back from the same widgets whenever
 * the history or the image changes, so editing a module directly is reflected here too.
 *
 * The scale (range, step, unit, color stops) is copied from the module's slider. Exposure,
 * temperature and tint show the module's own number; the others show a whole number from -100 to
 * +100 over the module's soft range (a display factor only: the value stored is the module's).
 *
 * The panel title and the Auto / B&W / reset actions sit in the scopes card's header row, above
 * the histogram, as one "Adjust" panel; the Light and Color sections fold and count their edits.
 */

#include "common/conf.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/application.h"
#include "gui/window_manager.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "widgets/bauhaus.h"
#include "widgets/button.h"
#include "widgets/paint.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include <math.h>
#include <string.h>

DT_MODULE(1)

typedef enum dt_lib_essentials_section_t
{
  DT_ESSENTIALS_LIGHT = 0,
  DT_ESSENTIALS_COLOR = 1,
  DT_ESSENTIALS_SECTIONS
} dt_lib_essentials_section_t;

typedef struct dt_lib_essentials_def_t
{
  const char *op;           // target module
  const char *param;        // introspection field of the target slider, or NULL
  const char *target_label; // label of the target slider (untranslated) when it has no field
  const char *label;        // our label (untranslated)
  const char *tooltip;      // our tooltip (untranslated)
  dt_lib_essentials_section_t section;
  gboolean scale100;        // show as a whole number from -100 to +100 over the soft range
} dt_lib_essentials_def_t;

static const dt_lib_essentials_def_t _defs[] = {
  { "exposure", "exposure", NULL, N_("Exposure"),
    N_("brightness of the whole picture, in EV (exposure module)"), DT_ESSENTIALS_LIGHT, FALSE },
  { "colorbalancergb", "contrast", NULL, N_("Contrast"),
    N_("separation between darks and lights around middle gray (color balance rgb)"), DT_ESSENTIALS_LIGHT, TRUE },
  { "toneequal", "highlights", NULL, N_("Highlights"),
    N_("brighten or darken the bright areas (tone equalizer, -2 EV band)"), DT_ESSENTIALS_LIGHT, TRUE },
  { "toneequal", "blacks", NULL, N_("Shadows"),
    N_("brighten or darken the dark areas (tone equalizer, -5 EV band)"), DT_ESSENTIALS_LIGHT, TRUE },
  { "toneequal", "whites", NULL, N_("Whites"),
    N_("brighten or darken the brightest tones (tone equalizer, -1 EV band)"), DT_ESSENTIALS_LIGHT, TRUE },
  { "toneequal", "deep_blacks", NULL, N_("Blacks"),
    N_("brighten or darken the darkest tones (tone equalizer, -6 EV band)"), DT_ESSENTIALS_LIGHT, TRUE },
  { "temperature", NULL, N_("temperature"), N_("Temperature"),
    N_("make the picture warmer or cooler (white balance)"), DT_ESSENTIALS_COLOR, FALSE },
  { "temperature", NULL, N_("tint"), N_("Tint"),
    N_("shift colors between magenta and green (white balance)"), DT_ESSENTIALS_COLOR, FALSE },
  { "colorbalancergb", "vibrance", NULL, N_("Vibrance"),
    N_("strengthen the least saturated colors first (color balance rgb)"), DT_ESSENTIALS_COLOR, TRUE },
  { "colorbalancergb", "saturation_global", NULL, N_("Saturation"),
    N_("strengthen or mute all colors (color balance rgb)"), DT_ESSENTIALS_COLOR, TRUE },
};

#define DT_ESSENTIALS_COUNT (sizeof(_defs) / sizeof(_defs[0]))

/* black & white: color balance rgb's global chroma all the way down */
static const dt_lib_essentials_def_t _bw_def
    = { "colorbalancergb", "chroma_global", NULL, NULL, NULL, DT_ESSENTIALS_COLOR, FALSE };

static const char *_section_conf[DT_ESSENTIALS_SECTIONS]
    = { "plugins/darkroom/essentials/light_collapsed", "plugins/darkroom/essentials/color_collapsed" };

#define DT_ESSENTIALS_CHEVRON_DOWN "m6 9 6 6 6-6"
#define DT_ESSENTIALS_CHEVRON_RIGHT "m9 6 6 6-6 6"

typedef struct dt_lib_essentials_t
{
  GtkWidget *header;    // title and actions; moved into the scopes card's header row when it exists
  GtkWidget *auto_button;
  GtkWidget *bw_button;
  GtkWidget *reset_button;
  GtkWidget *wb_combo;  // white balance setting, mirrors the module's own combobox
  GtkWidget *wb_target;
  GtkWidget *sliders[DT_ESSENTIALS_COUNT];
  GtkWidget *targets[DT_ESSENTIALS_COUNT]; // the module slider each one drives, NULL when absent
  GtkWidget *section_body[DT_ESSENTIALS_SECTIONS];
  GtkWidget *section_arrow[DT_ESSENTIALS_SECTIONS];
  GtkWidget *section_count[DT_ESSENTIALS_SECTIONS];
  gboolean syncing;                        // TRUE while we write our own widgets
  guint wb_idle;                           // pending late re-read of the white balance setting
} dt_lib_essentials_t;

const char *name(dt_lib_module_t *self)
{
  return _("Essentials");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "darkroom", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  // always open: sections fold, the panel does not
  return 0;
}

int position()
{
  // under the histogram (1000), above the module tabs (998)
  return 999;
}

/* The base instance of a module: the one the essentials drive. */
static dt_iop_module_t *_find_module(const char *op)
{
  dt_develop_t *dev = dt_dev_get_global();
  if(IS_NULL_PTR(dev)) return NULL;
  for(const GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)l->data;
    if(m->multi_priority == 0 && !strcmp(m->op, op) && !IS_NULL_PTR(m->gui)) return m;
  }
  return NULL;
}

/* The module's widget for a definition: matched by the params field it is bound to, or by its
 * label when it is a computed control (white balance temperature, tint and setting). */
static GtkWidget *_find_widget(const dt_lib_essentials_def_t *def, const dt_bauhaus_type_t type)
{
  dt_iop_module_t *module = _find_module(def->op);
  if(IS_NULL_PTR(module) || dt_iop_is_hidden(module)) return NULL;

  gpointer field = NULL;
  if(def->param && module->so && module->so->get_f && module->params)
  {
    const dt_introspection_field_t *f = module->so->get_f(def->param);
    if(f) field = (uint8_t *)module->params + f->header.offset;
  }

  gchar *label = NULL;
  if(def->target_label)
  {
    // same transform dt_bauhaus_widget_set_label() applies to the stored label
    label = g_strdup(_(def->target_label));
    dt_capitalize_label(label);
  }

  GtkWidget *found = NULL;
  for(const GList *l = DT_GUI_MODULE(module)->widget_list; l && !found; l = g_list_next(l))
  {
    struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)l->data;
    if(IS_NULL_PTR(w) || w->type != type) continue;
    if((field && w->field == field) || (label && !g_strcmp0(w->label, label))) found = GTK_WIDGET(w);
  }
  dt_free(label);
  return found;
}

static GtkWidget *_find_target(const dt_lib_essentials_def_t *def)
{
  return _find_widget(def, DT_BAUHAUS_SLIDER);
}

static const dt_lib_essentials_def_t _wb_def
    = { "temperature", NULL, N_("settings"), NULL, NULL, DT_ESSENTIALS_COLOR, FALSE };

/* A slider left at its default. Same tolerance as the value color in bauhaus. */
static gboolean _at_default(GtkWidget *slider)
{
  const struct dt_bauhaus_widget_t *w = (const struct dt_bauhaus_widget_t *)slider;
  const float def = w->data.slider.defpos;
  return fabsf(dt_bauhaus_slider_get(slider) - def) <= 1e-6f * fmaxf(1.f, fabsf(def));
}

static void _update_counts(dt_lib_essentials_t *d)
{
  int count[DT_ESSENTIALS_SECTIONS] = { 0 };
  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
    if(gtk_widget_is_sensitive(d->sliders[i]) && !_at_default(d->sliders[i])) count[_defs[i].section]++;

  for(int k = 0; k < DT_ESSENTIALS_SECTIONS; k++)
  {
    if(count[k] == 0)
    {
      gtk_label_set_text(GTK_LABEL(d->section_count[k]), "");
      continue;
    }
    gchar *text = g_strdup_printf(ngettext("%d adjustment", "%d adjustments", count[k]), count[k]);
    gtk_label_set_text(GTK_LABEL(d->section_count[k]), text);
    dt_free(text);
  }
}

/* Mirror the white balance setting combobox: same entries, same selection. */
static void _refresh_wb(dt_lib_essentials_t *d)
{
  GtkWidget *target = d->wb_target = _find_widget(&_wb_def, DT_BAUHAUS_COMBOBOX);
  gtk_widget_set_sensitive(d->wb_combo, !IS_NULL_PTR(target));
  dt_bauhaus_combobox_clear(d->wb_combo);
  if(IS_NULL_PTR(target)) return;
  const int n = dt_bauhaus_combobox_length(target);
  for(int k = 0; k < n; k++)
  {
    const char *entry = dt_bauhaus_combobox_get_entry(target, k);
    dt_bauhaus_combobox_add(d->wb_combo, entry ? entry : "");
  }
  dt_bauhaus_combobox_set(d->wb_combo, dt_bauhaus_combobox_get(target));
}

/* Read every target back into our widgets. */
static void _refresh(dt_lib_module_t *self)
{
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(IS_NULL_PTR(d)) return;

  d->syncing = TRUE;
  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
  {
    GtkWidget *target = _find_target(&_defs[i]);
    d->targets[i] = target;
    gtk_widget_set_sensitive(d->sliders[i], !IS_NULL_PTR(target));
    if(IS_NULL_PTR(target)) continue;
    dt_bauhaus_slider_copy_scale(d->sliders[i], target);
    if(_defs[i].scale100)
    {
      // -100..+100 over the module's soft range: the value stored is the module's, only the
      // number shown (and typed) is scaled
      const float span = fmaxf(fabsf(dt_bauhaus_slider_get_soft_min(target)),
                               fabsf(dt_bauhaus_slider_get_soft_max(target)));
      if(span > 0.f)
      {
        dt_bauhaus_slider_set_factor(d->sliders[i], 100.f / span);
        dt_bauhaus_slider_set_offset(d->sliders[i], 0.f);
        dt_bauhaus_slider_set_digits(d->sliders[i], 0);
        dt_bauhaus_slider_set_format(d->sliders[i], "");
      }
    }
    else if(!strcmp(_defs[i].op, "exposure"))
      dt_bauhaus_slider_set_digits(d->sliders[i], 2); // +0,35 EV reads better than +0,350 EV
    const float value = dt_bauhaus_slider_get(target);
    // A module still at its defaults is "not edited", even when its slider shows a value
    // computed from the picture (white balance shows the as-shot temperature): show it as such.
    dt_iop_module_t *module = _find_module(_defs[i].op);
    if(module && module->params && module->default_params
       && !memcmp(module->params, module->default_params, module->params_size))
      dt_bauhaus_slider_set_default(d->sliders[i], value);
    dt_bauhaus_slider_set(d->sliders[i], value);
  }
  _refresh_wb(d);
  GtkWidget *chroma = _find_target(&_bw_def);
  gtk_widget_set_sensitive(d->bw_button, !IS_NULL_PTR(chroma));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bw_button),
                               chroma && dt_bauhaus_slider_get(chroma) <= -0.999f);
  _update_counts(d);
  d->syncing = FALSE;
}

static void _slider_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(IS_NULL_PTR(d) || d->syncing) return;

  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
  {
    if(d->sliders[i] != widget) continue;
    // the module may have been reloaded since the last refresh: look it up again
    GtkWidget *target = _find_target(&_defs[i]);
    d->targets[i] = target;
    if(target) dt_bauhaus_slider_set(target, dt_bauhaus_slider_get(widget));
    break;
  }
  _update_counts(d);
}

static void _wb_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(IS_NULL_PTR(d) || d->syncing) return;
  GtkWidget *target = _find_widget(&_wb_def, DT_BAUHAUS_COMBOBOX);
  if(target) dt_bauhaus_combobox_set(target, dt_bauhaus_combobox_get(widget));
}

static void _bw_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(IS_NULL_PTR(d) || d->syncing) return;
  GtkWidget *chroma = _find_target(&_bw_def);
  if(chroma) dt_bauhaus_slider_set(chroma, gtk_toggle_button_get_active(button) ? -1.f : 0.f);
}

static void _auto_clicked(GtkButton *button, gpointer user_data)
{
  // the darkroom's autoset (magic wand in the toolbox): runs each module's own auto-adjustment
  GtkWidget *autoset = g_object_get_data(G_OBJECT(dt_gui_main_window()), "lumivell-autoset-button");
  if(autoset) gtk_button_clicked(GTK_BUTTON(autoset));
}

/* The white balance module settles its setting combobox after the history signal we react to
 * (it compares coefficients in its own update): read it once more when GTK is idle. */
static gboolean _wb_idle(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(IS_NULL_PTR(d)) return G_SOURCE_REMOVE;
  d->wb_idle = 0;
  d->syncing = TRUE;
  _refresh_wb(d);
  d->syncing = FALSE;
  return G_SOURCE_REMOVE;
}

static void _develop_changed(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _refresh(self);
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(d && !d->wb_idle) d->wb_idle = g_idle_add(_wb_idle, self);
}

/* Reset a whole module through its own header reset button: the only right reset for controls
 * computed from the picture (white balance temperature and tint have no fixed default value to
 * go back to -- the module's defaults decide, as-shot or camera reference). */
static void _reset_module(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->reset_button)) return;
  if(module->params && module->default_params
     && !memcmp(module->params, module->default_params, module->params_size))
    return; // nothing to reset, and no history item for nothing
  GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
  event->button.button = 1;
  gboolean handled = FALSE;
  g_signal_emit_by_name(module->gui->reset_button, "button-press-event", event, &handled);
  gdk_event_free(event);
}

void gui_reset(dt_lib_module_t *self)
{
  // Reset drives every target back to its default, through the modules, like a manual reset.
  gboolean wb_done = FALSE;
  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
  {
    if(_defs[i].target_label)
    {
      if(!wb_done) _reset_module(_find_module(_defs[i].op));
      wb_done = TRUE;
      continue;
    }
    GtkWidget *target = _find_target(&_defs[i]);
    if(target && !_at_default(target)) dt_bauhaus_slider_reset(target);
  }
  _refresh(self);
}

static void _reset_clicked(GtkWidget *button, gpointer user_data)
{
  gui_reset((dt_lib_module_t *)user_data);
}

/* -- sections ---------------------------------------------------------------------------------- */

static void _section_apply(dt_lib_essentials_t *d, const int k)
{
  const gboolean collapsed = dt_conf_get_bool(_section_conf[k]);
  gtk_widget_set_visible(d->section_body[k], !collapsed);
  dtgtk_button_set_paint(DTGTK_BUTTON(d->section_arrow[k]), dtgtk_cairo_paint_line_icon, 0,
                         collapsed ? DT_ESSENTIALS_CHEVRON_RIGHT : DT_ESSENTIALS_CHEVRON_DOWN);
  gtk_widget_queue_draw(d->section_arrow[k]);
}

static void _section_toggle(dt_lib_module_t *self, const int k)
{
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  dt_conf_set_bool(_section_conf[k], !dt_conf_get_bool(_section_conf[k]));
  _section_apply(d, k);
}

static void _section_arrow_clicked(GtkWidget *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _section_toggle(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "section")));
}

static gboolean _section_header_pressed(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button != 1 || event->type != GDK_BUTTON_PRESS) return FALSE;
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _section_toggle(self, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "section")));
  return TRUE;
}

static GtkWidget *_section_new(dt_lib_module_t *self, const int k, const char *title)
{
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;

  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(section, "essentials-section");

  GtkWidget *header_evb = gtk_event_box_new();
  g_object_set_data(G_OBJECT(header_evb), "section", GINT_TO_POINTER(k));
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_section_header_pressed), self);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(header, "essentials-section-header");
  gtk_container_add(GTK_CONTAINER(header_evb), header);

  d->section_arrow[k] = dtgtk_button_new(dtgtk_cairo_paint_line_icon, 0, DT_ESSENTIALS_CHEVRON_DOWN);
  g_object_set_data(G_OBJECT(d->section_arrow[k]), "section", GINT_TO_POINTER(k));
  dt_gui_add_class(d->section_arrow[k], "essentials-section-arrow");
  g_signal_connect(G_OBJECT(d->section_arrow[k]), "clicked", G_CALLBACK(_section_arrow_clicked), self);
  gtk_box_pack_start(GTK_BOX(header), d->section_arrow[k], FALSE, FALSE, 0);

  GtkWidget *label = gtk_label_new(title);
  dt_gui_add_class(label, "essentials-section-title");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);

  d->section_count[k] = gtk_label_new("");
  dt_gui_add_class(d->section_count[k], "essentials-section-count");
  dt_gui_add_class(d->section_count[k], "dt_monospace");
  gtk_box_pack_end(GTK_BOX(header), d->section_count[k], FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(section), header_evb, FALSE, FALSE, 0);

  d->section_body[k] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(d->section_body[k], "essentials-section-body");
  gtk_widget_set_no_show_all(d->section_body[k], FALSE);
  gtk_box_pack_start(GTK_BOX(section), d->section_body[k], FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), section, FALSE, FALSE, 0);
  return d->section_body[k];
}

/* -- header ------------------------------------------------------------------------------------ */

/* The title and actions go into the scopes card's header row, above the histogram, when the
 * scopes module offers one; otherwise they stay at the top of our own box. */
static void _place_header(dt_lib_module_t *self)
{
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(IS_NULL_PTR(d) || IS_NULL_PTR(d->header)) return;
  GtkWidget *slot = g_object_get_data(G_OBJECT(dt_gui_main_window()), "lumivell-panel-header");
  GtkWidget *parent = gtk_widget_get_parent(d->header);
  if(slot && parent != slot)
  {
    if(parent) gtk_container_remove(GTK_CONTAINER(parent), d->header); // we hold a reference
    gtk_box_pack_start(GTK_BOX(slot), d->header, TRUE, TRUE, 0);
  }
  else if(IS_NULL_PTR(slot) && IS_NULL_PTR(parent))
  {
    gtk_box_pack_start(GTK_BOX(self->widget), d->header, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(self->widget), d->header, 0);
  }
  gtk_widget_show_all(d->header);
  if(slot) gtk_widget_show(slot);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  _place_header(self);
  for(int k = 0; k < DT_ESSENTIALS_SECTIONS; k++) _section_apply((dt_lib_essentials_t *)self->data, k);
  _refresh(self);
}

void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  // the scopes card is also shown outside the darkroom, where our header has nothing to drive
  GtkWidget *slot = g_object_get_data(G_OBJECT(dt_gui_main_window()), "lumivell-panel-header");
  if(slot) gtk_widget_hide(slot);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)calloc(1, sizeof(dt_lib_essentials_t));
  self->data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "essentials");

  // header: title, Auto, B&W, reset
  d->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->header, "essentials-actions");
  g_object_ref_sink(d->header); // it may live in another module's box: keep it ours
  GtkWidget *title = gtk_label_new(_("Adjust"));
  gtk_widget_set_name(title, "panel-title");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->header), title, TRUE, TRUE, 0);

  d->auto_button = gtk_button_new_with_label(_("Auto"));
  gtk_widget_set_tooltip_text(d->auto_button, _("let each module adjust itself to the picture"));
  g_signal_connect(G_OBJECT(d->auto_button), "clicked", G_CALLBACK(_auto_clicked), self);
  d->bw_button = gtk_toggle_button_new_with_label(_("B&W"));
  gtk_widget_set_tooltip_text(d->bw_button, _("black and white: remove all chroma (color balance rgb)"));
  g_signal_connect(G_OBJECT(d->bw_button), "toggled", G_CALLBACK(_bw_toggled), self);
  d->reset_button = dtgtk_button_new(dtgtk_cairo_paint_line_icon, 0, DTGTK_LINE_ICON_RESET);
  gtk_widget_set_name(d->reset_button, "essentials-reset");
  gtk_widget_set_size_request(d->reset_button, DT_PIXEL_APPLY_DPI(28), DT_PIXEL_APPLY_DPI(28));
  gtk_widget_set_valign(d->reset_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(d->reset_button, _("reset the light and color sliders to their defaults"));
  g_signal_connect(G_OBJECT(d->reset_button), "clicked", G_CALLBACK(_reset_clicked), self);
  gtk_box_pack_start(GTK_BOX(d->header), d->auto_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->header), d->bw_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->header), d->reset_button, FALSE, FALSE, 0);

  GtkWidget *bodies[DT_ESSENTIALS_SECTIONS];
  bodies[DT_ESSENTIALS_LIGHT] = _section_new(self, DT_ESSENTIALS_LIGHT, _("Light"));
  bodies[DT_ESSENTIALS_COLOR] = _section_new(self, DT_ESSENTIALS_COLOR, _("Color"));

  // white balance setting (as shot, camera reference, presets...) heads the color section
  d->wb_combo = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(d->wb_combo, N_("White balance"));
  gtk_widget_set_tooltip_text(d->wb_combo, _("white balance setting (white balance module)"));
  g_signal_connect(G_OBJECT(d->wb_combo), "value-changed", G_CALLBACK(_wb_changed), self);
  gtk_box_pack_start(GTK_BOX(bodies[DT_ESSENTIALS_COLOR]), d->wb_combo, FALSE, FALSE, 0);

  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
  {
    const dt_lib_essentials_def_t *def = &_defs[i];
    // placeholder scale until the first refresh copies the module's
    GtkWidget *slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(self),
                                                         -1.f, 1.f, 0.01f, 0.f, 2);
    dt_bauhaus_widget_set_label(slider, def->label);
    gtk_widget_set_tooltip_text(slider, _(def->tooltip));
    g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_slider_changed), self);
    gtk_box_pack_start(GTK_BOX(bodies[def->section]), slider, FALSE, FALSE, 0);
    d->sliders[i] = slider;
  }

  _place_header(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                                  G_CALLBACK(_develop_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_develop_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_develop_changed), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_develop_changed), self);
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)self->data;
  if(d && d->wb_idle) g_source_remove(d->wb_idle);
  if(d && d->header)
  {
    GtkWidget *parent = gtk_widget_get_parent(d->header);
    if(parent) gtk_container_remove(GTK_CONTAINER(parent), d->header);
    g_object_unref(d->header);
  }
  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

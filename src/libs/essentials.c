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
 * The scale (range, step, digits, unit, color stops) is copied from the module's slider, so the
 * two always show the same number.
 */

#include "common/conf.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/application.h"
#include "gui/window_manager.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "widgets/bauhaus.h"
#include "widgets/label.h"
#include "widgets/widget_style.h"

#include <string.h>

DT_MODULE(1)

typedef enum dt_lib_essentials_section_t
{
  DT_ESSENTIALS_LIGHT = 0,
  DT_ESSENTIALS_COLOR = 1,
} dt_lib_essentials_section_t;

typedef struct dt_lib_essentials_def_t
{
  const char *op;           // target module
  const char *param;        // introspection field of the target slider, or NULL
  const char *target_label; // label of the target slider (untranslated) when it has no field
  const char *label;        // our label (untranslated)
  const char *tooltip;      // our tooltip (untranslated)
  dt_lib_essentials_section_t section;
} dt_lib_essentials_def_t;

static const dt_lib_essentials_def_t _defs[] = {
  { "exposure", "exposure", NULL, N_("Exposure"),
    N_("brightness of the whole picture, in EV (exposure module)"), DT_ESSENTIALS_LIGHT },
  { "colorbalancergb", "contrast", NULL, N_("Contrast"),
    N_("separation between darks and lights around middle gray (color balance rgb)"), DT_ESSENTIALS_LIGHT },
  { "toneequal", "highlights", NULL, N_("Highlights"),
    N_("brighten or darken the bright areas (tone equalizer, -2 EV band)"), DT_ESSENTIALS_LIGHT },
  { "toneequal", "blacks", NULL, N_("Shadows"),
    N_("brighten or darken the dark areas (tone equalizer, -5 EV band)"), DT_ESSENTIALS_LIGHT },
  { "toneequal", "whites", NULL, N_("Whites"),
    N_("brighten or darken the brightest tones (tone equalizer, -1 EV band)"), DT_ESSENTIALS_LIGHT },
  { "toneequal", "deep_blacks", NULL, N_("Blacks"),
    N_("brighten or darken the darkest tones (tone equalizer, -6 EV band)"), DT_ESSENTIALS_LIGHT },
  { "temperature", NULL, N_("temperature"), N_("Temperature"),
    N_("make the picture warmer or cooler (white balance)"), DT_ESSENTIALS_COLOR },
  { "temperature", NULL, N_("tint"), N_("Tint"),
    N_("shift colors between magenta and green (white balance)"), DT_ESSENTIALS_COLOR },
  { "colorbalancergb", "vibrance", NULL, N_("Vibrance"),
    N_("strengthen the least saturated colors first (color balance rgb)"), DT_ESSENTIALS_COLOR },
  { "colorbalancergb", "saturation_global", NULL, N_("Saturation"),
    N_("strengthen or mute all colors (color balance rgb)"), DT_ESSENTIALS_COLOR },
};

#define DT_ESSENTIALS_COUNT (sizeof(_defs) / sizeof(_defs[0]))

/* black & white: color balance rgb's global chroma all the way down */
static const dt_lib_essentials_def_t _bw_def
    = { "colorbalancergb", "chroma_global", NULL, NULL, NULL, DT_ESSENTIALS_COLOR };

typedef struct dt_lib_essentials_t
{
  GtkWidget *auto_button;
  GtkWidget *bw_button;
  GtkWidget *sliders[DT_ESSENTIALS_COUNT];
  GtkWidget *targets[DT_ESSENTIALS_COUNT]; // the module slider each one drives, NULL when absent
  gboolean syncing;                        // TRUE while we write our own sliders
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

/* The module's slider for a definition: matched by the params field it is bound to, or by its
 * label when it is a computed control (white balance temperature and tint). */
static GtkWidget *_find_target(const dt_lib_essentials_def_t *def)
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
    if(IS_NULL_PTR(w) || w->type != DT_BAUHAUS_SLIDER) continue;
    if((field && w->field == field) || (label && !g_strcmp0(w->label, label))) found = GTK_WIDGET(w);
  }
  dt_free(label);
  return found;
}

/* Read every target back into our sliders. */
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
    const float value = dt_bauhaus_slider_get(target);
    // A module still at its defaults is "not edited", even when its slider shows a value
    // computed from the picture (white balance shows the as-shot temperature): show it as such.
    dt_iop_module_t *module = _find_module(_defs[i].op);
    if(module && module->params && module->default_params
       && !memcmp(module->params, module->default_params, module->params_size))
      dt_bauhaus_slider_set_default(d->sliders[i], value);
    dt_bauhaus_slider_set(d->sliders[i], value);
  }
  GtkWidget *chroma = _find_target(&_bw_def);
  gtk_widget_set_sensitive(d->bw_button, !IS_NULL_PTR(chroma));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bw_button),
                               chroma && dt_bauhaus_slider_get(chroma) <= -0.999f);
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

static void _develop_changed(gpointer instance, gpointer user_data)
{
  _refresh((dt_lib_module_t *)user_data);
}

void gui_reset(dt_lib_module_t *self)
{
  // Reset drives every target back to its default, through the modules, like a manual reset.
  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
  {
    GtkWidget *target = _find_target(&_defs[i]);
    if(target) dt_bauhaus_slider_reset(target);
  }
  _refresh(self);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  _refresh(self);
}

void init(dt_lib_module_t *self)
{
  // open by default: these are the controls most edits start with
  if(!dt_conf_key_exists("plugins/darkroom/essentials/expanded"))
    dt_conf_set_bool("plugins/darkroom/essentials/expanded", TRUE);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_essentials_t *d = (dt_lib_essentials_t *)calloc(1, sizeof(dt_lib_essentials_t));
  self->data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "essentials");

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(actions, "essentials-actions");
  d->auto_button = gtk_button_new_with_label(_("Auto"));
  gtk_widget_set_tooltip_text(d->auto_button, _("let each module adjust itself to the picture"));
  g_signal_connect(G_OBJECT(d->auto_button), "clicked", G_CALLBACK(_auto_clicked), self);
  d->bw_button = gtk_toggle_button_new_with_label(_("B&W"));
  gtk_widget_set_tooltip_text(d->bw_button, _("black and white: remove all chroma (color balance rgb)"));
  g_signal_connect(G_OBJECT(d->bw_button), "toggled", G_CALLBACK(_bw_toggled), self);
  gtk_box_pack_start(GTK_BOX(actions), d->auto_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(actions), d->bw_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), actions, FALSE, FALSE, 0);

  int last_section = -1;
  for(size_t i = 0; i < DT_ESSENTIALS_COUNT; i++)
  {
    const dt_lib_essentials_def_t *def = &_defs[i];
    if((int)def->section != last_section)
    {
      GtkWidget *title = dt_ui_section_label_new(def->section == DT_ESSENTIALS_LIGHT ? _("Light") : _("Color"));
      gtk_box_pack_start(GTK_BOX(self->widget), title, FALSE, FALSE, 0);
      last_section = def->section;
    }

    // placeholder scale until the first refresh copies the module's
    GtkWidget *slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(self),
                                                         -1.f, 1.f, 0.01f, 0.f, 2);
    dt_bauhaus_widget_set_label(slider, def->label);
    gtk_widget_set_tooltip_text(slider, _(def->tooltip));
    g_signal_connect(G_OBJECT(slider), "value-changed", G_CALLBACK(_slider_changed), self);
    gtk_box_pack_start(GTK_BOX(self->widget), slider, FALSE, FALSE, 0);
    d->sliders[i] = slider;
  }

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
  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

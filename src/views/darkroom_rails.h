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

#ifndef DT_VIEWS_DARKROOM_RAILS_H
#define DT_VIEWS_DARKROOM_RAILS_H

struct dt_develop_t;
struct dt_view_t;

/**
 * The darkroom's icon rails: shortcuts that organize what the side panels already hold.
 *
 * Left rail: one button per left-panel module of the darkroom (navigation, history, masking &
 * blending, duplicates, snapshots, notes, EXIF...). A click opens that module and folds the
 * others away; a second click folds it. Nothing is moved or removed: the modules stay where they
 * are, the rail only decides which one is open.
 *
 * Right rail: tools. "Adjust" goes back to the Essentials and the module list; "Crop" and "Retouch"
 * open and focus those modules; "Masks" opens masking & blending; "Presets" pops up the Styles menu.
 */
void dt_darkroom_rails_enter(struct dt_develop_t *dev, const struct dt_view_t *view);
void dt_darkroom_rails_leave(void);

#endif // DT_VIEWS_DARKROOM_RAILS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

/*
 * Copyright (C) 2026 alaraajavamma
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: alaraajavamma <aki@urheiluaki.fi>
 */

#include <gtk/gtk.h>

#pragma once

G_BEGIN_DECLS

#define PHOSH_TYPE_CAMERA_LOCKSCREEN (phosh_camera_lockscreen_get_type ())
G_DECLARE_FINAL_TYPE (PhoshCameraLockscreen, phosh_camera_lockscreen, PHOSH, CAMERA_LOCKSCREEN, GtkBox)

G_END_DECLS

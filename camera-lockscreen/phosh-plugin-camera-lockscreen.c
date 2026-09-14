/*
 * Copyright (C) 2026 alaraajavamma
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: alaraajavamma <aki@urheiluaki.fi>
 */

#include "camera-lockscreen.h"
#include "phosh-plugin.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

char **g_io_phosh_plugin_camera_lockscreen_query (void);

void
g_io_module_load (GIOModule *module)
{
  g_type_module_use (G_TYPE_MODULE (module));

  g_io_extension_point_implement (PHOSH_PLUGIN_EXTENSION_POINT_LOCKSCREEN_WIDGET,
                                  PHOSH_TYPE_CAMERA_LOCKSCREEN,
                                  PLUGIN_NAME,
                                  10);
}

void
g_io_module_unload (GIOModule *module)
{
  (void) module;
}

char **
g_io_phosh_plugin_camera_lockscreen_query (void)
{
  char *extension_points[] = {PHOSH_PLUGIN_EXTENSION_POINT_LOCKSCREEN_WIDGET, NULL};

  return g_strdupv (extension_points);
}

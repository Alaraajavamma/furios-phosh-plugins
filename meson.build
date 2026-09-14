project('furios-phosh-plugins', 'c',
  version: '1.0.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.56.0',
  default_options: [
    'warning_level=2',
  ],
)

gnome = import('gnome')
i18n = import('i18n')

prefix = get_option('prefix')
datadir = join_paths(prefix, get_option('datadir'))
phosh_plugin_icon_dir = datadir / 'phosh' / 'plugins' / 'icons'

pkgconfig = import('pkgconfig')
phosh_plugins_dep = dependency('phosh-plugins')
plugins_dir = phosh_plugins_dep.get_variable(pkgconfig: 'quick_setting_plugins_dir')
status_icons_plugins_dir = phosh_plugins_dep.get_variable(pkgconfig: 'status_icons_plugins_dir')
lockscreen_plugins_dir = phosh_plugins_dep.get_variable(pkgconfig: 'lockscreen_plugins_dir')

phosh_dep = dependency('libphosh-0.45')
glib_dep = dependency('glib-2.0')
gobject_dep = dependency('gobject-2.0')
gtk_dep = dependency('gtk+-3.0')
gsettings_dep = dependency('gsettings-desktop-schemas', version: '>=42')
gstreamer_dep = dependency('gstreamer-1.0')
gstreamer_video_dep = dependency('gstreamer-video-1.0')

plugin_dep = declare_dependency(
  dependencies: [
    phosh_dep,
    glib_dep,
    gobject_dep,
    gtk_dep,
  ]
)

subdir('airplane-mode-quick-setting')
subdir('camera-privacy-quick-setting')
subdir('microphone-privacy-quick-setting')
subdir('powersaving-quick-setting')
subdir('android-quick-setting')
subdir('weather-status-icon')
subdir('camera-lockscreen')

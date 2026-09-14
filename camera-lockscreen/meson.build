name='camera-lockscreen'

camera_lockscreen_resources = gnome.compile_resources(
  'phosh-plugin-camera-lockscreen-resources',
  'phosh-plugin-camera-lockscreen.gresources.xml',
  c_name: 'phosh_plugin_camera_lockscreen',
)

camera_lockscreen_plugin_sources = files(
  'phosh-plugin-camera-lockscreen.c',
  'camera-lockscreen.c',
)

phosh_camera_lockscreen_plugin = shared_module(
  'phosh-plugin-camera-lockscreen',
  camera_lockscreen_plugin_sources,
  camera_lockscreen_resources,
  c_args : ['-DG_LOG_DOMAIN="phosh-plugin-@0@"'.format(name),
            '-DPLUGIN_NAME="@0@"'.format(name),
           ],
  dependencies: [plugin_dep, gstreamer_dep, gstreamer_video_dep],
  install: true,
  install_dir: lockscreen_plugins_dir,
)

pluginconf = configuration_data()
pluginconf.set('name', name)
pluginconf.set('plugins_dir', lockscreen_plugins_dir)

i18n.merge_file(
  input: configure_file(
    input: name + '.desktop.in.in',
    output: name + '.desktop.in',
    configuration: pluginconf,
  ),
  output: name + '.plugin',
  po_dir: join_paths(meson.project_source_root(), 'po'),
  install: true,
  install_dir: lockscreen_plugins_dir,
  type: 'desktop'
)

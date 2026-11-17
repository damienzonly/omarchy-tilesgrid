project('tilesgrid', 'cpp',
  version: '0.1',
  default_options: ['buildtype=release'],
  meson_version: '>= 1.1'
)

cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-std=c++23')
  add_global_arguments('-std=c++23', language: 'cpp')
elif cpp_compiler.has_argument('-std=c++2b')
  add_global_arguments('-std=c++2b', language: 'cpp')
else
  error('Could not configure current C++ compiler (' + cpp_compiler.get_id() + ' ' + cpp_compiler.version() + ') with required C++ standard (C++23)')
endif

add_project_arguments(
  [
    '-Wno-narrowing', '-fPIC', '--no-gnu-unique'
  ],
  language: 'cpp')

globber = run_command('sh', '-c', 'find src -name "*.cpp" -print', check: true)
src = globber.stdout().strip().split('\n')

incdir = []
dependencies = [
  dependency('pixman-1'),
  dependency('libdrm'),
  dependency('pangocairo'),
  dependency('libinput'),
  dependency('libudev'),
  dependency('wayland-server'),
  dependency('xkbcommon'),
]

if get_option('local_include')
  incdir += include_directories('include')
  incdir += include_directories('include/hyprland/protocols')
else
  dependencies += dependency('hyprland')
endif

shared_module(meson.project_name(), src,
  dependencies: dependencies,
  install: true,
  include_directories: incdir
)

mapping_test = executable(
  'mapping_test',
  'tests/mapping_test.cpp',
  include_directories: include_directories('src')
)
test('mapping', mapping_test)

map_conf_test = executable(
  'map_conf_test',
  ['tests/map_conf_test.cpp', 'src/map.cpp'],
  include_directories: include_directories('src')
)
test('map_conf', map_conf_test)

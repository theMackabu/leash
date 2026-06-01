project('leash', ['c', 'objc'], default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true',
  'warning_level=2'
], subproject_dir: 'vendor')

inc = include_directories('include')

foundation = dependency('appleframeworks', modules: ['Foundation'])
virtualization = dependency('appleframeworks', modules: ['Virtualization'])

zlib_project = subproject(
  'zlib-ng',
  default_options: [
    'zlib-compat=true',
    'tests=disabled',
    'default_library=static',
  ],
) 
zlib_dep = zlib_project.get_variable('zlib_ng_dep')

libcurl = dependency(
  'libcurl',
  default_options: [
    'default_library=static',
    'libz=disabled',
    'openssl=disabled',
    'secure-transport=enabled',
    'ssl-default-backend=secure-transport',
    'tests=disabled',
    'tool=disabled',
    'warning_level=0',
  ],
)

libyaml_project = subproject('libyaml', default_options: ['default_library=static', 'warning_level=0'])
libyaml = libyaml_project.get_variable('yaml_dep')

crprintf_project = subproject('crprintf', default_options: ['default_library=static', 'warning_level=0'])
crprintf = crprintf_project.get_variable('crprintf_dep')

embed_text = find_program('tools/embed-text.py')
default_ubuntu_builder = custom_target(
  'default-ubuntu-builder',
  input: 'builders/ubuntu.yml',
  output: 'default_ubuntu_builder.inc',
  command: [embed_text, '@INPUT@', '@OUTPUT@', 'default_ubuntu_builder'],
)

leash_source_files = [
  'src/builders/build.c',
  'src/builders/download.c',
  'src/builders/nocloud.c',
  'src/control.c',
  'src/main.c',
  'src/run/backend.m',
  'src/run/options.c',
  'src/key.c',
  'src/util.c',
]

leash_sources = [
  default_ubuntu_builder,
] + leash_source_files

format_file_names = [
  'include/leash/builder.h',
  'include/leash/download.h',
  'include/leash/nocloud.h',
  'include/leash/run/backend.h',
  'include/leash/run/options.h',
  'include/leash/key.h',
  'include/leash/util.h',
  'include/leash/control.h',
] + leash_source_files

format_files = []
foreach file_name : format_file_names
  format_files += meson.current_source_dir() / file_name
endforeach

clang_format = find_program('clang-format', required: false)
if clang_format.found()
  run_target(
    'format',
    command: [clang_format, '-i', format_files],
  )
  run_target(
    'format-check',
    command: [clang_format, '--dry-run', '-Werror', format_files],
  )
endif

leash_exe = executable(
  'leash',
  leash_sources,
  include_directories: inc,
  dependencies: [foundation, virtualization, crprintf, libcurl, libyaml, zlib_dep],
  objc_args: ['-fobjc-arc', '-fblocks', '-Wno-nullability-completeness'],
  c_args: ['-D_DARWIN_C_SOURCE', '-Wno-nullability-completeness'],
  install: true,
)

custom_target(
  'sign-leash',
  input: leash_exe,
  output: '.leash-signed.stamp',
  command: [
    'sh', '-c',
    'codesign -f -s - --entitlements "$2" "$1" && touch "$3"',
    'sign-leash',
    '@INPUT@',
    meson.current_source_dir() / 'leash.entitlements',
    '@OUTPUT@',
  ],
  build_by_default: true,
)

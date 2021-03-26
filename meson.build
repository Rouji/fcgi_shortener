project('fcgi_shortener', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3', 'cpp_std=c++17'])

executable(
    'fcgi_shortener',
    'fcgi_shortener.cpp',
    dependencies: [
        dependency('lmdb'),
        dependency('threads'),
        meson.get_compiler('cpp').find_library('fcgi')
    ],
    install : true
)

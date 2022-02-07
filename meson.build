project('mati', 'c',
    default_options: [
        'c_std=gnu99',
        'warning_level=2',
    ]
)

gnome = import('gnome')

glib = dependency('glib-2.0')
gio = dependency('gio-2.0')
gio_os = dependency('gio-unix-2.0')
gstreamer = dependency('gstreamer-1.0')
gstreamer_base = dependency('gstreamer-base-1.0')
#gstreamer_good = dependency('gstreamer-good-1.0')
#gstreamer_bad = dependency('gstreamer-bad-1.0')
gstreamer_video = dependency('gstreamer-video-1.0')

subdir('src')
mati_sources = files(
    'mati-application.c',
    'mati-detector.c',
    'mati-uploader.c',
)

mati_dependencies = [
    glib,
    gio,
    gio_os,
    gstreamer,
    gstreamer_base,
#    gstreamer_good,
#    gstreamer_bad,
    gstreamer_video,
]

mati_core = static_library('mati-core', [ mati_sources ], dependencies: mati_dependencies)

mati = executable('mati',
    'main.c',
    link_with: mati_core,
    dependencies: mati_dependencies,
    install: true,
)
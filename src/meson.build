mati_sources = files(
    'mati-application.c',
    'mati-detector.c',
    'mati-communicator.c',
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

mati_core = static_library('mati-core', [ mati_sources, gdbus_mati_src ], dependencies: mati_dependencies)

mati = executable('mati',
    'main.c',
    link_with: mati_core,
    dependencies: mati_dependencies,
    install: true,
)
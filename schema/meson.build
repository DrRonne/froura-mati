gdbus_mati_src = gnome.gdbus_codegen('froura-mati-dbus-proxy',
    'com.froura.Mati.xml',
    interface_prefix: 'com.froura.mati',
    namespace: 'MatiDbus',
)

install_data('com.froura.Mati.xml',
    install_dir: get_option('datadir') / 'froura-mati',
)
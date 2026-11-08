%define __os_install_post %{nil}
%define _build_id_links none

Name:           goley-revival-linux
Version:        0.1.5
Release:        1%{?dist}
Summary:        Goley Revival için doğal GTK4 + Libadwaita Linux başlatıcısı
License:        GPL-3.0-or-later
URL:            https://github.com/Fuwia/goley-revival-linux
BuildArch:      x86_64

Requires:       gtk4, libadwaita
Recommends:     wine, gamemode, mangohud

%description
Goley Revival için doğal GTK4 + Libadwaita Linux başlatıcısı.
ncrypt ECDSA köprüsü, DXVK ve gizli arka plan oturum yönetimi içerir.

%install
mkdir -p %{buildroot}/usr/bin
mkdir -p %{buildroot}/usr/share/applications
mkdir -p %{buildroot}/usr/share/icons/hicolor/scalable/apps
mkdir -p %{buildroot}/usr/share/metainfo

install -m 755 %{_sourcedir}/goley-revival-linux %{buildroot}/usr/bin/goley-revival-linux
install -m 644 %{_sourcedir}/io.github.fuwia.GoleyRevivalLinux.desktop %{buildroot}/usr/share/applications/
install -m 644 %{_sourcedir}/io.github.fuwia.GoleyRevivalLinux.svg %{buildroot}/usr/share/icons/hicolor/scalable/apps/
install -m 644 %{_sourcedir}/io.github.fuwia.GoleyRevivalLinux.metainfo.xml %{buildroot}/usr/share/metainfo/

%files
/usr/bin/goley-revival-linux
/usr/share/applications/io.github.fuwia.GoleyRevivalLinux.desktop
/usr/share/icons/hicolor/scalable/apps/io.github.fuwia.GoleyRevivalLinux.svg
/usr/share/metainfo/io.github.fuwia.GoleyRevivalLinux.metainfo.xml

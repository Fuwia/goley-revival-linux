#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

VERSION="${1:-$(grep -m1 '^version' Cargo.toml | cut -d '"' -f2)}"
DIST_DIR="$SCRIPT_DIR/dist"
mkdir -p "$DIST_DIR"
rm -rf "$DIST_DIR"/*

echo "======================================================"
echo "  Goley Revival Linux — Çoklu Platform Paketleme v${VERSION}"
echo "======================================================"

# 1. Derleme
if [ ! -f "target/release/goley-revival-linux" ]; then
    echo "[1/6] İkili dosya derleniyor (cargo build --release)..."
    cargo build --release
else
    echo "[1/6] target/release/goley-revival-linux mevcut."
fi

# 2. Taşınabilir Tarball Arşivi (.tar.gz)
echo "[2/6] Taşınabilir Tarball paketi oluşturuluyor (.tar.gz)..."
TAR_TMP="$(mktemp -d)"
mkdir -p "$TAR_TMP/pkg"
cp target/release/goley-revival-linux "$TAR_TMP/pkg/"
cp assets/io.github.fuwia.GoleyRevivalLinux.desktop "$TAR_TMP/pkg/"
cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$TAR_TMP/pkg/"
cp assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml "$TAR_TMP/pkg/"
cp install.sh uninstall.sh LICENSE README.md "$TAR_TMP/pkg/"
tar -czf "$DIST_DIR/goley-revival-linux-x86_64.tar.gz" -C "$TAR_TMP/pkg" .
rm -rf "$TAR_TMP"

# 3. Universal AppImage (.AppImage)
echo "[3/6] Universal AppImage oluşturuluyor (.AppImage)..."
APPDIR="$(mktemp -d)"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$APPDIR/usr/share/metainfo"

cp target/release/goley-revival-linux "$APPDIR/usr/bin/"
cp assets/io.github.fuwia.GoleyRevivalLinux.desktop "$APPDIR/io.github.fuwia.GoleyRevivalLinux.desktop"
cp assets/io.github.fuwia.GoleyRevivalLinux.desktop "$APPDIR/usr/share/applications/"
cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$APPDIR/io.github.fuwia.GoleyRevivalLinux.svg"
cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$APPDIR/usr/share/icons/hicolor/scalable/apps/"
cp assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml "$APPDIR/usr/share/metainfo/"
cp assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml "$APPDIR/usr/share/metainfo/io.github.fuwia.GoleyRevivalLinux.appdata.xml"

# 256x256 PNG .DirIcon
if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 256 -h 256 assets/io.github.fuwia.GoleyRevivalLinux.svg -o "$APPDIR/.DirIcon"
elif command -v convert >/dev/null 2>&1; then
    convert -background none -resize 256x256 assets/io.github.fuwia.GoleyRevivalLinux.svg "$APPDIR/.DirIcon"
else
    cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$APPDIR/.DirIcon"
fi
cp "$APPDIR/.DirIcon" "$APPDIR/usr/share/icons/hicolor/256x256/apps/io.github.fuwia.GoleyRevivalLinux.png"
cp "$APPDIR/.DirIcon" "$APPDIR/io.github.fuwia.GoleyRevivalLinux.png"

cat << 'EOF' > "$APPDIR/AppRun"
#!/bin/sh
SELF=$(readlink -f "$0")
HERE=${SELF%/*}
export PATH="${HERE}/usr/bin:${PATH}"
export XDG_DATA_DIRS="${HERE}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "${HERE}/usr/bin/goley-revival-linux" "$@"
EOF
chmod +x "$APPDIR/AppRun"
chmod -R a+rX "$APPDIR"

AI_TOOL_DIR="$(mktemp -d)"
if [ ! -f "/tmp/appimagetool" ]; then
    curl -sL https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage -o "/tmp/appimagetool"
    chmod +x "/tmp/appimagetool"
fi
(
    cd "$AI_TOOL_DIR"
    /tmp/appimagetool --appimage-extract >/dev/null 2>&1
    ARCH=x86_64 ./squashfs-root/AppRun "$APPDIR" "$DIST_DIR/goley-revival-linux-x86_64.AppImage"
)
rm -rf "$APPDIR" "$AI_TOOL_DIR"

# 4. Debian / Ubuntu Paketi (.deb)
echo "[4/6] Debian / Ubuntu paketi oluşturuluyor (.deb)..."
DEB_TMP="$(mktemp -d)"
mkdir -p "$DEB_TMP/data/usr/bin"
mkdir -p "$DEB_TMP/data/usr/share/applications"
mkdir -p "$DEB_TMP/data/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$DEB_TMP/data/usr/share/metainfo"
mkdir -p "$DEB_TMP/data/usr/share/doc/goley-revival-linux"
mkdir -p "$DEB_TMP/control_dir"

cp target/release/goley-revival-linux "$DEB_TMP/data/usr/bin/"
cp assets/io.github.fuwia.GoleyRevivalLinux.desktop "$DEB_TMP/data/usr/share/applications/"
cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$DEB_TMP/data/usr/share/icons/hicolor/scalable/apps/"
cp assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml "$DEB_TMP/data/usr/share/metainfo/"
cp LICENSE "$DEB_TMP/data/usr/share/doc/goley-revival-linux/copyright"
cp README.md "$DEB_TMP/data/usr/share/doc/goley-revival-linux/"

cat << EOF > "$DEB_TMP/control_dir/control"
Package: goley-revival-linux
Version: ${VERSION}
Section: games
Priority: optional
Architecture: amd64
Maintainer: Fuwia <byfuria93@gmail.com>
Depends: libgtk-4-1, libadwaita-1-0, libc6
Suggests: wine, gamemode, mangohud
Homepage: https://github.com/Fuwia/goley-revival-linux
Description: Goley Revival Linux Baslaticisi
 Goley Revival icin dogal GTK4 + Libadwaita Linux baslaticisi.
 ECDSA ncrypt koprusu, DXVK ve gizli calisma modu icerir.
EOF

echo "2.0" > "$DEB_TMP/debian-binary"
tar -czf "$DEB_TMP/control.tar.gz" -C "$DEB_TMP/control_dir" control
tar -cJf "$DEB_TMP/data.tar.xz" -C "$DEB_TMP/data" usr
(cd "$DEB_TMP" && ar rcs "$DIST_DIR/goley-revival-linux_${VERSION}_amd64.deb" debian-binary control.tar.gz data.tar.xz)
rm -rf "$DEB_TMP"

# 5. Arch Linux Paketi (.pkg.tar.zst)
echo "[5/6] Arch Linux / CachyOS / Manjaro paketi oluşturuluyor (.pkg.tar.zst)..."
ARCH_TMP="$(mktemp -d)"
mkdir -p "$ARCH_TMP/pkg/usr/bin"
mkdir -p "$ARCH_TMP/pkg/usr/share/applications"
mkdir -p "$ARCH_TMP/pkg/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$ARCH_TMP/pkg/usr/share/metainfo"
mkdir -p "$ARCH_TMP/pkg/usr/share/licenses/goley-revival-linux"

cp target/release/goley-revival-linux "$ARCH_TMP/pkg/usr/bin/"
cp assets/io.github.fuwia.GoleyRevivalLinux.desktop "$ARCH_TMP/pkg/usr/share/applications/"
cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$ARCH_TMP/pkg/usr/share/icons/hicolor/scalable/apps/"
cp assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml "$ARCH_TMP/pkg/usr/share/metainfo/"
cp LICENSE "$ARCH_TMP/pkg/usr/share/licenses/goley-revival-linux/"

INST_SIZE=$(du -sb "$ARCH_TMP/pkg/usr" | cut -f1)

cat << EOF > "$ARCH_TMP/pkg/.PKGINFO"
pkgname = goley-revival-linux
pkgbase = goley-revival-linux
pkgver = ${VERSION}-1
pkgdesc = Goley Revival için doğal GTK4 + Libadwaita Linux başlatıcısı (ncrypt ECDSA köprüsü, DXVK ve gizli arka plan modu)
url = https://github.com/Fuwia/goley-revival-linux
builddate = $(date +%s)
packager = Fuwia <byfuria93@gmail.com>
size = ${INST_SIZE}
arch = x86_64
license = GPL-3.0-or-later
depend = gtk4
depend = libadwaita
depend = gcc-libs
depend = glibc
optdepend = wine: Alternatif uyumluluk katmanı
optdepend = umu-launcher: Proton-GE ile en yüksek oyun uyumluluğu için
optdepend = gamemode: Performans optimizasyonu için
optdepend = mangohud: Oyun içi FPS ve donanım göstergesi için
EOF

(
    cd "$ARCH_TMP/pkg"
    tar -cf - .PKGINFO usr | zstd -c -T0 -19 > "$DIST_DIR/goley-revival-linux-${VERSION}-1-x86_64.pkg.tar.zst"
)
rm -rf "$ARCH_TMP"

# 6. Fedora / openSUSE / RHEL Paketi (.rpm)
echo "[6/6] Fedora / openSUSE / RHEL paketi oluşturuluyor (.rpm)..."
RPM_TMP="$(mktemp -d)"
mkdir -p "$RPM_TMP/SOURCES" "$RPM_TMP/SPECS" "$RPM_TMP/BUILD" "$RPM_TMP/RPMS" "$RPM_TMP/SRPMS"
cp target/release/goley-revival-linux "$RPM_TMP/SOURCES/"
cp assets/io.github.fuwia.GoleyRevivalLinux.desktop "$RPM_TMP/SOURCES/"
cp assets/io.github.fuwia.GoleyRevivalLinux.svg "$RPM_TMP/SOURCES/"
cp assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml "$RPM_TMP/SOURCES/"

sed "s/^Version:.*/Version:        ${VERSION}/" packaging/rpm/goley-revival-linux.spec > "$RPM_TMP/SPECS/goley-revival-linux.spec"

if command -v rpmbuild >/dev/null 2>&1; then
    rpmbuild --define "_topdir $RPM_TMP" -bb "$RPM_TMP/SPECS/goley-revival-linux.spec" >/dev/null 2>&1 || true
elif command -v podman >/dev/null 2>&1; then
    podman run --rm -v "$RPM_TMP:/rpmbuild:Z" alpine:latest sh -c \
        "apk add --no-cache rpm bash && rpmbuild --define '_topdir /rpmbuild' -bb /rpmbuild/SPECS/goley-revival-linux.spec" >/dev/null 2>&1 || true
fi

RPM_BUILT=$(find "$RPM_TMP/RPMS" -name "*.rpm" 2>/dev/null | head -n 1 || true)
if [ -n "$RPM_BUILT" ] && [ -f "$RPM_BUILT" ]; then
    cp "$RPM_BUILT" "$DIST_DIR/goley-revival-linux-${VERSION}-1.x86_64.rpm"
fi
rm -rf "$RPM_TMP"

# Checksum oluşturma
echo "--- SHA-256 Doğrulama Özetleri Hesaplanıyor ---"
cd "$DIST_DIR"
for f in *; do
    if [ -f "$f" ] && [[ "$f" != *.sha256 ]] && [[ "$f" != "SHA256SUMS.txt" ]]; then
        sha256sum "$f" > "${f}.sha256"
    fi
done
sha256sum goley-revival-linux* > SHA256SUMS.txt 2>/dev/null || true

echo ""
echo "Tüm paketler hazır! ('dist/' klasöründe):"
ls -lh "$DIST_DIR"

#!/usr/bin/env bash
set -euo pipefail

REPO="Fuwia/goley-revival-linux"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
META_DIR="$HOME/.local/share/metainfo"

echo "======================================================"
echo "  Goley Revival Linux Başlatıcısı — Kurulum Betiği"
echo "======================================================"

mkdir -p "$BIN_DIR" "$APP_DIR" "$ICON_DIR" "$META_DIR"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" 2>/dev/null && pwd || pwd)"

if [ -f "$SCRIPT_DIR/target/release/goley-revival-linux" ] && [ "${1:-}" != "--remote" ]; then
    echo "[1/3] Yerel derlenmiş ikili dosya bulundu, kuruluyor..."
    install -Dm755 "$SCRIPT_DIR/target/release/goley-revival-linux" "$BIN_DIR/goley-revival-linux"
    install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.desktop" "$APP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop"
    install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.svg" "$ICON_DIR/io.github.fuwia.GoleyRevivalLinux.svg"
    install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml" "$META_DIR/io.github.fuwia.GoleyRevivalLinux.metainfo.xml"
elif [ -f "$SCRIPT_DIR/Cargo.toml" ] && [ "${1:-}" = "--build" ]; then
    echo "[1/3] Kaynak koddan derleniyor (cargo build --release)..."
    (cd "$SCRIPT_DIR" && cargo build --release)
    install -Dm755 "$SCRIPT_DIR/target/release/goley-revival-linux" "$BIN_DIR/goley-revival-linux"
    install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.desktop" "$APP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop"
    install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.svg" "$ICON_DIR/io.github.fuwia.GoleyRevivalLinux.svg"
    install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.metainfo.xml" "$META_DIR/io.github.fuwia.GoleyRevivalLinux.metainfo.xml"
else
    TMP_DIR="$(mktemp -d)"
    trap 'rm -rf "$TMP_DIR"' EXIT
    TARBALL_URL="https://github.com/$REPO/releases/latest/download/goley-revival-linux-x86_64.tar.gz"
    echo "[1/3] GitHub Releases üzerinden hazır paket indiriliyor..."
    if curl -fsSL "$TARBALL_URL" -o "$TMP_DIR/release.tar.gz"; then
        tar -xzf "$TMP_DIR/release.tar.gz" -C "$TMP_DIR"
        install -Dm755 "$TMP_DIR/goley-revival-linux" "$BIN_DIR/goley-revival-linux"
        install -Dm644 "$TMP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop" "$APP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop"
        install -Dm644 "$TMP_DIR/io.github.fuwia.GoleyRevivalLinux.svg" "$ICON_DIR/io.github.fuwia.GoleyRevivalLinux.svg"
        [ -f "$TMP_DIR/io.github.fuwia.GoleyRevivalLinux.metainfo.xml" ] && \
            install -Dm644 "$TMP_DIR/io.github.fuwia.GoleyRevivalLinux.metainfo.xml" "$META_DIR/io.github.fuwia.GoleyRevivalLinux.metainfo.xml"
    elif [ -f "$SCRIPT_DIR/Cargo.toml" ]; then
        echo "[!] Hazır sürüm bulunamadı, yerel kaynak koddan derleniyor..."
        (cd "$SCRIPT_DIR" && cargo build --release)
        install -Dm755 "$SCRIPT_DIR/target/release/goley-revival-linux" "$BIN_DIR/goley-revival-linux"
        install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.desktop" "$APP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop"
        install -Dm644 "$SCRIPT_DIR/assets/io.github.fuwia.GoleyRevivalLinux.svg" "$ICON_DIR/io.github.fuwia.GoleyRevivalLinux.svg"
    else
        echo "[!] Hazır sürüm indirilemedi. Git deposu klonlanıp derleniyor..."
        git clone "https://github.com/$REPO.git" "$TMP_DIR/src"
        (cd "$TMP_DIR/src" && cargo build --release)
        install -Dm755 "$TMP_DIR/src/target/release/goley-revival-linux" "$BIN_DIR/goley-revival-linux"
        install -Dm644 "$TMP_DIR/src/assets/io.github.fuwia.GoleyRevivalLinux.desktop" "$APP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop"
        install -Dm644 "$TMP_DIR/src/assets/io.github.fuwia.GoleyRevivalLinux.svg" "$ICON_DIR/io.github.fuwia.GoleyRevivalLinux.svg"
    fi
fi

# Ensure Exec points cleanly if ~/.local/bin isn't in desktop session PATH
sed -i "s|^Exec=.*|Exec=$BIN_DIR/goley-revival-linux|" "$APP_DIR/io.github.fuwia.GoleyRevivalLinux.desktop"

echo "[2/3] Masaüstü veritabanı güncelleniyor..."
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APP_DIR" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

echo "[3/3] Kurulum tamamlandı!"
echo ""
echo "  Çalıştırmak için uygulama menünüzden 'Goley Revival' aratabilir"
echo "  veya terminalden şu komutu çalıştırabilirsiniz:"
echo "      $BIN_DIR/goley-revival-linux"
echo ""

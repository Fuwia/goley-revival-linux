#!/usr/bin/env bash
set -euo pipefail

rm -f "$HOME/.local/bin/goley-revival-linux"
rm -f "$HOME/.local/share/applications/io.github.fuwia.GoleyRevivalLinux.desktop"
rm -f "$HOME/.local/share/icons/hicolor/scalable/apps/io.github.fuwia.GoleyRevivalLinux.svg"
rm -f "$HOME/.local/share/metainfo/io.github.fuwia.GoleyRevivalLinux.metainfo.xml"

echo "Goley Revival Linux Başlatıcısı kaldırıldı."
echo "(Oyun dosyalarınız ve ayarlarınız korunmuştur. Tamamen silmek için ~/.config/goley-revival-linux ve ~/.local/share/goley-revival-linux klasörlerini silebilirsiniz.)"

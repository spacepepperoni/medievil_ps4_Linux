#!/bin/sh
# Installs (or updates) MediEvil (PS4) for Linux/Steam Deck:
#   curl -fsSL https://raw.githubusercontent.com/spacepepperoni/medievil_ps4_Linux/main/install.sh | sh
# The latest release's AppImage goes to ~/Applications, with an application menu entry and a
# desktop icon. Saves and settings live in ~/.local/share/medievil-pc and are never touched.
# `sh install.sh --uninstall` removes the AppImage and the menu entry (saves are kept).
set -eu

repo=spacepepperoni/medievil_ps4_Linux
apps="$HOME/Applications"
appimage="$apps/MediEvil-x86_64.AppImage"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
entry="$data/applications/medievil-linux.desktop"
icon="$data/icons/hicolor/256x256/apps/medievil-linux.png"
desktop=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")

if [ "${1:-}" = --uninstall ]; then
    rm -f "$appimage" "$entry" "$icon" "$desktop/medievil-linux.desktop"
    echo "MediEvil removed. Saves and settings are still in $data/medievil-pc."
    exit 0
fi

[ "$(uname -m)" = x86_64 ] || { echo 'MediEvil needs a 64-bit x86 PC.' >&2; exit 1; }
command -v curl >/dev/null || { echo 'Install curl first.' >&2; exit 1; }

echo 'Finding the latest release...'
url=$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" |
      grep -o '"browser_download_url": *"[^"]*MediEvil-x86_64\.AppImage"' | head -1 | sed 's/.*"\(http[^"]*\)"/\1/')
[ -n "$url" ] || { echo "No release found at https://github.com/$repo/releases" >&2; exit 1; }

mkdir -p "$apps" "$(dirname "$entry")" "$(dirname "$icon")"
echo "Downloading $url"
curl -fL --progress-bar -o "$appimage.part" "$url"
chmod +x "$appimage.part"
mv -f "$appimage.part" "$appimage"
curl -fsSL -o "$icon" "https://raw.githubusercontent.com/$repo/main/launcher/medievil.png" || true

cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=MediEvil
Comment=MediEvil (PS4) for Linux/Steam Deck
Exec="$appimage"
Icon=$icon
Terminal=false
Categories=Game;
EOF
chmod +x "$entry"
if [ -d "$desktop" ]; then
    cp "$entry" "$desktop/medievil-linux.desktop"
    chmod +x "$desktop/medievil-linux.desktop"
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" 2>/dev/null || true

echo
echo "Installed: $appimage"
echo 'Open MediEvil from the application menu or the desktop icon and choose your game folder'
echo '(or .pkg) on the Game page.'
if grep -q '^ID=steamos' /etc/os-release 2>/dev/null; then
    echo 'Then press Advanced > Add to Steam. Back in Gaming Mode, MediEvil is in your library under Non-Steam.'
else
    echo 'Then press Play (or Advanced > Add to Steam to play from your Steam library).'
fi

#!/usr/bin/env bash
# Installs the extracted Explosion Simulator package into the per-user XDG
# directories. Run from the package directory with: bash install.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
prefix="$data_home/ExplosionSim"
apps="$data_home/applications"
icons="$data_home/icons/hicolor/256x256/apps"

mkdir -p "$prefix" "$apps" "$icons"
cp "$here/ExplosionSim" "$prefix/"
cp "$here/ExplosionSimUpdater" "$prefix/"
chmod +x "$prefix/ExplosionSim" "$prefix/ExplosionSimUpdater"
cp -r "$here/assets" "$prefix/"
for file in README.md LICENSE CHANGELOG.md README-linux.txt; do
    if [ -f "$here/$file" ]; then
        cp "$here/$file" "$prefix/"
    fi
done
cp "$here/explosionsim.desktop" "$apps/"
cp "$here/explosionsim.png" "$icons/"

# Point the desktop entry at the installed binary.
sed -i "s|^Exec=.*|Exec=$prefix/ExplosionSim|" \
    "$apps/explosionsim.desktop"

printf 'Installed to %s\n' "$prefix"
printf 'Desktop entry: %s\n' "$apps/explosionsim.desktop"
printf 'Run: %s/ExplosionSim\n' "$prefix"

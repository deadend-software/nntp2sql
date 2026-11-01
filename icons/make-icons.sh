#!/bin/sh
# Regenerates every icon file for both programs from the SVGs in hicolor/scalable/apps/:
#   isotope-viewer.svg          the article and file browser (newspaper)
#   hydrogen-group-viewer.svg   the newsgroup list (group hierarchy with a hydrogen atom)
# Needs: rsvg-convert (librsvg), python3; and iconutil (macOS only) for the .icns.
set -e
cd "$(dirname "$0")"
for name in isotope-viewer hydrogen-group-viewer; do
    svg=hicolor/scalable/apps/$name.svg
    for size in 16 22 24 32 48 64 128 256 512; do
        mkdir -p "hicolor/${size}x${size}/apps"
        rsvg-convert -w "$size" -h "$size" "$svg" -o "hicolor/${size}x${size}/apps/$name.png"
    done

    # Windows: an .ico that embeds PNG images (supported since Windows Vista), plus the resource script.
    NAME=$name python3 - <<'PY'
import os, struct
name = os.environ["NAME"]
sizes = [16, 32, 48, 64, 128, 256]
images = [open(f"hicolor/{s}x{s}/apps/{name}.png", "rb").read() for s in sizes]
header = struct.pack("<HHH", 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
entries = b""
for s, data in zip(sizes, images):
    entries += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
open(f"{name}.ico", "wb").write(header + entries + b"".join(images))
open(f"{name}.rc", "w").write(
    "/* Windows resource script: gives the .exe its icon in Explorer and the taskbar.\n"
    " * Compiled by windres when the programs are built for Windows (see Makefile.am). */\n"
    f'IDI_ICON1 ICON "{name}.ico"\n')
PY

    # macOS: an .icns (used by an .app bundle; the programs also set the Dock icon themselves while running).
    if command -v iconutil >/dev/null 2>&1; then
        set_dir=$(mktemp -d)/$name.iconset
        mkdir -p "$set_dir"
        for pair in 16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 256:128x128@2x 256:256x256 512:256x256@2x 512:512x512; do
            px=${pair%%:*}; label=${pair#*:}
            rsvg-convert -w "$px" -h "$px" "$svg" -o "$set_dir/icon_${label}.png"
        done
        rsvg-convert -w 1024 -h 1024 "$svg" -o "$set_dir/icon_512x512@2x.png"
        iconutil -c icns "$set_dir" -o "$name.icns"
    fi
done
echo "icons regenerated"

#!/bin/bash
# ESP32tRNS Release: validate, tag FIRST, then build
set -e
export PATH="$HOME/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION=$(cat "$SCRIPT_DIR/VERSION" | tr -d '[:space:]')

# Validate: D.D.D.D or D.D.D.D-suffix or D.D.D.D-suffix0
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+(-[a-z]+[0-9]*)?$ ]] || { echo "Bad version: $VERSION"; exit 1; }

# Use version as-is (no дата-суффиксов)
FULL="${VERSION}"

echo "Version: $FULL"

# Generate version.h
cat > "$SCRIPT_DIR/ESP32tRNS/version.h" << EOF
// ВРУЧНУЮ НЕ ПРАВИТЬ!, создается автоматом при запуске ./release.sh
#ifndef VERSION_H
#define VERSION_H
#define FIRMWARE_VERSION "$FULL"
#endif
EOF

# Build
if command -v arduino-cli &>/dev/null; then
  mkdir -p "$SCRIPT_DIR/build"
  
  # TinyUF2 bootloader установлен — используем его partition scheme
  FQBN="esp32:esp32:lolin_s2_mini:CDCOnBoot=default"
  
  echo "Compiling..."
  # Используем partitions.csv из скетча (совместимо с TinyUF2 разметкой)
  # Увеличиваем лимит размера прошивки под ota_0 (0x2c0000 = 2883584 bytes)
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-property upload.maximum_size=2883584 \
    --output-dir "$SCRIPT_DIR/build" \
    "$SCRIPT_DIR/ESP32tRNS"
  
  # Переименовываем .bin
  mv "$SCRIPT_DIR/build/ESP32tRNS.ino.bin" "$SCRIPT_DIR/build/firmware-${FULL}.bin"
  
  # Конвертируем в UF2 (ESP32-S2 family: 0xbfdd4eee, base address: 0x0 для TinyUF2)
  echo "Converting to UF2..."
  python3 "$SCRIPT_DIR/tinyuf2/uf2conv.py" \
    -f 0xbfdd4eee \
    -b 0x0 \
    -c "$SCRIPT_DIR/build/firmware-${FULL}.bin" \
    -o "$SCRIPT_DIR/build/firmware-${FULL}.uf2"
  
  # Копии для удобства
  cp "$SCRIPT_DIR/build/firmware-${FULL}.uf2" "$SCRIPT_DIR/build/firmware.uf2"
  cp "$SCRIPT_DIR/build/firmware-${FULL}.bin" "$SCRIPT_DIR/build/firmware.bin"
  
  echo ""
  echo "✓ Built: build/firmware-${FULL}.uf2"
  echo ""
  echo "To flash: copy firmware.uf2 to S2MINIBOOT drive"
fi

# === СНАЧАЛА БИЛД, ПОТОМ КОММИТ/ТЭГ ===
if [[ "$1" != "--no-tag" ]] && git rev-parse --is-inside-work-tree &>/dev/null; then
  TAG="v${VERSION}"
  git tag -l | grep -q "^${TAG}$" && { echo "Tag $TAG exists locally!"; exit 1; }
  git ls-remote --tags origin 2>/dev/null | grep -q "refs/tags/${TAG}$" && { echo "Tag $TAG exists on remote!"; exit 1; }

  git add "$SCRIPT_DIR/ESP32tRNS/version.h"
  git diff --cached --quiet || git commit -m "chore: v${FULL}"
  git tag -a "$TAG" -m "Release $FULL"
  git push origin "$TAG"
  echo "Tagged: $TAG"
fi

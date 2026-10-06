#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_DIR="${ROOT_DIR}/esp-idf"

# Check out exactly what the submodule pins: a commit on upstream ESP-IDF master,
# which is the first ESP-IDF with ESP32-S31 support. FS-only USB host mode
# (USB_DWC_FSLS_ONLY in CMakeLists.txt) is implemented by the adafruit/esp-usb
# fork pulled in via main/idf_component.yml, so no ESP-IDF patches are needed.
git -C "${ROOT_DIR}" submodule update --init esp-idf

git -C "${IDF_DIR}" submodule update --init --recursive --depth 1

"${IDF_DIR}/install.sh" esp32p4 esp32s3 esp32s31

echo
echo "ESP-IDF $(git -C "${IDF_DIR}" describe --tags --always) is installed."
echo "Run: source scripts/idf-env.sh"

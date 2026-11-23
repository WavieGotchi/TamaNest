#!/bin/sh
# Copies the current build into web/firmware/ and stamps the version into manifest.json.
# Usage (after idf.py build):  sh web/make_release.sh
# The parts are written separately (not one merged image), so updating through the web page keeps the
# settings (NVS at 0x9000) and the ROM store; "erase" on a first install wipes everything.
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^set(PROJECT_VER "\(.*\)").*/\1/p' CMakeLists.txt)
mkdir -p web/firmware
cp build/bootloader/bootloader.bin web/firmware/bootloader.bin
cp build/partition_table/partition-table.bin web/firmware/partition-table.bin
cp build/ota_data_initial.bin web/firmware/ota_data_initial.bin
cp build/tamanest.bin web/firmware/tamanest.bin
# Sign it (2026-11-23): devices refuse Wi-Fi/SD updates without a valid signature. Needs the private key (kept off
# the repo, see web/sign_update.py) and Python's "cryptography" (in the ESP-IDF Python).
PY=python3
$PY -c "import cryptography" 2>/dev/null || PY="$HOME/.espressif/tools/python/v6.1/venv/bin/python"
$PY web/sign_update.py web/firmware/tamanest.bin || { echo "NOT SIGNED - stopping (an unsigned build must not be published)"; exit 1; }
cp web/firmware/tamanest.bin "web/firmware/tamanest-$VER.bin"
sed -i '' "s/\"version\": \"[^\"]*\"/\"version\": \"$VER\"/" web/manifest.json
# Cache-busting (2026-11-23): browsers (and GitHub Pages' 10-minute cache) can hand ESP Web Tools an OLD firmware file
# when the file names stay the same - an install then "completes" with the previous build. Every run stamps a new
# build id onto the manifest link and the part links, so each build has new addresses. (The device's own online
# update reads firmware/tamanest.bin directly and is not affected.)
BUILD=$(date +%Y%m%d%H%M%S)
python3 - "$BUILD" <<'PYEOF'
import re, sys
b = sys.argv[1]
m = open('web/manifest.json').read()
m = re.sub(r'("path": "firmware/[^"?]+)(\?b=\d+)?"', lambda x: x.group(1) + '?b=' + b + '"', m)
open('web/manifest.json', 'w').write(m)
h = open('web/index.html').read()
h = re.sub(r'manifest="manifest\.json(\?b=\d+)?"', 'manifest="manifest.json?b=' + b + '"', h)
open('web/index.html', 'w').write(h)
PYEOF
grep -q 'UPDATE_URL_DEFAULT ""' main/main.c && echo "NOTE: UPDATE_URL_DEFAULT in main/main.c is empty - boards cannot check online (see RELEASE.md)"
echo "web/ updated to TamaNest $VER (build $BUILD) (also the file for SD:/UPDATE/TAMANEST.BIN: web/firmware/tamanest.bin)"

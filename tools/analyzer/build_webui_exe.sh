#!/usr/bin/env bash
# Build a single self-contained executable for the analysis web UI.
# Note: PyInstaller does NOT cross-compile; this produces a binary for the OS it runs on.
# For a Windows .exe run build_webui_exe.bat on Windows.
# Output: tools/analyzer/dist/hldsmon-webui
set -euo pipefail
cd "$(dirname "$0")"
PY="${PYTHON:-python3}"

echo "[1/2] Installing/updating PyInstaller..."
"$PY" -m pip install --user --upgrade pyinstaller

echo "[2/2] Building hldsmon-webui..."
"$PY" -m PyInstaller --noconfirm --clean --onefile --name hldsmon-webui \
  --add-data "webui/index.html:webui" \
  --hidden-import hldsmon \
  webui.py

echo
echo "Built: $(pwd)/dist/hldsmon-webui"

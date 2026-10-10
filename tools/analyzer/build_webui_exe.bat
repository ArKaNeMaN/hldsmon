@echo off
rem Build a single self-contained Windows executable for the analysis web UI.
rem Output: tools\analyzer\dist\hldsmon-webui.exe
rem Requires Python 3 on Windows (py launcher or python on PATH). Pure stdlib app.
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if not errorlevel 1 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo [1/2] Installing/updating PyInstaller...
%PY% -m pip install --user --upgrade pyinstaller
if errorlevel 1 goto :err

echo [2/2] Building hldsmon-webui.exe...
%PY% -m PyInstaller --noconfirm --clean --onefile --name hldsmon-webui ^
  --add-data "webui\index.html;webui" ^
  --hidden-import hldsmon ^
  webui.py
if errorlevel 1 goto :err

echo.
echo Built: %~dp0dist\hldsmon-webui.exe
echo Run it and open the printed URL (it also opens the browser with --open).
pause
goto :eof

:err
echo.
echo Build FAILED. Make sure Python 3 is installed and on PATH.
pause

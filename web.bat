@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if not errorlevel 1 (
  py -3 "tools\analyzer\webui.py" --open
  goto end
)

where python >nul 2>nul
if not errorlevel 1 (
  python "tools\analyzer\webui.py" --open
  goto end
)

echo.
echo Python 3 not found.
echo Install Python 3 from https://www.python.org/ and re-run this file,
echo or run the web UI from WSL/Linux with:  make web
echo.
pause

:end
endlocal

@echo off
REM ============================================================
REM  run_legacy.bat — Smart Traffic Light Legacy (Python-only)
REM
REM  Launches the original Python FastAPI backend on port 8000
REM  and then the Python vision pipeline against it. Use this
REM  when you need to fall back from the hybrid C++/Python stack.
REM
REM  Usage:
REM    scripts\run_legacy.bat
REM    scripts\run_legacy.bat --camera
REM ============================================================

setlocal

set "ROOT=%~dp0.."
pushd "%ROOT%"

if "%PUBLIC_PORT%"==""    set "PUBLIC_PORT=8000"
if "%STATE_ENDPOINT%"=="" set "STATE_ENDPOINT=http://127.0.0.1:%PUBLIC_PORT%/state"

REM ---- Parse args -------------------------------------------------
set "USE_CAMERA="
:argloop
if "%~1"=="" goto args_done
if /I "%~1"=="--camera" set "USE_CAMERA=--camera"
shift
goto argloop
:args_done

if exist ".venv\Scripts\activate.bat" call ".venv\Scripts\activate.bat"

echo [run_legacy] Starting Python FastAPI on port %PUBLIC_PORT% ...
start "fastapi_server" /B python -m uvicorn python.server.app:app --host 0.0.0.0 --port %PUBLIC_PORT%

set "HEALTH_URL=http://127.0.0.1:%PUBLIC_PORT%/health"
echo [run_legacy] Waiting for %HEALTH_URL% ...
set /a WAITED=0
:wait_health
powershell -NoProfile -Command "try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 1 '%HEALTH_URL%').StatusCode } catch { 0 }" > "%TEMP%\_traffic_health.txt" 2>nul
set /p HSTATUS=<"%TEMP%\_traffic_health.txt"
del "%TEMP%\_traffic_health.txt" 2>nul
if "%HSTATUS%"=="200" goto health_ok
set /a WAITED+=1
if %WAITED% GEQ 30 (
    echo [run_legacy] ERROR: FastAPI did not become healthy within 30s.
    popd
    exit /b 2
)
ping -n 2 127.0.0.1 >nul
goto wait_health
:health_ok
echo [run_legacy] FastAPI is healthy.

echo [run_legacy] STATE_ENDPOINT=%STATE_ENDPOINT%
echo [run_legacy] Launching python\auto_launcher.py %USE_CAMERA% ...
pushd python
python auto_launcher.py %USE_CAMERA%
set "PY_EXIT=%ERRORLEVEL%"
popd

echo [run_legacy] Vision exited (code %PY_EXIT%). Stopping FastAPI ...
REM kill uvicorn workers (best effort)
for /f "tokens=2" %%P in ('tasklist /FI "WINDOWTITLE eq fastapi_server*" /NH 2^>nul ^| findstr /I "python"') do taskkill /F /PID %%P >nul 2>&1
taskkill /F /FI "WINDOWTITLE eq fastapi_server*" >nul 2>&1

popd
endlocal
exit /b %PY_EXIT%

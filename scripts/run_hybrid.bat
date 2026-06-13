@echo off
REM ============================================================
REM  run_hybrid.bat — Smart Traffic Light Hybrid Stack
REM
REM  Launches the new C++ backend on port 8000 (HTTP + WebSocket
REM  via the unified Router) and then the Python vision pipeline,
REM  which posts state to the C++ server via $STATE_ENDPOINT.
REM
REM  Usage:
REM    scripts\run_hybrid.bat           - run with simulated vision
REM    scripts\run_hybrid.bat --camera  - run with real camera input
REM    scripts\run_hybrid.bat --greedy  - use GreedyAgingController
REM                                       on the standalone controller exe
REM ============================================================

setlocal

set "ROOT=%~dp0.."
pushd "%ROOT%"

REM ---- Configuration ----------------------------------------------
if "%PUBLIC_PORT%"==""    set "PUBLIC_PORT=8000"
if "%STATE_ENDPOINT%"=="" set "STATE_ENDPOINT=http://127.0.0.1:%PUBLIC_PORT%/state"

set "SERVER_EXE=cpp\build\Release\traffic_server.exe"
set "CONTROLLER_EXE=cpp\build\Release\smart_traffic_controller.exe"

if not exist "%SERVER_EXE%" (
    echo [run_hybrid] ERROR: "%SERVER_EXE%" not found.
    echo                       Build it first:
    echo                         cmake --build cpp\build --config Release --target traffic_server
    popd
    exit /b 1
)

REM ---- Parse args -------------------------------------------------
set "USE_CAMERA="
set "USE_GREEDY="
:argloop
if "%~1"=="" goto args_done
if /I "%~1"=="--camera" set "USE_CAMERA=--camera"
if /I "%~1"=="--greedy" set "USE_GREEDY=--greedy"
shift
goto argloop
:args_done

echo [run_hybrid] Starting C++ traffic_server on port %PUBLIC_PORT% ...
start "traffic_server" /B "%SERVER_EXE%" %PUBLIC_PORT%

REM ---- Wait until /health returns 200 ----------------------------
set "HEALTH_URL=http://127.0.0.1:%PUBLIC_PORT%/health"
echo [run_hybrid] Waiting for %HEALTH_URL% ...
set /a WAITED=0
:wait_health
powershell -NoProfile -Command "try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 1 '%HEALTH_URL%').StatusCode } catch { 0 }" > "%TEMP%\_traffic_health.txt" 2>nul
set /p HSTATUS=<"%TEMP%\_traffic_health.txt"
del "%TEMP%\_traffic_health.txt" 2>nul
if "%HSTATUS%"=="200" goto health_ok
set /a WAITED+=1
if %WAITED% GEQ 30 (
    echo [run_hybrid] ERROR: traffic_server did not become healthy within 30s.
    popd
    exit /b 2
)
ping -n 2 127.0.0.1 >nul
goto wait_health
:health_ok
echo [run_hybrid] traffic_server is healthy.

REM ---- Optionally start the standalone smart_traffic_controller --
if exist "%CONTROLLER_EXE%" (
    echo [run_hybrid] Starting smart_traffic_controller --server 127.0.0.1 %PUBLIC_PORT% %USE_GREEDY% ...
    start "smart_traffic_controller" /B "%CONTROLLER_EXE%" --server 127.0.0.1 %PUBLIC_PORT% %USE_GREEDY%
) else (
    echo [run_hybrid] (smart_traffic_controller.exe not present — skipping)
)

REM ---- Activate venv and run Python vision -----------------------
if exist ".venv\Scripts\activate.bat" call ".venv\Scripts\activate.bat"

echo [run_hybrid] STATE_ENDPOINT=%STATE_ENDPOINT%
echo [run_hybrid] Launching python\auto_launcher.py %USE_CAMERA% ...
pushd python
python auto_launcher.py %USE_CAMERA%
set "PY_EXIT=%ERRORLEVEL%"
popd

REM ---- Cleanup ----------------------------------------------------
echo [run_hybrid] Vision exited (code %PY_EXIT%). Stopping C++ processes ...
taskkill /F /IM traffic_server.exe >nul 2>&1
taskkill /F /IM smart_traffic_controller.exe >nul 2>&1

popd
endlocal
exit /b %PY_EXIT%

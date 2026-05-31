"""
run_e2e.py
----------
One-command launcher for end-to-end flow:
Vision/Simulation -> FastAPI Server -> C++ Controller -> React Dashboard (optional)

Usage examples:
  python python/run_e2e.py
  python python/run_e2e.py --camera
  python python/run_e2e.py --with-client
  python python/run_e2e.py --skip-vision
"""

from __future__ import annotations

import argparse
import os
import socket
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import List, Dict, Any


ROOT = Path(__file__).resolve().parents[1]
PYTHON_DIR = ROOT / "python"
CPP_BUILD = ROOT / "cpp" / "build"
CLIENT_DIR = ROOT / "client"


def find_cpp_controller_exe() -> Path:
    candidates = [
        CPP_BUILD / "Debug" / "smart_traffic_controller.exe",
        CPP_BUILD / "Release" / "smart_traffic_controller.exe",
        CPP_BUILD / "smart_traffic_controller.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate

    raise FileNotFoundError(
        "C++ controller executable not found. Build it first (cmake --build cpp/build --config Debug)."
    )


def wait_for_health(url: str, timeout_sec: float = 25.0) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=2.5) as response:
                if response.status == 200:
                    return True
        except Exception:
            time.sleep(0.5)
    return False


def is_port_available(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.5)
        return sock.connect_ex((host, port)) != 0


def start_process(cmd: List[str], cwd: Path, name: str, env: Dict[str, str] | None = None) -> subprocess.Popen:
    print(f"[E2E] Starting {name}: {' '.join(cmd)}")
    return subprocess.Popen(
        cmd,
        cwd=str(cwd),
        env=env,
        creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0),
    )


def make_proc_spec(
    name: str,
    cmd: List[str],
    cwd: Path,
    restart_on_clean_exit: bool = False,
    env: Dict[str, str] | None = None,
) -> Dict[str, Any]:
    return {
        "name": name,
        "cmd": cmd,
        "cwd": cwd,
        "restart_on_clean_exit": restart_on_clean_exit,
        "env": env,
        "process": None,
    }


def launch_spec(spec: Dict[str, Any]) -> None:
    spec["process"] = start_process(
        spec["cmd"],
        spec["cwd"],
        spec["name"],
        spec.get("env"),
    )


def stop_process(proc: subprocess.Popen, name: str) -> None:
    if proc.poll() is not None:
        return

    print(f"[E2E] Stopping {name}...")
    try:
        if os.name == "nt":
            proc.send_signal(signal.CTRL_BREAK_EVENT)
            time.sleep(1.0)
        else:
            proc.terminate()
    except Exception:
        pass

    if proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            pass

    if proc.poll() is None:
        try:
            proc.kill()
        except Exception:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Run full Smart Traffic E2E stack")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--camera", action="store_true", help="Use real camera mode in auto_launcher")
    parser.add_argument("--skip-vision", action="store_true", help="Skip auto_launcher process")
    parser.add_argument("--with-client", action="store_true", help="Start React dashboard (npm start)")
    args = parser.parse_args()

    server_url = f"http://{args.host}:{args.port}"
    health_url = f"{server_url}/health"
    state_url = f"{server_url}/state"

    process_specs: List[Dict[str, Any]] = []
    run_env = os.environ.copy()
    run_env["TRAFFIC_USE_REAL_HARDWARE"] = "true" if args.camera else "false"

    try:
        if not is_port_available(args.host, args.port):
            print(
                f"[E2E] ERROR: {args.host}:{args.port} is already in use. "
                f"Stop the process using this port or run with --port <other_port>."
            )
            return 1

        # 1) FastAPI server
        server_cmd = [
            sys.executable,
            "-m",
            "uvicorn",
            "server.app:app",
            "--app-dir",
            str(PYTHON_DIR),
            "--host",
            args.host,
            "--port",
            str(args.port),
        ]
        server_spec = make_proc_spec(
            "FastAPI server",
            server_cmd,
            ROOT,
            restart_on_clean_exit=False,
            env=run_env,
        )
        launch_spec(server_spec)
        process_specs.append(server_spec)

        if not wait_for_health(health_url, timeout_sec=25.0):
            print(f"[E2E] ERROR: server health check failed: {health_url}")
            return 1

        print(f"[E2E] Server is healthy at {server_url}")
        
        # Check configuration
        try:
            config_response = urllib.request.urlopen(f"{server_url}/config", timeout=5)
            config_data = config_response.read().decode('utf-8')
            print(f"[E2E] ✓ Configuration loaded successfully")
        except Exception as e:
            print(f"[E2E] WARNING: Could not load config: {e}")

        # 2) C++ controller connected mode
        cpp_exe = find_cpp_controller_exe()
        controller_cmd = [str(cpp_exe), "--server", args.host, str(args.port)]
        controller_spec = make_proc_spec(
            "C++ controller",
            controller_cmd,
            cpp_exe.parent,
            restart_on_clean_exit=True,
            env=run_env,
        )
        launch_spec(controller_spec)
        process_specs.append(controller_spec)

        # 3) Vision/Simulation feeders
        if not args.skip_vision:
            launcher_cmd = [sys.executable, str(PYTHON_DIR / "auto_launcher.py"), "--server", state_url]
            if args.camera:
                launcher_cmd.append("--camera")

            launcher_spec = make_proc_spec(
                "Vision/Simulation auto launcher",
                launcher_cmd,
                ROOT,
                restart_on_clean_exit=False,
                env=run_env,
            )
            launch_spec(launcher_spec)
            process_specs.append(launcher_spec)

        # 4) Dashboard (optional)
        if args.with_client:
            if os.name == "nt":
                client_cmd = ["cmd", "/c", "npm", "start"]
            else:
                client_cmd = ["npm", "start"]

            client_spec = make_proc_spec(
                "React dashboard",
                client_cmd,
                CLIENT_DIR,
                restart_on_clean_exit=False,
                env=run_env,
            )
            launch_spec(client_spec)
            process_specs.append(client_spec)

        print("\n[E2E] Stack is running.")
        print(f"[E2E] Health: {health_url}")
        print(f"[E2E] API docs: {server_url}/docs")
        if args.with_client:
            print("[E2E] Dashboard: http://127.0.0.1:3000")
        print(f"[E2E] Hardware mode: {'REAL' if args.camera else 'SIMULATION'}")
        print("[E2E] Press Ctrl+C to stop all processes.")

        while True:
            time.sleep(1.0)
            for spec in process_specs:
                proc = spec["process"]
                if proc is None:
                    continue
                if proc.poll() is None:
                    continue

                code = proc.returncode
                name = spec["name"]
                if code == 0 and spec.get("restart_on_clean_exit", False):
                    print(f"[E2E] {name} exited cleanly. Restarting...")
                    launch_spec(spec)
                    continue

                print(f"[E2E] Process exited unexpectedly: {name} (code={code})")
                return 1

    except KeyboardInterrupt:
        print("\n[E2E] Shutdown requested.")
        return 0
    except Exception as exc:
        print(f"[E2E] ERROR: {exc}")
        return 1
    finally:
        for spec in reversed(process_specs):
            proc = spec.get("process")
            if proc is not None:
                stop_process(proc, spec["name"])


if __name__ == "__main__":
    raise SystemExit(main())

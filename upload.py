#!/usr/bin/env python3
"""
Compile & Upload Tool for ESP32 Audio Player (ESP-IDF)
Adapted from haptic/upload.py for full build, reset toggle, multi-attempt flash, and serial monitoring.
"""

import sys
import os
import time
import json
import subprocess
import serial
import serial.tools.list_ports

# ESP-IDF toolchain paths on this system
IDF_PATH = r"C:\Espressif\frameworks\esp-idf-v5.3.1"
IDF_TOOLS_PATH = r"C:\Users\rakib\.espressif"
IDF_PYTHON = r"C:\Users\rakib\.espressif\python_env\idf5.3_py3.14_env\Scripts\python.exe"
NINJA_DIR = r"C:\Espressif\tools\ninja\1.11.1"
CMAKE_DIR = r"C:\Espressif\tools\cmake\3.24.0\bin"
XTENSA_DIR = r"C:\Espressif\tools\xtensa-esp-elf\esp-13.2.0_20240530\xtensa-esp-elf\bin"
IDF_TOOLS_DIR = os.path.join(IDF_PATH, "tools")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# Check if running from root or ESP_Audio_Player folder
if os.path.exists(os.path.join(SCRIPT_DIR, "ESP_Audio_Player")):
    PROJECT_DIR = os.path.join(SCRIPT_DIR, "ESP_Audio_Player")
else:
    PROJECT_DIR = SCRIPT_DIR

BUILD_DIR = os.path.join(PROJECT_DIR, "build")
FLASHER_ARGS_PATH = os.path.join(BUILD_DIR, "flasher_args.json")

def setup_environment():
    env = os.environ.copy()
    env["IDF_PATH"] = IDF_PATH
    env["IDF_TOOLS_PATH"] = IDF_TOOLS_PATH
    env["IDF_PYTHON_ENV_PATH"] = r"C:\Users\rakib\.espressif\python_env\idf5.3_py3.14_env"
    
    paths_to_add = [
        r"C:\Users\rakib\.espressif\python_env\idf5.3_py3.14_env\Scripts",
        NINJA_DIR,
        CMAKE_DIR,
        XTENSA_DIR,
        IDF_TOOLS_DIR,
    ]
    env["PATH"] = os.pathsep.join(paths_to_add) + os.pathsep + env.get("PATH", "")
    return env

def get_ports():
    return [p.device for p in serial.tools.list_ports.comports()]

def reset_to_bootloader(port):
    print(f"[*] Toggling DTR/RTS bootloader reset sequence on {port}...")
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.open()
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.dtr = True
        ser.rts = False
        time.sleep(0.1)
        ser.dtr = False
        ser.rts = False
        ser.close()
        print("[+] Reset signal dispatched.")
        return True
    except Exception as e:
        print(f"[-] Reset toggle failed (normal for pure UART bridges): {e}")
        return False

def compile_project():
    print("=" * 60)
    print("[*] Starting Compilation (ESP-IDF via Ninja)...")
    print("=" * 60)
    env = setup_environment()
    
    # Check if build directory exists and has build.ninja
    if not os.path.exists(os.path.join(BUILD_DIR, "build.ninja")):
        print("[*] Configuring CMake build system...")
        reconfig_cmd = [
            IDF_PYTHON,
            os.path.join(IDF_TOOLS_DIR, "idf.py"),
            "reconfigure"
        ]
        res = subprocess.run(reconfig_cmd, cwd=PROJECT_DIR, env=env)
        if res.returncode != 0:
            print("[-] Reconfiguration failed.")
            return False
            
    # Build with ninja (using -j 4 to prevent Windows file locking contention)
    ninja_exe = os.path.join(NINJA_DIR, "ninja.exe")
    cmd = [ninja_exe, "-C", BUILD_DIR, "-j", "4"]
    print(f"[*] Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, env=env)
    if res.returncode != 0:
        print("[-] Build failed with exit code:", res.returncode)
        return False
    print("[+] Compilation finished successfully!")
    return True

def flash_firmware(port, baud=460800, no_reset=False):
    if not os.path.exists(FLASHER_ARGS_PATH):
        print(f"[-] Error: {FLASHER_ARGS_PATH} not found.")
        return False

    with open(FLASHER_ARGS_PATH, "r") as f:
        args_data = json.load(f)

    flash_mode = args_data.get("flash_settings", {}).get("flash_mode", "dio")
    flash_freq = args_data.get("flash_settings", {}).get("flash_freq", "40m")
    flash_size = args_data.get("flash_settings", {}).get("flash_size", "4MB")

    bootloader_bin = os.path.join(BUILD_DIR, "bootloader", "bootloader.bin")
    partition_bin = os.path.join(BUILD_DIR, "partition_table", "partition-table.bin")
    app_bin = os.path.join(BUILD_DIR, "ESP_Audio_Player.bin")

    before_mode = "no-reset" if no_reset else "default-reset"
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32",
        "--port", port,
        "--baud", str(baud),
        "--before", before_mode,
        "--after", "hard-reset",
        "write-flash",
        "-z",
        "--flash-mode", flash_mode,
        "--flash-freq", flash_freq,
        "--flash-size", flash_size,
        "0x1000", bootloader_bin,
        "0x8000", partition_bin,
        "0x10000", app_bin
    ]

    print("-" * 60)
    print(f"[*] Executing Flash: {' '.join(cmd)}")
    print("-" * 60)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=BUILD_DIR)
    
    verified_count = 0
    for line in proc.stdout:
        print(line, end="")
        if "Hash of data verified." in line:
            verified_count += 1
    proc.wait()
    
    # All 3 partitions (bootloader, partition-table, app) must be verified
    return (proc.returncode == 0) or (verified_count >= 3)

def serial_monitor(port, baud=115200):
    print("\n" + "=" * 60)
    print(f"[*] Opening Serial Monitor on {port} @ {baud} baud (Ctrl+C to quit)...")
    print("=" * 60)
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
        ser.dtr = False
        ser.rts = False
        while True:
            data = ser.read(128)
            if data:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n[*] Serial monitor exited.")
    except Exception as e:
        print(f"\n[-] Monitor error: {e}")

def main():
    target_port = "COM12"
    baud = 460800
    skip_compile = "--skip-compile" in sys.argv
    do_monitor = "--monitor" in sys.argv or "-m" in sys.argv
    no_reset = "--no-reset" in sys.argv
    retries = 5

    for i, arg in enumerate(sys.argv):
        if arg == "--port" and i + 1 < len(sys.argv):
            target_port = sys.argv[i + 1]
        elif arg == "--baud" and i + 1 < len(sys.argv):
            baud = int(sys.argv[i + 1])
        elif arg == "--retries" and i + 1 < len(sys.argv):
            retries = int(sys.argv[i + 1])

    ports = get_ports()
    if target_port not in ports:
        print(f"[-] Port {target_port} not found! Available ports: {ports}")
        if ports:
            print(f"[*] Using first available: {ports[0]}")
            target_port = ports[0]
        else:
            sys.exit(1)

    print(f"[+] Target Device: {target_port} | Flashing Baud: {baud}")

    # 1. Compile
    if not skip_compile:
        if not compile_project():
            print("[-] Compilation failed! Aborting upload.")
            sys.exit(1)
    else:
        print("[*] Skipping compilation as requested.")

    # 2. Upload with retry loop
    print("\n" + "=" * 60)
    print(f"[*] Flashing Firmware to {target_port}...")
    print("[!] Ensure ESP32-CAM GPIO 0 is tied to GND, then press RST if waiting for connection.")
    print("=" * 60)

    success = False
    for attempt in range(1, retries + 1):
        print(f"\n---> Upload Attempt {attempt} of {retries}...")
        if not no_reset:
            reset_to_bootloader(target_port)
        if flash_firmware(target_port, baud, no_reset=no_reset):
            success = True
            break
        print(f"[-] Attempt {attempt} failed.")
        if attempt < retries:
            print("[*] Retrying in 2 seconds... (make sure GPIO 0 is grounded and press RST!)")
            time.sleep(2)

    if success:
        print("\n" + "=" * 60)
        print("[+] Firmware successfully flashed and verified!")
        print("[!] Note: Disconnect GPIO 0 from GND and press RST on ESP32-CAM to boot application.")
        print("=" * 60)
        if do_monitor:
            serial_monitor(target_port)
    else:
        print("\n[-] Flashing failed after all attempts.")
        sys.exit(1)

if __name__ == "__main__":
    main()

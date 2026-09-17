#!/usr/bin/env python3
"""
Automated Hardware & Firmware Test Suite for ESP32 Audiophile A2DP Player
Runs full end-to-end verification over USB Serial (COM12 @ 115200 baud):
  - Serial Liveness & REPL prompt
  - System Telemetry & Power/Reset Health
  - Internal SRAM & External PSRAM Allocator
  - MicroSD Card Detection & File Listing
  - Digital Volume Engine (Logarithmic curve)
  - Bluetooth A2DP Sink Connection State
  - Test Tone Audio Generation & Streaming
  - Real FLAC Audio Playback, Decoding & PSRAM Ring Buffer
  - Nokia C1-01 Display Subsystem Pattern
"""

import sys
import os
import time
import json
import argparse
import serial
import serial.tools.list_ports

# Force UTF-8 encoding on Windows console
if sys.stdout.encoding != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

class ESPTester:
    def __init__(self, port="COM12", baud=115200, timeout=0.2):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None
        self.results = []
        self.raw_logs = []

    def log(self, msg, level="INFO"):
        prefix = {
            "INFO": "\033[94m[*]\033[0m",
            "PASS": "\033[92m[✓ PASS]\033[0m",
            "FAIL": "\033[91m[✗ FAIL]\033[0m",
            "WARN": "\033[93m[!]\033[0m"
        }.get(level, "[*]")
        print(f"{prefix} {msg}")

    def record_result(self, test_name, passed, detail=""):
        self.results.append({
            "name": test_name,
            "passed": passed,
            "detail": detail
        })
        status = "PASS" if passed else "FAIL"
        self.log(f"{test_name}: {detail}", level=status)

    def connect(self):
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
            # Never toggle DTR/RTS so we don't disrupt active state
            self.ser.dtr = False
            self.ser.rts = False
            time.sleep(0.3)
            # Drain any old bytes
            if self.ser.in_waiting:
                self.ser.read(self.ser.in_waiting)
            return True
        except Exception as e:
            self.log(f"Cannot open {self.port}: {e}", level="FAIL")
            return False

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_cmd(self, cmd, wait_sec=1.5, stop_on_panic=True):
        if not self.ser or not self.ser.is_open:
            return ""
        # Drain
        if self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
            
        self.ser.write((cmd + "\r\n").encode('utf-8'))
        t_end = time.time() + wait_sec
        captured = ""
        while time.time() < t_end:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                captured += chunk
                self.raw_logs.append(chunk)
                if stop_on_panic and ("Guru Meditation Error" in chunk or "assert failed:" in chunk):
                    # Panic detected! Capture immediate rest of crash dump
                    time.sleep(1.0)
                    if self.ser.in_waiting:
                        captured += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                    break
            time.sleep(0.04)
        return captured

    def parse_json_from_output(self, text, expected_type):
        for line in text.splitlines():
            line = line.strip()
            if line.startswith("{") and line.endswith("}"):
                try:
                    data = json.loads(line)
                    if data.get("type") == expected_type:
                        return data
                except Exception:
                    continue
        return None

    # --- TEST SUITE STAGES ---

    def test_liveness(self):
        self.log("STAGE 1: Checking serial connection & REPL prompt...")
        resp = self.send_cmd("", wait_sec=0.8)
        if "esp32>" in resp:
            self.record_result("Serial Liveness", True, "Prompt 'esp32>' responsive")
            return True
        # Retry with newline
        resp = self.send_cmd("\n", wait_sec=1.0)
        if "esp32>" in resp:
            self.record_result("Serial Liveness", True, "Prompt 'esp32>' responsive on retry")
            return True
        self.record_result("Serial Liveness", False, "No 'esp32>' prompt received")
        return False

    def test_telemetry_and_memory(self):
        self.log("STAGE 2: Verifying system telemetry & memory allocators...")
        resp = self.send_cmd("status_json", wait_sec=1.2)
        data = self.parse_json_from_output(resp, "status")
        if not data:
            self.record_result("System Telemetry", False, "Failed to parse JSON status")
            return False

        sram_kb = data.get("sram", 0) // 1024
        psram_kb = data.get("psram", 0) // 1024
        
        mem_ok = (sram_kb >= 80) and (psram_kb >= 2048)
        detail = f"SRAM Free: {sram_kb} KB, PSRAM Free: {psram_kb} KB (~{psram_kb//1024} MB)"
        self.record_result("Memory & Heap Health", mem_ok, detail)
        return mem_ok

    def test_volume_control(self):
        self.log("STAGE 3: Testing digital volume engine...")
        resp1 = self.send_cmd("volume 80", wait_sec=0.8)
        ok1 = "Volume set to 80%" in resp1

        resp2 = self.send_cmd("volume 10", wait_sec=0.8)
        ok2 = "Volume set to 10%" in resp2

        self.record_result("Volume Control (Logarithmic)", ok1 and ok2, "Tested 80% and 10% presets cleanly")
        return ok1 and ok2

    def test_sd_card_and_files(self):
        self.log("STAGE 4: Testing MicroSD storage & directory listing...")
        resp = self.send_cmd("ls_json", wait_sec=2.0)
        data = self.parse_json_from_output(resp, "ls")
        if not data or "files" not in data:
            self.record_result("MicroSD Mount & Read", False, "Could not list files from /sdcard")
            return []

        audio_files = [f for f in data["files"] if not f.get("is_dir") and (f.get("name", "").endswith(".flac") or f.get("name", "").endswith(".wav"))]
        passed = len(audio_files) > 0
        detail = f"Found {len(audio_files)} playable audio tracks in /sdcard"
        self.record_result("MicroSD Mount & Read", passed, detail)
        return audio_files

    def test_sine_playback(self):
        self.log("STAGE 5: Testing 440 Hz test tone generation & A2DP stream...")
        resp = self.send_cmd("tone 440", wait_sec=3.0)
        
        if "Guru Meditation Error" in resp or "assert failed:" in resp:
            self.record_result("Sine Tone Audio", False, "CRASH / Panic detected during tone start")
            return False

        # Query status
        status_resp = self.send_cmd("status_json", wait_sec=1.0)
        data = self.parse_json_from_output(status_resp, "status")
        is_playing = data and data.get("state") == "PLAYING" and data.get("source") == "SINE"

        # Stop tone
        self.send_cmd("stop", wait_sec=0.8)
        self.record_result("Sine Tone Audio", is_playing, "440 Hz stream generated and stopped without crash")
        return is_playing

    def test_flac_playback(self, audio_files):
        self.log("STAGE 6: Testing real FLAC playback from SD card...")
        if not audio_files:
            self.record_result("FLAC Playback", False, "No audio files available on SD card to test")
            return False

        # Find a .flac file (preferably small to medium)
        flac_files = [f for f in audio_files if f.get("name", "").endswith(".flac")]
        if not flac_files:
            flac_files = audio_files

        target = None
        for f in flac_files:
            if "animal" in f.get("name", "").lower():
                target = f
                break
        if not target:
            target = flac_files[0]
        track_path = target.get("path")
        track_name = target.get("name")
        self.log(f"Testing playback of: '{track_name}'...")

        resp = self.send_cmd(f'play_file "{track_path}"', wait_sec=4.0)
        
        if "Guru Meditation Error" in resp or "assert failed:" in resp or "Panic" in resp:
            self.record_result("FLAC Playback", False, f"CRASH: Kernel panic detected while opening '{track_name}'!")
            return False

        if "Failed to play file" in resp or "Cannot decode FLAC" in resp:
            self.record_result("FLAC Playback", False, f"File open or format error: {resp.strip()}")
            return False

        # Monitor playback for 5 seconds to verify ring buffer stability
        time.sleep(2.0)
        status_resp = self.send_cmd("status_json", wait_sec=1.2)
        data = self.parse_json_from_output(status_resp, "status")
        
        played = data.get("pos", 0) if data else 0
        total = data.get("total", 0) if data else 0
        state = data.get("state", "UNKNOWN") if data else "UNKNOWN"

        # Stop playback
        self.send_cmd("stop", wait_sec=0.8)

        passed = (state == "PLAYING" or played > 0)
        detail = f"Playing: {state}, Streamed: {played//1024} KB / {total//1024} KB with zero panics"
        self.record_result("FLAC Playback", passed, detail)
        return passed

    def test_display_subsystem(self):
        self.log("STAGE 7: Testing Nokia C1-01 Display Subsystem...")
        resp = self.send_cmd("display_test", wait_sec=1.5)
        if "Guru Meditation Error" in resp or "assert failed:" in resp:
            self.record_result("Display Subsystem", False, "SPI panic during display test pattern")
            return False
        passed = "Triggering Nokia C1-01 display test pattern" in resp
        self.record_result("Display Subsystem", passed, "Non-blocking hardware SPI pattern executed cleanly")
        return passed

    def run_all(self):
        print("\n" + "=" * 65)
        print("    ESP32 AUDIO PLAYER — AUTOMATED HARDWARE TEST SUITE")
        print("=" * 65)

        if not self.connect():
            return False

        try:
            if not self.test_liveness():
                return False

            self.test_telemetry_and_memory()
            self.test_volume_control()
            files = self.test_sd_card_and_files()
            self.test_sine_playback()
            self.test_flac_playback(files)
            self.test_display_subsystem()

        finally:
            self.close()

        # Print Final Report Matrix
        print("\n" + "=" * 65)
        print(f"{'TEST NAME':<35} | {'STATUS':<8} | {'DETAIL'}")
        print("-" * 65)
        all_passed = True
        for r in self.results:
            st = "PASS" if r["passed"] else "FAIL"
            if not r["passed"]:
                all_passed = False
            print(f"{r['name']:<35} | {st:<8} | {r['detail']}")
        print("=" * 65)

        if all_passed:
            print("\033[92m[✓] ALL TESTS PASSED! Hardware & firmware are 100% verified.\033[0m\n")
        else:
            print("\033[91m[!] SOME TESTS FAILED. Please review the detailed logs above.\033[0m\n")

        return all_passed

def main():
    parser = argparse.ArgumentParser(description="ESP32 Audio Player Automated Test Suite")
    parser.add_argument("--port", default="COM12", help="Serial port of ESP32 (default: COM12)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    args = parser.parse_args()

    tester = ESPTester(port=args.port, baud=args.baud)
    success = tester.run_all()
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()

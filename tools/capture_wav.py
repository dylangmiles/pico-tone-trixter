#!/usr/bin/env python3
"""
Capture WAV output from the offline_test firmware over USB CDC.

Workflow:
  1. Flash offline_test to the Pico — either method works:

     a) Debug probe (CLion) — preferred:
        - Run → Edit Configurations → + → OpenOCD Download & Run
        - Name: "offline_test on Pico"
        - Target:      offline_test
        - Executable:  offline_test.elf
        - Board config file: openocd.cfg  (same file as the main config)
        - Use "Run" (not Debug) to avoid Core 1 halting issues
        - The Pico resets and starts running automatically after flash

     b) UF2 drag-and-drop:
        - Hold BOOTSEL, plug in USB, release
        - cp cmake-build-debug/offline_test.uf2 /Volumes/RPI-RP2/

  2. Run this script (it will wait for the Pico to enumerate as USB CDC)
  3. The script triggers processing by sending a newline to the Pico
  4. WAV is saved to <output.wav>

Setup (first time):
  cd tools
  python3 -m venv .venv
  source .venv/bin/activate
  pip install -r requirements.txt

Usage (activate venv first):
  cd tools
  source .venv/bin/activate
  python3 capture_wav.py <uart_device> [output.wav]
       [--guitar {garrison,tanglewood}] [--duration SECONDS]

If output.wav is omitted, the filename is auto-generated:
  output/output_<guitar>-<duration>s-<YYYYMMDD>.wav

The Pico communicates over UART via the debug probe (GPIO 0 TX, GPIO 1 RX).
The debug probe exposes this as a USB CDC serial port (usbmodem) at 115200 baud.
Transfer of the ~764 KB WAV takes around 66 seconds at this rate.

Finding the UART device on macOS:
  ls /dev/cu.usbmodem*
  (use the port that appears when the debug probe is connected, not when the
   Pico's own USB is connected — offline_test has USB CDC disabled)
"""

import argparse
import datetime
import os
import sys
import struct
import time


def main():
    parser = argparse.ArgumentParser(description="Capture WAV output from offline_test firmware")
    parser.add_argument('device', help='UART device, e.g. /dev/cu.usbmodem101')
    parser.add_argument('output', nargs='?', help='Output WAV path (auto-generated if omitted)')
    parser.add_argument('--guitar', choices=['garrison', 'tanglewood'], default='garrison',
                        help='Guitar used in this build (for output filename)')
    parser.add_argument('--duration', type=int, default=20,
                        help='Duration embedded in firmware in seconds (for output filename)')
    args = parser.parse_args()

    device = args.device
    if args.output:
        out_path = args.output
    else:
        date_str = datetime.date.today().strftime('%Y%m%d')
        out_dir  = os.path.join(os.path.dirname(__file__), 'output')
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, f'output_{args.guitar}-{args.duration}s-{date_str}.wav')
        print(f"Output: {out_path}")

    try:
        import serial
    except ImportError:
        print("pyserial not found. Set up the venv first:")
        print("  cd tools && python3 -m venv .venv && source .venv/bin/activate")
        print("  pip install -r requirements.txt")
        sys.exit(1)

    BAUD = 115200   # debug probe UART bridge rate

    print(f"Opening {device} at {BAUD} baud ...")
    ser = serial.Serial(device, BAUD, timeout=60)

    # Wait for the Pico to print "READY" (sent once per second until host responds).
    # Only then send the trigger — avoids races where the host sends too early.
    print("Waiting for Pico READY signal (reset the Pico if it doesn't appear)...")
    while True:
        line = ser.readline()
        if not line:
            print("ERROR: timeout — is offline_test flashed and the Pico connected via USB?")
            sys.exit(1)
        text = line.decode('utf-8', errors='replace').rstrip()
        if text and text != 'READY':
            print(f"  Pico: {text}")
        if text == 'READY':
            print("  Pico: READY — triggering...")
            ser.write(b'\n')
            ser.flush()
            break

    # Read remaining status lines until WAV_DATA_START
    print("Waiting for WAV_DATA_START...")
    while True:
        line = ser.readline()
        if not line:
            print("ERROR: timeout waiting for processing to start")
            sys.exit(1)
        text = line.decode('utf-8', errors='replace').rstrip()
        if text:
            print(f"  Pico: {text}")
        if text == 'WAV_DATA_START':
            break

    # Read WAV header (fixed 44 bytes for a standard PCM WAV)
    wav_header = ser.read(44)
    if len(wav_header) < 44:
        print("ERROR: incomplete WAV header received")
        sys.exit(1)

    if wav_header[:4] != b'RIFF' or wav_header[8:12] != b'WAVE':
        print("ERROR: invalid WAV header — binary may be misaligned")
        sys.exit(1)

    data_size   = struct.unpack_from('<I', wav_header, 40)[0]
    sample_rate = struct.unpack_from('<I', wav_header, 24)[0]
    bits        = struct.unpack_from('<H', wav_header, 34)[0]
    n_samples   = data_size // (bits // 8)
    duration    = n_samples / sample_rate

    print(f"WAV: {sample_rate} Hz, {bits}-bit mono, {n_samples} samples ({duration:.2f} sec)")
    print(f"Receiving {data_size} bytes of audio data...")

    ser.timeout = 180  # at 115200 baud ~764 KB takes ~66 sec; allow 3 min for safety

    with open(out_path, 'wb') as f:
        f.write(wav_header)
        received = 0
        t_start  = time.time()

        while received < data_size:
            chunk = ser.read(min(4096, data_size - received))
            if not chunk:
                print(f"\nERROR: timeout after {received} bytes")
                sys.exit(1)
            f.write(chunk)
            received += len(chunk)

            elapsed = time.time() - t_start
            pct     = 100 * received // data_size
            rate_kb = received / elapsed / 1024 if elapsed > 0 else 0
            eta_s   = (data_size - received) / (received / elapsed) if received > 0 and elapsed > 0 else 0
            print(f"\r  {pct:3d}%  {received}/{data_size} bytes  "
                  f"{rate_kb:.0f} KB/s  ETA {eta_s:.0f}s   ", end='', flush=True)

    print(f"\nSaved: {out_path}")

    # Read trailing status from Pico (timing info)
    ser.timeout = 3
    while True:
        line = ser.readline()
        if not line:
            break
        text = line.decode('utf-8', errors='replace').rstrip()
        if text:
            print(f"  Pico: {text}")

    ser.close()
    print("\nDone. Open the WAV in Audacity to compare with the original piezo recording.")


if __name__ == '__main__':
    main()

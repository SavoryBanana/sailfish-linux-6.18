#!/usr/bin/env python3
import sys
import os
import re
import base64

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.join(SCRIPT_DIR, "firmware")
os.makedirs(FW_DIR, exist_ok=True)

def process_text(text):
    pattern = re.compile(r'---START_([a-zA-Z0-9_\.\-]+)_B64[^\n]*---\n(.*?)---END_\1_B64---', re.DOTALL)
    matches = pattern.findall(text)
    if not matches:
        # Also try simpler pattern
        pattern = re.compile(r'---START_([a-zA-Z0-9_\.\-]+)_B64[^\n]*---\n(.*?)(?:---END|\Z)', re.DOTALL)
        matches = pattern.findall(text)

    saved = 0
    for name, b64_data in matches:
        # Clean up whitespace
        clean_b64 = "".join(b64_data.split())
        try:
            raw = base64.b64decode(clean_b64)
            out_path = os.path.join(FW_DIR, name)
            with open(out_path, "wb") as f:
                f.write(raw)
            print(f"[+] Successfully extracted {name} ({len(raw)} bytes) -> {out_path}")
            saved += 1
        except Exception as e:
            print(f"[-] Failed to decode {name}: {e}")

    if saved == 0:
        print("[-] No base64 blocks found. Please paste the dump output or supply a file path.")
    else:
        print(f"\n[+] Total firmware files saved to {FW_DIR}: {saved}")

if __name__ == "__main__":
    if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]):
        with open(sys.argv[1], "r", errors="ignore") as f:
            process_text(f.read())
    else:
        print("[*] Reading from standard input (paste base64 dump and press Ctrl+D):")
        process_text(sys.stdin.read())


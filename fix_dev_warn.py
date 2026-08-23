#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path.cwd()
DEVICE = ROOT / "src/compat/linux/device.h"

def fail(msg):
    print(f"\nFAILED: {msg}", file=sys.stderr)
    sys.exit(1)

if not DEVICE.exists():
    fail(f"file not found: {DEVICE} (run this from the rtl8188ee-macos project root)")

text = DEVICE.read_text(encoding="utf-8")

old = "rtw88_printk(KERN_WARNING, fmt, ##__VA_ARGS__)"
new = "rtw88_printk(KERN_WARN, fmt, ##__VA_ARGS__)"

if new in text:
    print("Already fixed -- skipping.")
elif old in text:
    text = text.replace(old, new)
    DEVICE.write_text(text, encoding="utf-8")
    print("Fixed: KERN_WARNING -> KERN_WARN in device.h")
else:
    fail("expected dev_warn macro body not found in device.h -- check the file manually")

print("\nDone. Next: rm -rf build && make -f Makefile.rtl8188ee kext")
print("Then: sudo chown -R root:wheel build/out/rtl8188ee.kext && ./check_kext_symbols.sh")

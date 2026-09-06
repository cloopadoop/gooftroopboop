"""Inert process fixture for Rust protocol/lifecycle tests; no project data."""
import sys
import time

for line in sys.stdin:
    if sys.argv[1] == "hang":
        time.sleep(60)
    if sys.argv[1] == "eof":
        break
    if sys.argv[1] == "invalid":
        print("fixture-private-sentinel: deliberately not JSON", flush=True)
    else:
        print(line, end="", flush=True)

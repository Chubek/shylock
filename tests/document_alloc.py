import subprocess
import sys

proc = subprocess.run([sys.argv[1]], capture_output=True, text=True, timeout=120)
assert proc.returncode == 0 and "Sanitizer" not in proc.stderr, (proc.returncode, proc.stdout, proc.stderr)
print(proc.stdout, end="")

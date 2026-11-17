import sys
from run_binary_checks import run_checks
run_checks(sys.argv[1], ["test_test24_shim.py", "test_test24_roundtrip.py"])

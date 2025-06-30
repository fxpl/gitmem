import os
import subprocess
import sys
import argparse

EXAMPLES_DIR = "examples"

def run_gitmem_test(gitmem_path, file_path, should_pass):
    try:
        result = subprocess.run([gitmem_path, file_path, "-e", "-o", "/dev/null"], capture_output=True, text=True)
        passed = (result.returncode == 0)
    except FileNotFoundError:
        print(f"Error: '{gitmem_path}' executable not found.")
        sys.exit(1)

    if passed == should_pass:
        status = "PASS"
    else:
        status = "FAIL"

    print(f"[{status}] {file_path} (exit code: {result.returncode})")
    return status == "PASS"

def main():
    parser = argparse.ArgumentParser(description="Test runner for gitmem.")
    parser.add_argument(
        "--gitmem", "-g",
        required=True,
        help="Path to the gitmem executable"
    )
    args = parser.parse_args()
    gitmem_path = args.gitmem

    total_tests = 0
    failed_tests = 0

    for outcome in ["passing", "failing"]:
        should_pass = (outcome == "passing")
        for category in ["syntax", "semantics"]:
            test_dir = os.path.join(EXAMPLES_DIR, outcome, category)
            if not os.path.isdir(test_dir):
                continue
            for root, _, files in os.walk(test_dir):
                for file in files:
                    file_path = os.path.join(root, file)
                    total_tests += 1
                    if not run_gitmem_test(gitmem_path, file_path, should_pass):
                        failed_tests += 1

    print("\nSummary:")
    print(f"Total tests run: {total_tests}")
    print(f"Tests failed:    {failed_tests}")
    print(f"Tests passed:    {total_tests - failed_tests}")

    if failed_tests > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()

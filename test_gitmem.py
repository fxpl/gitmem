import os
import subprocess
import sys
import argparse
from collections import defaultdict

EXAMPLES_DIR = "examples"

SYNC_KINDS = {
  "linear": {"sync": "linear"},
  "branching-eager": {"sync": "branching", "branching_mode": "eager"},
  "branching-lazy": {"sync": "branching", "branching_mode": "lazy"},
}

def supports_color():
  return sys.stdout.isatty() and os.getenv("NO_COLOR") is None

def color(text, code):
  if not supports_color():
    return text
  return f"\033[{code}m{text}\033[0m"

def green(text):
  return color(text, "32")

def red(text):
  return color(text, "31")

def run_gitmem_test(gitmem_path, file_path, should_accept, sync_kind):
  sync_config = SYNC_KINDS[sync_kind]

  cmd = [
    gitmem_path,
    file_path,
    "--sync", sync_config["sync"],
  ]

  if "branching_mode" in sync_config:
    cmd.extend(["--branching-mode", sync_config["branching_mode"]])

  cmd.extend([
    "-e",
    "-o", "/dev/null"
  ])

  try:
    result = subprocess.run(
      cmd,
      capture_output=True,
      text=True
    )
    if should_accept:
      accepted = (result.returncode == 0)
    else:
      accepted = (result.returncode == 1)
  except FileNotFoundError:
    print(f"Error: '{gitmem_path}' executable not found.")
    sys.exit(1)

  status = green("PASS") if accepted else red("FAIL")
  print(f"[{status}] {file_path} [{sync_kind}] (exit code: {result.returncode})")
  return accepted

def main():
  parser = argparse.ArgumentParser(description="Test runner for gitmem.")
  parser.add_argument(
    "--gitmem", "-g",
    required=True,
    help="Path to the gitmem executable"
  )
  parser.add_argument(
    "--linear",
    action="store_true",
    help="Only run linear sync tests"
  )
  parser.add_argument(
    "--branching-eager",
    action="store_true",
    help="Only run branching-eager tests"
  )
  parser.add_argument(
    "--branching-lazy",
    action="store_true",
    help="Only run branching-lazy tests"
  )

  args = parser.parse_args()
  gitmem_path = args.gitmem

  selected_syncs = []

  if args.linear:
    selected_syncs.append("linear")
  if args.branching_eager:
    selected_syncs.append("branching-eager")
  if args.branching_lazy:
    selected_syncs.append("branching-lazy")

  # If none specified, run all
  if not selected_syncs:
    selected_syncs = list(SYNC_KINDS.keys())

  results = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: {
    "total": 0,
    "failed": 0
  })))

  total_tests = 0
  failed_tests = 0
  failing_tests = []

  for expectation in ["accept", "reject"]:
    should_accept = (expectation == "accept")

    for category in ["syntax", "semantics"]:
      base_dir = os.path.join(EXAMPLES_DIR, expectation, category)
      if not os.path.isdir(base_dir):
        continue

      for sync_kind in selected_syncs:
        # syntax tests are sync-agnostic → only run once
        if category == "syntax" and sync_kind != "linear":
          continue

        if category == "semantics":
          if sync_kind == "linear":
            test_dir = os.path.join(base_dir, "linear")
          else:
            test_dir = os.path.join(base_dir, "branching")
        else:
          test_dir = base_dir

        if not os.path.isdir(test_dir):
          continue

        for root, _, files in os.walk(test_dir):
          for file in files:
            file_path = os.path.join(root, file)

            total_tests += 1
            results[expectation][category][sync_kind]["total"] += 1

            passed = run_gitmem_test(
              gitmem_path,
              file_path,
              should_accept,
              sync_kind
            )

            if not passed:
              failed_tests += 1
              results[expectation][category][sync_kind]["failed"] += 1
              failing_tests.append((file_path, sync_kind))

  print("\nDetailed Summary:")
  for expectation, categories in results.items():
    print(f"\n{expectation.upper()}:")
    for category, syncs in categories.items():
      print(f"  {category}:")
      for sync_kind, stats in syncs.items():
        passed = stats["total"] - stats["failed"]
        print(
          f"    {sync_kind}: "
          f"{passed}/{stats['total']} passed "
          f"({stats['failed']} failed)"
        )

  print("\nOverall Summary:")
  print(f"Total tests run: {total_tests}")
  print(f"Tests failed:    {failed_tests}")
  print(f"Tests passed:    {total_tests - failed_tests}")

  if failing_tests:
    print("\nFailing tests:")
    for path, sync in failing_tests:
      print(f"  {red(path)} [{sync}]")

  if failed_tests > 0:
    sys.exit(1)

if __name__ == "__main__":
  main()

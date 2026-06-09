#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

run_py() {
	local cmd=("$@")
	if "${cmd[@]}"; then
		return 0
	fi
	return 1
}

if run_py python3 tools/make_icon.py \
	|| run_py python tools/make_icon.py \
	|| run_py py -3 tools/make_icon.py \
	|| run_py /c/Windows/py -3 tools/make_icon.py \
	|| run_py /c/Users/dodo/AppData/Local/Programs/Python/Python313/python.exe tools/make_icon.py \
	|| run_py /c/Users/dodo/AppData/Local/Programs/Python/Python312/python.exe tools/make_icon.py \
	|| run_py /c/Users/dodo/AppData/Local/Programs/Python/Python311/python.exe tools/make_icon.py; then
	exit 0
fi

if [[ -f icon.jpg ]]; then
	echo "Python not found in PATH; keeping existing icon.jpg"
	echo "To regenerate: pacman -S python  (MSYS2)  or  py -3 tools/make_icon.py  (Windows)"
	exit 0
fi

echo "Error: Python not found and icon.jpg is missing." >&2
echo "Install Python (MSYS2: pacman -S python) or copy icon.jpg into the project." >&2
exit 1

#!/bin/bash
# Build + package OutRunners_MD for Amiga RTG
set -euo pipefail
cd "$(dirname "$0")"
exec ../../shared/package_rtg_hdf.sh "$@" "OutRunners_MD"

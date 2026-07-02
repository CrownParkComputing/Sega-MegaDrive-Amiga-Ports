#!/bin/bash
# Build + package OutRun2019_MD for Amiga RTG
set -euo pipefail
cd "$(dirname "$0")"
exec ../../shared/package_rtg_hdf.sh "$@" "OutRun2019_MD"

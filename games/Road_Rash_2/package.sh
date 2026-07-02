#!/bin/bash
# Build + package RoadRash2_MD for Amiga RTG
set -euo pipefail
cd "$(dirname "$0")"
exec ../../shared/package_rtg_hdf.sh "$@" "RoadRash2_MD"

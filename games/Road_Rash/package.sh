#!/bin/bash
# Build + package RoadRash_MD for Amiga RTG
set -euo pipefail
cd "$(dirname "$0")"
exec ../../shared/package_rtg_hdf.sh "$@" "RoadRash_MD"

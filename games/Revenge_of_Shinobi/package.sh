#!/bin/bash
# Build + package Revenge of Shinobi MD for Amiga RTG
set -euo pipefail
cd "$(dirname "$0")"
exec ../shared/package_rtg_hdf.sh "$@" "RevengeOfShinobi_MD"
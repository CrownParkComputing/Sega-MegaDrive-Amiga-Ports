#!/bin/bash
# Build + package Sonic1_MD for Amiga RTG
set -euo pipefail
cd "$(dirname "$0")"
exec ../../shared/package_rtg_hdf.sh "$@" "Sonic1_MD"

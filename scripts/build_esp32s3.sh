#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir/HeishaMon"
arduino-cli compile --library "$project_dir/../bridge-time" --output-dir . \
  --fqbn=esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=enabled,PartitionScheme=min_spiffs \
  --warnings=none --verbose HeishaMon.ino

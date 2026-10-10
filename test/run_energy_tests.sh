#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
json_include="${ARDUINOJSON_INCLUDE:-$project_dir/.pio/libdeps/esp32s3/ArduinoJson/src}"
if [[ ! -f "$json_include/ArduinoJson.h" ]]; then
  json_include="$HOME/Arduino/libraries/ArduinoJson/src"
fi
if [[ ! -f "$json_include/ArduinoJson.h" ]]; then
  echo "Set ARDUINOJSON_INCLUDE to the installed ArduinoJson src directory" >&2
  exit 1
fi
test_binary="$(mktemp /tmp/heishamon-energy-test.XXXXXX)"
trap 'rm -f "$test_binary"' EXIT
g++ -std=c++11 -Wall -Wextra -Werror -I "$project_dir/HeishaMon" \
  -I "$json_include" "$project_dir/test/energy_reporting_test.cpp" -o "$test_binary"
"$test_binary"
echo "Energy reporting tests passed"

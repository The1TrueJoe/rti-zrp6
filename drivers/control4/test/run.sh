#!/usr/bin/env bash
# Builds the driver (stage only) and runs every test that can run here:
#   - unit tests under each available interpreter (lua, luajit)
#   - the Remote Buttons and Control tab pages against the driver, if node is installed
#   - the mosquitto integration test, if mosquitto is installed
# Nothing here talks to a real ZRP-6: the broker is a private mosquitto on
# 127.0.0.1 with a random port.
set -euo pipefail
cd "$(dirname "$0")/.."

./build.sh --stage
DRIVER=build/stage/driver.lua
status=0

# The tab pages must be in the package, where driver.xml's <tab>s point.
for tab in 'Remote Buttons:index' 'Control:control'; do
  name="${tab%%:*}"; page="${tab##*:}"
  if [[ ! -f "build/stage/www/html/$page.html" ]] || ! grep -q "<tab name=\"$name\" file=\"html/$page.html\"/>" build/stage/driver.xml; then
    echo "FAIL: build/stage is missing the $name tab (www/html/$page.html or its <tab>)"
    status=1
  fi
done

for interp in lua luajit; do
  if command -v "$interp" >/dev/null 2>&1; then
    echo
    "$interp" test/test_driver.lua "$DRIVER" || status=1
  fi
done

if command -v node >/dev/null 2>&1; then
  for interp in lua luajit; do
    if command -v "$interp" >/dev/null 2>&1; then
      echo
      node test/ui/page_test.js "$DRIVER" "$interp" || status=1
      echo
      node test/ui/control_test.js "$DRIVER" "$interp" || status=1
    fi
  done
else
  echo
  echo "node not installed: skipping the Remote Buttons tab test"
fi

if command -v mosquitto >/dev/null 2>&1 && command -v mosquitto_pub >/dev/null 2>&1; then
  echo
  interps=()
  for interp in lua luajit; do command -v "$interp" >/dev/null 2>&1 && interps+=("$interp"); done
  python3 test/integration/run_integration.py "$DRIVER" "${interps[@]}" || status=1
else
  echo
  echo "mosquitto not installed: skipping the integration test"
fi

exit $status

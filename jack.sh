#!/bin/bash
set -e

# Usage: ./jack.sh <number of gigabytes to allocate to jack>
# example: ./jack 4 will allocate 4 gigabytes

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JACK_SECURITY_PROPERTIES="$SCRIPT_DIR/build/host/java.security-jack"
JACK_ADMIN="$SCRIPT_DIR/prebuilts/sdk/tools/jack-admin"
JACK_HOME_DIR="${JACK_HOME:-$HOME/.jack-server}"

if [ ! -r "$JACK_SECURITY_PROPERTIES" ]; then
    echo "Missing Jack TLS compatibility policy: $JACK_SECURITY_PROPERTIES" >&2
    exit 1
fi

# This historical Jack server needs TLSv1/TLSv1.1 enabled locally. Keep the
# exception scoped to Jack rather than weakening the host JVM policy.
export JACK_SERVER_VM_ARGUMENTS="-Dfile.encoding=UTF-8 -XX:+TieredCompilation -Xmx$1g -Djava.security.properties=$JACK_SECURITY_PROPERTIES"
: "${JACK_EXTRA_CURL_OPTIONS:=--tlsv1.2}"
export JACK_EXTRA_CURL_OPTIONS

if [ ! -d "$JACK_HOME_DIR" ]; then
    "$JACK_ADMIN" install-server \
        "$SCRIPT_DIR/prebuilts/sdk/tools/jack-launcher.jar" \
        "$SCRIPT_DIR/prebuilts/sdk/tools/jack-server-4.8.ALPHA.jar"
fi
if [ ! -r "$JACK_HOME_DIR/launcher.jar" ] || [ ! -r "$JACK_HOME_DIR/server-1.jar" ]; then
    echo "Jack installation in $JACK_HOME_DIR is incomplete; inspect it before retrying." >&2
    exit 1
fi

kill_status=0
"$JACK_ADMIN" kill-server || kill_status=$?
if [ "$kill_status" -ne 0 ] && [ "$kill_status" -ne 2 ]; then
    exit "$kill_status"
fi
"$JACK_ADMIN" start-server

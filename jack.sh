#!/bin/bash

# Usage: ./jack.sh <number of gigabytes to allocate to jack>
# example: ./jack 4 will allocate 4 gigabytes

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JACK_SECURITY_PROPERTIES="$SCRIPT_DIR/build/host/java.security-jack"

if [ ! -r "$JACK_SECURITY_PROPERTIES" ]; then
    echo "Missing Jack TLS compatibility policy: $JACK_SECURITY_PROPERTIES" >&2
    exit 1
fi

# This historical Jack server needs TLSv1/TLSv1.1 enabled locally. Keep the
# exception scoped to Jack rather than weakening the host JVM policy.
export JACK_SERVER_VM_ARGUMENTS="-Dfile.encoding=UTF-8 -XX:+TieredCompilation -Xmx$1g -Djava.security.properties=$JACK_SECURITY_PROPERTIES"
: "${JACK_EXTRA_CURL_OPTIONS:=--tlsv1.2}"
export JACK_EXTRA_CURL_OPTIONS
./prebuilts/sdk/tools/jack-admin kill-server
./prebuilts/sdk/tools/jack-admin start-server

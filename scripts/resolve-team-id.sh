#!/usr/bin/env bash
# Resolves the iOS development team ID.
# Uses DEVELOPMENT_TEAM env var if set, otherwise prompts the user.

if [ -n "$DEVELOPMENT_TEAM" ]; then
    echo "$DEVELOPMENT_TEAM"
    exit 0
fi

echo "Available signing identities:" >&2
security find-identity -v -p codesigning >&2
echo "" >&2
read -p "Enter your Development Team ID: " team < /dev/tty
echo "$team"

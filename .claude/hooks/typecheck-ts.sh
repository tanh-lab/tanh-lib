#!/bin/bash
# Type-check TypeScript/TSX files after Write|Edit (full project, incremental)

FILE=$(jq -r '.tool_input.file_path' 2>/dev/null)
[ -z "$FILE" ] || [ ! -f "$FILE" ] && exit 0

case "$FILE" in
  *.ts|*.tsx)
    DIR=$(dirname "$FILE")
    TSC=""
    TSCONFIG=""
    SEARCH="$DIR"
    while [ "$SEARCH" != "/" ]; do
        if [ -f "$SEARCH/node_modules/.bin/tsc" ] && [ -f "$SEARCH/tsconfig.check.json" ]; then
            TSC="$SEARCH/node_modules/.bin/tsc"
            TSCONFIG="$SEARCH/tsconfig.check.json"
            break
        fi
        SEARCH=$(dirname "$SEARCH")
    done

    [ -z "$TSC" ] && exit 0

    OUTPUT=$("$TSC" --noEmit --incremental -p "$TSCONFIG" 2>&1)
    if [ $? -ne 0 ]; then
      echo "$OUTPUT" >&2
      exit 2
    fi
    ;;
esac

exit 0

#!/bin/bash
# Format and lint TypeScript/TSX files after Write|Edit

FILE=$(jq -r '.tool_input.file_path' 2>/dev/null)
[ -z "$FILE" ] || [ ! -f "$FILE" ] && exit 0

case "$FILE" in
  *.ts|*.tsx)
    prettier --write "$FILE" 2>/dev/null

    DIR=$(dirname "$FILE")
    ESLINT=""
    SEARCH="$DIR"
    while [ "$SEARCH" != "/" ]; do
        if [ -f "$SEARCH/node_modules/.bin/eslint" ]; then
            ESLINT="$SEARCH/node_modules/.bin/eslint"
            break
        fi
        SEARCH=$(dirname "$SEARCH")
    done

    [ -z "$ESLINT" ] && exit 0

    # Auto-fix first, then check — single ESLint process
    "$ESLINT" --fix "$FILE" 2>/dev/null

    OUTPUT=$("$ESLINT" --max-warnings=0 "$FILE" 2>&1)
    EXIT=$?
    if [ $EXIT -ne 0 ]; then
      if echo "$OUTPUT" | grep -q "File ignored because of a matching ignore pattern"; then
        exit 0
      fi
      echo "$OUTPUT" >&2
      exit 2
    fi
    ;;
esac

exit 0

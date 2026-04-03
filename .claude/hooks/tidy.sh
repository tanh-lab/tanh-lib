#!/bin/bash
# Run clang-tidy on C/C++/ObjC++ source files after Write|Edit

FILE=$(jq -r '.tool_input.file_path' 2>/dev/null)
[ -z "$FILE" ] || [ ! -f "$FILE" ] && exit 0

case "$FILE" in
  *.cpp|*.mm)
    OUTPUT=$(clang-tidy --warnings-as-errors='*' -p build/desktop/Debug/ "$FILE" 2>&1)
    if [ $? -ne 0 ]; then
      echo "$OUTPUT" >&2
      exit 2
    fi
    ;;
esac

exit 0

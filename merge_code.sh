#!/bin/bash

# Check if an extension was provided
if [ -z "$1" ]; then
  echo "Usage: $0 <extension> <output_file> [directory]"
  echo "Example: $0 py all_code.txt"
  exit 1
fi

EXT="$1"
OUTPUT="${2:-merged_output.txt}"  # Default to merged_output.txt if not provided
SEARCH_DIR="${3:-.}"             # Default to current directory if not provided

# Clear the output file if it exists to avoid appending to old data
> "$OUTPUT"

echo "Merging all .$EXT files from '$SEARCH_DIR' into '$OUTPUT'..."

# Find files and loop through them
# We use -print0 and read -d $'\0' to safely handle filenames with spaces
find "$SEARCH_DIR" -type f -name "*.$EXT" -print0 | sort -z | while IFS= read -r -d '' file; do
  # 1. Append the header
  echo "#### $file" >> "$OUTPUT"
  
  # 2. Append the file content
  cat "$file" >> "$OUTPUT"
  
  # 3. Add a newline separator (optional, helps readability between files)
  echo -e "\n" >> "$OUTPUT"
done

echo "Done! Created $OUTPUT"
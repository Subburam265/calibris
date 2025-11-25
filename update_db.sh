#!/bin/bash

# Configuration - modify these values
DB_FILE="mydata.db"
TABLE_NAME="tamper_logs"
NULL_COLUMN="pushed_at"
CSV_OUTPUT="output1.csv"

# Check if required arguments are provided
if [ $# -eq 4 ]; then
  DB_FILE=$1
  TABLE_NAME=$2
  NULL_COLUMN=$3
  CSV_OUTPUT=$4
fi

# Validate inputs
if [ ! -f "$DB_FILE" ]; then
  echo "Error: Database file $DB_FILE does not exist."
  echo "Usage: $0 database.db table_name null_column output.csv"
  exit 1
fi

# Get current timestamp in the format we want
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo "Using timestamp: $TIMESTAMP"

# Step 1: Select rows with NULL values and export to CSV
echo "Exporting rows with NULL values to CSV..."
sqlite3 -header -csv "$DB_FILE" "SELECT * FROM $TABLE_NAME WHERE $NULL_COLUMN IS NULL;" > "$CSV_OUTPUT"

# Count rows for verification
ROW_COUNT=$(wc -l < "$CSV_OUTPUT")
ROW_COUNT=$((ROW_COUNT - 1))  # Subtract header row
echo "Found $ROW_COUNT rows with NULL values in $NULL_COLUMN"

if [ $ROW_COUNT -eq 0 ]; then
  echo "No NULL values found in column $NULL_COLUMN. No updates required."
  exit 0
fi

# Step 2: Update the CSV file to replace NULL with timestamp
echo "Updating CSV file with timestamp values..."

# First, identify which column index has the NULL values
HEADER=$(head -1 "$CSV_OUTPUT")
IFS=',' read -ra COLUMNS <<< "$HEADER"
COL_INDEX=-1
for i in "${!COLUMNS[@]}"; do
  if [[ "${COLUMNS[$i]}" == "$NULL_COLUMN" ]]; then
    COL_INDEX=$i
    break
  fi
done

if [[ $COL_INDEX -eq -1 ]]; then
  echo "Error: Column $NULL_COLUMN not found in CSV header"
  exit 1
fi

# Create a temporary file for the updated CSV
TEMP_CSV=$(mktemp)
head -1 "$CSV_OUTPUT" > "$TEMP_CSV"  # Copy the header

# Process each data row, replacing NULL with timestamp
tail -n +2 "$CSV_OUTPUT" | while IFS= read -r line; do
  # Split the line into fields
  IFS=',' read -ra FIELDS <<< "$line"
  
  # Replace the NULL value with timestamp (enclosed in quotes for CSV)
  FIELDS[$COL_INDEX]="\"$TIMESTAMP\""
  
  # Join fields back into a line
  NEW_LINE=$(IFS=,; echo "${FIELDS[*]}")
  echo "$NEW_LINE" >> "$TEMP_CSV"
done

# Replace the original CSV with the updated one
mv "$TEMP_CSV" "$CSV_OUTPUT"

# Step 3: Update the SQLite database with the same timestamp
echo "Updating database records..."
UPDATED_ROWS=$(sqlite3 "$DB_FILE" "UPDATE $TABLE_NAME SET $NULL_COLUMN = '$TIMESTAMP' WHERE $NULL_COLUMN IS NULL; SELECT changes();")

echo "Process completed successfully!"
echo "Updated $UPDATED_ROWS rows in database with timestamp: $TIMESTAMP"
echo "CSV file saved to: $CSV_OUTPUT"

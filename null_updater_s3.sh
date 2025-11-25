#!/bin/bash

# Configuration - modify these values
DB_FILE="mydata.db"
TABLE_NAME="tamper_log"  # Changed from tamper_logs to tamper_log
NULL_COLUMN="pushed_at"
CSV_OUTPUT="/tmp/output1.csv"  # Changed to /tmp to avoid permission issues
S3_BUCKET="calibris"
S3_PATH="/"  # Optional: path within the bucket
KEEP_LOCAL=false  # Set to true if you want to keep local copy

# Check if required arguments are provided
if [ $# -ge 4 ]; then
  DB_FILE=$1
  TABLE_NAME=$2
  NULL_COLUMN=$3
  CSV_OUTPUT=$4

  # Optional S3 parameters
  if [ $# -ge 5 ]; then
    S3_BUCKET=$5
  fi

  if [ $# -ge 6 ]; then
    S3_PATH=$6
  fi
fi

# Validate inputs
if [ ! -f "$DB_FILE" ]; then
  echo "Error: Database file $DB_FILE does not exist."
  echo "Usage: $0 database.db table_name null_column output.csv [s3_bucket] [s3_path]"
  exit 1
fi

# Get current timestamp in the format we want
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo "Using timestamp: $TIMESTAMP"

# Step 1: Select rows with NULL values and export to CSV
echo "Exporting rows with NULL values to CSV..."
sqlite3 -header -csv "$DB_FILE" "SELECT * FROM $TABLE_NAME WHERE $NULL_COLUMN IS NULL;" > "$CSV_OUTPUT"

# Count rows for verification
ROW_COUNT=$(wc -l < "$CSV_OUTPUT" || echo "0")
if [ "$ROW_COUNT" -gt 0 ]; then
  ROW_COUNT=$((ROW_COUNT - 1))  # Subtract header row
fi
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

# Step 4: Upload the CSV to AWS S3
echo "Uploading CSV to AWS S3..."
S3_DESTINATION="s3://$S3_BUCKET$S3_PATH$(basename "$CSV_OUTPUT")"
aws s3 cp "$CSV_OUTPUT" "$S3_DESTINATION"
UPLOAD_STATUS=$?

if [ $UPLOAD_STATUS -eq 0 ]; then
  echo "Successfully uploaded CSV to $S3_DESTINATION"

  # Clean up local file if not keeping
  if [ "$KEEP_LOCAL" = false ]; then
    echo "Removing local CSV file..."
    rm "$CSV_OUTPUT"
  fi
else
  echo "Error uploading to S3. Check your AWS configuration and permissions."
fi

echo "Process completed successfully!"
echo "Updated $UPDATED_ROWS rows in database with timestamp: $TIMESTAMP"

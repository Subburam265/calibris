#!/bin/bash

# Exit immediately if a command fails
set -e

# Default commit message if none is provided
COMMIT_MSG=${1:-"added changes"}

# Initialize repo if not already
if [ ! -d .git ]; then
  echo "Initializing new Git repository..."
  git init
fi

# Stage all changes
echo "Staging files..."
git add .

# Commit with message
echo "Committing with message: '$COMMIT_MSG'"
git commit -m "$COMMIT_MSG"

# Push to origin main
echo "Pushing to origin main..."
git push origin main

echo "✅ Push completed successfully."

#!/usr/bin/env bash
set -euo pipefail

echo "Checking sync status with upstream CrossPoint Reader..."
echo

git fetch upstream 2>/dev/null || {
  echo "ERROR: Cannot fetch upstream. Run: git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git"
  exit 1
}

BRANCH=$(git branch --show-current)
BEHIND=$(git rev-list --count HEAD..upstream/master 2>/dev/null || echo "?")
AHEAD=$(git rev-list --count upstream/master..HEAD 2>/dev/null || echo "?")

echo "  Branch:            $BRANCH"
echo "  Upstream:          upstream/master"
echo "  Commits behind:    $BEHIND"
echo "  Commits ahead:     $AHEAD"

if [ "$BEHIND" -gt 0 ] 2>/dev/null; then
  echo
  echo "⚠️  $BEHIND commit(s) behind upstream. Merge with:"
  echo "   git fetch upstream && git merge upstream/master"
elif [ "$BEHIND" -eq 0 ] 2>/dev/null; then
  echo
  echo "✅ In sync with upstream."
  LAST_TAG=$(git describe --tags --abbrev=0 upstream/master 2>/dev/null || echo "?")
  echo "   Latest upstream tag: $LAST_TAG"
fi

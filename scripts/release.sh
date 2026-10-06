#!/bin/bash
# Firmware release: set the version in src/version.h, commit, tag, push.
# GitHub Actions (.github/workflows/build.yml) builds the images and publishes the release.
#
#   scripts/release.sh 0.9.3
set -euo pipefail

VERSION="${1:?usage: scripts/release.sh X.Y.Z}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "the version is X.Y.Z" >&2; exit 1; }
TAG="v$VERSION"
cd "$(dirname "$0")/.."

[[ -z "$(git status --porcelain)" ]] || { echo "uncommitted changes — commit them first" >&2; git status --short; exit 1; }
git rev-parse "$TAG" >/dev/null 2>&1 && { echo "tag $TAG already exists" >&2; exit 1; }

sed -i.bak -E "s/#define FW_VERSION   \"[^\"]+\"/#define FW_VERSION   \"$VERSION\"/" src/version.h && rm src/version.h.bak
grep -m1 FW_VERSION src/version.h

git add src/version.h
git commit -m "Release $TAG" || true
git tag -a "$TAG" -m "firmware $TAG"
git push origin HEAD "$TAG"
echo "Pushed $TAG — GitHub Actions builds the release."

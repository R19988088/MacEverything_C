#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────
#  publish-opensource.sh
#
#  Prepares a sanitized copy of the MacEverything repository
#  for public open-source release under MIT license.
#
#  Usage:
#    ./scripts/publish-opensource.sh                 # dry-run (default)
#    ./scripts/publish-opensource.sh --push           # push to public remote
#    ./scripts/publish-opensource.sh --push --remote <name>
#
#  What it does:
#    1. Creates a temporary shallow clone of the repo
#    2. Removes internal/private files (binaries, AI config, internal docs)
#    3. Sanitizes personal paths (/Users/wujian → /Users/username)
#    4. Adds MIT LICENSE file
#    5. Updates README.md license section
#    6. Commits the sanitized state
#    7. Optionally pushes to a public remote
# ──────────────────────────────────────────────────────────
set -euo pipefail

# ── Configuration ────────────────────────────────────────

PUSH=false
PUBLIC_REMOTE="public"
BRANCH="main"

while [[ $# -gt 0 ]]; do
    case $1 in
        --push)    PUSH=true; shift ;;
        --remote)  PUBLIC_REMOTE="$2"; shift 2 ;;
        --branch)  BRANCH="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--push] [--remote <name>] [--branch <branch>]"
            echo ""
            echo "Options:"
            echo "  --push              Push to the public remote (default: dry-run)"
            echo "  --remote <name>     Remote name for public repo (default: public)"
            echo "  --branch <branch>   Branch name on public remote (default: main)"
            echo ""
            echo "Setup:"
            echo "  git remote add public git@github.com:YOUR_USER/MacEverything.git"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Resolve source repo ─────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Ensure we're in a git repo
if ! git -C "$REPO_ROOT" rev-parse --is-inside-work-tree &>/dev/null; then
    echo "ERROR: $REPO_ROOT is not a git repository"
    exit 1
fi

# Check we're on master
CURRENT_BRANCH=$(git -C "$REPO_ROOT" branch --show-current)
if [[ "$CURRENT_BRANCH" != "master" ]]; then
    echo "ERROR: Must run from master branch (currently on: $CURRENT_BRANCH)"
    exit 1
fi

# If --push, verify the public remote exists
if $PUSH; then
    if ! git -C "$REPO_ROOT" remote get-url "$PUBLIC_REMOTE" &>/dev/null; then
        echo "ERROR: Remote '$PUBLIC_REMOTE' not found."
        echo "Add it with: git remote add $PUBLIC_REMOTE git@github.com:YOUR_USER/MacEverything.git"
        exit 1
    fi
    REMOTE_URL=$(git -C "$REPO_ROOT" remote get-url "$PUBLIC_REMOTE")
    echo "Will push to: $REMOTE_URL ($PUBLIC_REMOTE/$BRANCH)"
fi

# ── Create temp work area ────────────────────────────────

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "==> Cloning to temporary directory..."
git clone --no-hardlinks "$REPO_ROOT" "$TMPDIR/repo" --quiet
cd "$TMPDIR/repo"

echo "==> Starting sanitization..."

# ── Step 1: Remove files that should not be public ───────

FILES_TO_REMOVE=(
    # Compiled binaries
    "mac_scanner"
    "string_search_bench"
    "string_search_bench.cpp"

    # AI/dev tooling config
    "CLAUDE.md"
    ".claude"

    # Internal planning docs
    "docs/superpowers"

    # Misplaced file (Homebrew README, not ours)
    "assets/README.md"
)

echo "  Removing private/internal files..."
for f in "${FILES_TO_REMOVE[@]}"; do
    if [[ -e "$f" ]]; then
        git rm -rf --quiet "$f"
        echo "    - $f"
    fi
done

# ── Step 2: Sanitize personal paths ──────────────────────

echo "  Sanitizing personal paths..."

# Files known to contain /Users/wujian references
SANITIZE_FILES=(
    "MacEverything/Core/SearchEngineQuery.cpp"
    "benchmarks/bench_structured_query.py"
    "tests/test_rescan_debounce.h"
    "tests/test_tilde_expansion.h"
    "docs/changelog/105-anchor-selection-optimization.md"
    "docs/changelog/117-tilde-expansion-in-query.md"
    "docs/changelog/118-extract-preprocessQuery-function.md"
    "docs/changelog/130-publish-opensource-script.md"
)

for f in "${SANITIZE_FILES[@]}"; do
    if [[ -f "$f" ]]; then
        # Case-insensitive replacement of /Users/wujian and /users/wujian
        sed -i '' -E 's|/[Uu]sers/wujian|/Users/username|g' "$f"
        # Also handle bare "wujian" in path context (e.g. "/wujian/data")
        sed -i '' -E 's|/wujian/|/username/|g' "$f"
        # Handle "Users/wujian" without leading slash (e.g. in descriptions)
        sed -i '' -E 's|Users/wujian|Users/username|g' "$f"
        git add "$f"
        echo "    - $f"
    fi
done

# ── Step 3: Create LICENSE file ──────────────────────────

echo "  Creating MIT LICENSE..."
YEAR=$(date +%Y)
cat > LICENSE <<EOF
MIT License

Copyright (c) 2024-$YEAR MacEverything Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
EOF
git add LICENSE

# ── Step 4: Update README.md license section ─────────────

echo "  Updating README.md license section..."
if [[ -f README.md ]]; then
    sed -i '' 's/私有项目，保留所有权利。/本项目采用 MIT 许可证，详见 [LICENSE](LICENSE) 文件。/' README.md
    git add README.md
fi

# ── Step 5: Update .gitignore ────────────────────────────

echo "  Updating .gitignore..."
cat >> .gitignore <<'GITIGNORE'

# Compiled binaries
mac_scanner
string_search_bench
test_runner

# Claude Code config (personal dev tooling)
CLAUDE.md
.claude/

# Internal dev docs
docs/superpowers/
GITIGNORE
git add .gitignore

# ── Step 6: Final verification ───────────────────────────

echo ""
echo "==> Verification..."

# Check no personal paths remain
LEAKS=$(git grep -i 'wujian' -- '*.cpp' '*.h' '*.py' '*.md' '*.swift' 2>/dev/null || true)
if [[ -n "$LEAKS" ]]; then
    echo "WARNING: Personal paths still found:"
    echo "$LEAKS"
    echo ""
    echo "Please update the SANITIZE_FILES list in this script."
    if $PUSH; then
        echo "Aborting push."
        exit 1
    fi
else
    echo "  OK: No personal paths found (git grep wujian = clean)"
fi

# Check excluded files are gone
for f in "${FILES_TO_REMOVE[@]}"; do
    if git ls-files --error-unmatch "$f" &>/dev/null; then
        echo "WARNING: $f still tracked!"
    fi
done
echo "  OK: All excluded files removed"

# Check LICENSE exists
if [[ -f LICENSE ]]; then
    echo "  OK: LICENSE file present"
else
    echo "ERROR: LICENSE file missing!"
    exit 1
fi

# ── Step 7: Commit ───────────────────────────────────────

echo ""
echo "==> Creating sanitized commit..."
git commit -m "chore: sanitize for open-source release (MIT)" --quiet

# ── Step 8: Push (or dry-run report) ─────────────────────

if $PUSH; then
    echo "==> Pushing to $PUBLIC_REMOTE/$BRANCH..."
    git push "$REPO_ROOT" HEAD:refs/heads/__publish_staging --force --quiet
    cd "$REPO_ROOT"
    git push "$PUBLIC_REMOTE" __publish_staging:"$BRANCH" --force
    git branch -D __publish_staging --quiet 2>/dev/null || true
    echo ""
    echo "Done! Pushed sanitized code to $PUBLIC_REMOTE/$BRANCH"
else
    echo ""
    echo "════════════════════════════════════════════"
    echo "  DRY RUN COMPLETE"
    echo "════════════════════════════════════════════"
    echo ""
    echo "Sanitized repo at: $TMPDIR/repo"
    echo ""
    echo "Changes made:"
    git log --oneline -1
    echo ""
    echo "Files in release:"
    git ls-files | head -40
    TOTAL=$(git ls-files | wc -l | tr -d ' ')
    echo "  ... ($TOTAL files total)"
    echo ""
    echo "To push for real:"
    echo "  1. Add a public remote:"
    echo "     git remote add public git@github.com:YOUR_USER/MacEverything.git"
    echo "  2. Run with --push:"
    echo "     ./scripts/publish-opensource.sh --push"
    echo ""
    # Keep temp dir alive for inspection in dry-run
    trap - EXIT
    echo "Temp dir preserved for inspection: $TMPDIR/repo"
    echo "(Delete manually when done: rm -rf $TMPDIR)"
fi

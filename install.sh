#!/usr/bin/env bash
# Installs FreeCAD (James Edition) for the current user:
#
#   curl -fsSL https://raw.githubusercontent.com/mrmaveric/FreeCAD/james/install.sh | bash
#
# Gets the tooling (the "james" branch of the repository) into
# ~/.local/opt/freecad-james/tooling, then installs the newest build with
# freecad-james-update. Options are passed on to "freecad-james-update update",
# e.g. "bash -s -- --pre" to include builds of release candidates.

set -euo pipefail

REPO_URL="${FREECAD_JAMES_REPO_URL:-https://github.com/mrmaveric/FreeCAD.git}"
ROOT="${FREECAD_JAMES_ROOT:-$HOME/.local/opt/freecad-james}"
TOOLING="$ROOT/tooling"

for tool in git curl python3 sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool is required" >&2; exit 1; }
done

mkdir -p "$ROOT"
if [[ -d "$TOOLING/.git" ]]; then
    git -C "$TOOLING" pull --quiet --ff-only
else
    git clone --quiet --branch james --single-branch "$REPO_URL" "$TOOLING"
fi

# The updater of the first installations lived in bin/
if [[ -f "$ROOT/bin/freecad-james-update" && ! -L "$ROOT/bin/freecad-james-update" ]]; then
    ln -sfn "$TOOLING/freecad-james-update" "$ROOT/bin/freecad-james-update"
fi

mkdir -p "$HOME/.local/bin"
ln -sfn "$TOOLING/freecad-james-update" "$HOME/.local/bin/freecad-james-update"
JAMES_NO_SELF_UPDATE=1 "$TOOLING/freecad-james-update" update "$@"

case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *) echo "Add ~/.local/bin to your PATH to run freecad-james and freecad-james-update by name." ;;
esac

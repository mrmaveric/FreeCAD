# shellcheck shell=bash disable=SC2034  # variables are used by the scripts that source this, arrays by name
# james.sh - the steps of a FreeCAD (James Edition) build, shared by
# freecad-james-update and the GitHub workflow (packaging/ci.sh). Source it.
#
# The functions are also called where bash ignores "set -e" (in "if" and
# "||"), so they check for failures themselves.
#
# A James Edition build is a FreeCAD release tag with the commits of the
# feature branch cherry-picked onto it, built and packaged with FreeCAD's own
# release scripts (package/bundle). Only the name, icon, desktop identity and
# the update information of the packages are changed.

JAMES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_NAME="FreeCAD (James Edition)"
APP_ID="freecad-james"
FILE_PREFIX="FreeCAD_James_Edition"
GITHUB_REPO="${GITHUB_REPO:-${GITHUB_REPOSITORY:-mrmaveric/FreeCAD}}"
FEATURE_BRANCH="${FEATURE_BRANCH:-feature/section-view}"
UPSTREAM="https://github.com/FreeCAD/FreeCAD.git"
# Releases older than this are never built automatically
OLDEST_VERSION="26.3rc2"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$(uname -m).AppImage"
JAMES_CACHE="${JAMES_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/freecad-james}"

STABLE_REGEX='^[0-9]+\.[0-9]+(\.[0-9]+)?$'
PRE_REGEX='^[0-9]+\.[0-9]+(\.[0-9]+)?(rc[0-9]+)?$'

if [[ -z "${PIXI:-}" ]]; then
    PIXI="$(command -v pixi || echo "$HOME/.pixi/bin/pixi")"
fi

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# Sorts versions and build names, release candidates before their release
sort_versions() { sed -E 's/rc([0-9]+)/~rc\1/' | sort -V | sed 's/~rc/rc/'; }

is_windows() { [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; }

# Whether version $1 is newer than $2
version_gt() {
    [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort_versions | tail -n 1)" == "$1" ]]
}

# ---------------------------------------------------------------------------
# Source

# The feature commits: what refs/remotes/patch/feature has on top of FreeCAD's
# main branch (refs/remotes/origin/main) in repository $1
feature_commits() {
    local base
    base="$(git -C "$1" merge-base refs/remotes/origin/main refs/remotes/patch/feature)"
    git -C "$1" rev-list --reverse --no-merges "$base..refs/remotes/patch/feature"
}

# Identifies release $2 with the current feature commits in repository $1
feature_stamp() {
    {
        git -C "$1" rev-parse "refs/tags/$2^{commit}"
        feature_commits "$1"
    } | sha1sum | cut -d' ' -f1
}

# Resets the work tree $1 to the release tag $2 and cherry-picks the feature
# commits. Conflicts resolved before are resolved again by git rerere where it
# is enabled. Returns 2, with the cherry-pick stopped, if conflicts are left.
apply_feature() {
    local wt="$1" version="$2"
    local -a commits
    mapfile -t commits < <(feature_commits "$wt")
    ((${#commits[@]})) || die "No feature commits found on $FEATURE_BRANCH"

    # The same commits everywhere, so all builds of a release show one hash
    local -a identity=(
        GIT_COMMITTER_NAME="FreeCAD James Edition"
        GIT_COMMITTER_EMAIL="james-edition@users.noreply.github.com"
        GIT_COMMITTER_DATE="$(git -C "$wt" log -1 --format=%cI "${commits[-1]}")"
    )

    log "Applying ${#commits[@]} feature commit(s) onto FreeCAD $version"
    git -C "$wt" cherry-pick --quit 2>/dev/null || true
    git -C "$wt" reset --quiet --hard "refs/tags/$version" || return 1
    if env "${identity[@]}" git -C "$wt" cherry-pick "${commits[@]}" >/dev/null; then
        return 0
    fi
    while git -C "$wt" rev-parse -q --verify CHERRY_PICK_HEAD >/dev/null; do
        if [[ -n "$(git -C "$wt" diff --name-only --diff-filter=U)" ]]; then
            warn "Conflicts in: $(git -C "$wt" diff --name-only --diff-filter=U | tr '\n' ' ')"
            return 2
        fi
        log "Conflict resolved with a recorded resolution"
        git -C "$wt" add -A
        env "${identity[@]}" GIT_EDITOR=true git -C "$wt" cherry-pick --continue >/dev/null || true
    done
}

# ---------------------------------------------------------------------------
# Build

# Builds FreeCAD in the source tree $1 and installs it into its pixi
# environment, with FreeCAD's release build script
build_source() {
    (
        cd "$1" || exit 1
        git submodule update --init --recursive || exit 1
        "$PIXI" install --frozen || exit 1
        is_windows && { _qt_tool_dlls add || exit 1; }
        export CCACHE_BASEDIR="$PWD" CCACHE_NOHASHDIR=1
        export CMAKE_BUILD_PARALLEL_LEVEL="${JOBS:-$(nproc)}"
        # Without FreeCAD's developer tests. CMAKE_ARGS is set by the
        # activation of the environment.
        cat >.james-build.sh <<'EOF'
CMAKE_ARGS="${CMAKE_ARGS:-} -DENABLE_DEVELOPER_TESTS=OFF" exec bash package/bundle/build.sh
EOF
        local status=0
        "$PIXI" run --frozen "bash .james-build.sh" || status=$?
        is_windows && _qt_tool_dlls remove
        exit "$status"
    )
}

# On Windows, the Qt tools of FreeCAD 26.3 (moc, rcc, uic, lrelease, ... in
# Library/lib/qt6 of the environment) don't find their DLLs when the build
# runs them on the GitHub runners since October 2026 (exit code 0xc0000135),
# not even in FreeCAD's own workflow. Copies of the DLLs next to them are
# found. "add" makes the copies in the source tree's environment, "remove"
# deletes them again, so they don't end up in the package.
_qt_tool_dlls() {
    local qt=".pixi/envs/default/Library/lib/qt6" list=".james-qt-dlls" dir dll
    case "$1" in
        add)
            : >"$list"
            while read -r dir; do
                for dll in .pixi/envs/default/Library/bin/*.dll; do
                    [[ -e "$dir/${dll##*/}" ]] && continue
                    cp "$dll" "$dir/" || return 1
                    echo "$dir/${dll##*/}" >>"$list"
                done
            done < <(find "$qt" -name '*.exe' -printf '%h\n' | sort -u)
            ;;
        remove)
            [[ -f "$list" ]] || return 0
            while read -r dll; do
                rm -f "$dll"
            done <"$list"
            rm -f "$list"
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Packages

appimage_name() { echo "$FILE_PREFIX-$1-$(uname -m).AppImage"; }
windows_name() { echo "$FILE_PREFIX-$1-Windows-$(uname -m)"; }

desktop_entry() {
    local exec="$1" icon="$2" version="$3"
    cat <<EOF
[Desktop Entry]
Type=Application
Name=$APP_NAME
GenericName=CAD Application
Comment=FreeCAD $version with section views
Exec=$exec
Icon=$icon
Terminal=false
StartupNotify=true
StartupWMClass=FreeCAD
Categories=Graphics;3DGraphics;
Keywords=CAD;3D;Parametric;Section;
MimeType=application/x-extension-fcstd;model/obj;model/step;model/step+zip;model/iges;application/iges;model/stl;
X-AppImage-Version=$version
EOF
}

appimagetool() {
    local tool
    tool="$JAMES_CACHE/appimagetool-$(uname -m).AppImage"
    if [[ ! -x "$tool" ]]; then
        mkdir -p "$JAMES_CACHE"
        curl -fsSL -o "$tool.part" "$APPIMAGETOOL_URL"
        chmod +x "$tool.part"
        mv "$tool.part" "$tool"
    fi
    echo "$tool"
}

# Tools FreeCAD's scripts expect on the system, taken from conda-forge when
# they are missing. Prints a directory to append to PATH.
tool_shims() {
    local shims="$JAMES_CACHE/shims" entry
    mkdir -p "$shims"
    for entry in msgfmt:gettext file:file; do
        printf '#!/bin/sh\nexec "%s" exec --spec %s -- %s "$@"\n' \
            "$PIXI" "${entry#*:}" "${entry%%:*}" >"$shims/${entry%%:*}"
        chmod +x "$shims/${entry%%:*}"
    done
    echo "$shims"
}

# Packages the installed environment of the source tree $1 as the AppImage
# $3/$(appimage_name $2) with FreeCAD's AppImage script
bundle_linux() {
    local src="$1" id="$2" out="$3"
    local dir="$src/package/bundle/linux" name tool shims
    name="$(appimage_name "$id")"
    tool="$(appimagetool)"
    shims="$(tool_shims)"

    git -C "$src" checkout --quiet -- package/bundle/linux || return 1
    git -C "$src" clean -fdxq package/bundle/linux || return 1
    # No update information (it would point to the official FreeCAD
    # releases), our name and appimagetool
    sed -e '/gh-releases-zsync/d' \
        -e 's#^curl -LO .*#: appimagetool is provided by james.sh#' \
        -e 's#^chmod a+x appimagetool.*#:#' \
        -e 's#\./appimagetool-\$(uname -m)\.AppImage#"${JAMES_APPIMAGETOOL}"#' \
        -e 's#^version_name=.*#version_name="${JAMES_APPIMAGE_NAME%.AppImage}"#' \
        -e '/Create the appimage/i source ./james-edition.sh' \
        "$dir/create_bundle.sh" >"$dir/create_bundle_james.sh"
    if grep -q 'gh-releases-zsync' "$dir/create_bundle_james.sh" \
        || ! grep -q 'source ./james-edition.sh' "$dir/create_bundle_james.sh" \
        || ! grep -q 'JAMES_APPIMAGETOOL' "$dir/create_bundle_james.sh"; then
        die "FreeCAD's linux/create_bundle.sh changed, packaging/james.sh needs to be adapted"
    fi

    cp "$JAMES_DIR/freecad-james.svg" "$dir/AppDir/$APP_ID.svg"
    desktop_entry "AppRun %F" "$APP_ID" "$id" >"$dir/james.desktop"
    cat >"$dir/james-edition.sh" <<'EOF'
# Runs inside create_bundle.sh, before the AppImage is created
rm -f AppDir/org.freecad.FreeCAD.desktop AppDir/org.freecad.FreeCAD.svg AppDir/.DirIcon
rm -f AppDir/usr/share/metainfo/org.freecad.FreeCAD.metainfo.xml
cp james.desktop "AppDir/${JAMES_APP_ID}.desktop"
mkdir -p AppDir/usr/share/applications
cp james.desktop "AppDir/usr/share/applications/${JAMES_APP_ID}.desktop"
# The edition name and desktop identity, read by FreeCAD at startup
cp "${JAMES_BRANDING}" AppDir/usr/bin/branding.xml
EOF

    (
        cd "$src/package/bundle" || exit 1
        # FreeCAD's script calls pixi by name. The shims come last, so tools of
        # the environment or the system are preferred.
        PATH="$(dirname "$PIXI"):$PATH:$shims"
        ARCH="$(uname -m)"
        export PATH ARCH
        export JAMES_APPIMAGETOOL="$tool" JAMES_APPIMAGE_NAME="$name" JAMES_APP_ID="$APP_ID"
        export JAMES_BRANDING="$JAMES_DIR/branding.xml"
        export BUILD_TAG="$id" VERSION="$id" APPIMAGE_EXTRACT_AND_RUN=1
        "$PIXI" run --frozen bash -c "cd linux && bash create_bundle_james.sh"
    )
    [[ -f "$dir/$name" ]] || die "No AppImage was created"
    mkdir -p "$out"
    mv -f "$dir/$name" "$out/$name"
    # The copy of the environment is big and not needed any more
    rm -rf "$dir/AppDir/usr"
}

# Packages the installed environment of the source tree $1 as the portable
# Windows build $3/$(windows_name $2).7z with FreeCAD's Windows script. Leaves
# the unpacked build in the source tree for testing.
bundle_windows() {
    local src="$1" id="$2" out="$3"
    local dir="$src/package/bundle/windows" name
    name="$(windows_name "$id")"

    git -C "$src" checkout --quiet -- package/bundle/windows || return 1
    git -C "$src" clean -fdxq package/bundle/windows || return 1
    # The edition name, read by FreeCAD at startup
    sed -e 's#^mv \${copy_dir} \${version_name}$#cp "${JAMES_BRANDING}" ${copy_dir}/bin/branding.xml\n&#' \
        "$dir/create_bundle.sh" >"$dir/create_bundle_james.sh"
    grep -q 'JAMES_BRANDING' "$dir/create_bundle_james.sh" \
        || die "FreeCAD's windows/create_bundle.sh changed, packaging/james.sh needs to be adapted"

    (
        cd "$src/package/bundle" || exit 1
        # FreeCAD's script names the build FreeCAD_${BUILD_TAG}-Windows-<arch>.
        # Its installer would replace an installed official FreeCAD.
        BUILD_TAG="${name#FreeCAD_}"
        export BUILD_TAG="${BUILD_TAG%-Windows-*}"
        export MAKE_INSTALLER=false UPLOAD_RELEASE=false WINDOWS_SIGN_RELEASE=0
        export JAMES_BRANDING="$JAMES_DIR/branding.xml"
        "$PIXI" run --frozen bash -c "cd windows && bash create_bundle_james.sh"
    )
    [[ -f "$dir/$name.7z" ]] || die "No Windows build was created"
    mkdir -p "$out"
    mv -f "$dir/$name.7z" "$out/$name.7z"
}

# ---------------------------------------------------------------------------
# Tests

# Runs FreeCAD test module $2 with the command $1 (an array name), in an empty
# user directory
_run_test() {
    local -n cmd="$1"
    local module="$2" home logfile ok=0
    home="$(mktemp -d)"
    logfile="$home/$module.log"
    FREECAD_USER_HOME="$home" "${cmd[@]}" -t "$module" >"$logfile" 2>&1 || true
    grep -q '^OK' "$logfile" && ok=1
    if ((!ok)); then
        tail -n 40 "$logfile" >&2
        warn "$module failed"
    fi
    rm -rf "$home"
    ((ok))
}

# Checks that the program shows the edition name, then runs the section view
# tests. $1 runs freecadcmd, $2 (optional) runs freecad.
_test_build() {
    local -n cmd_app="$1"
    local edition home
    home="$(mktemp -d)"
    edition="$(FREECAD_USER_HOME="$home" "${cmd_app[@]}" -c \
        "import FreeCAD; print('edition=' + FreeCAD.ConfigGet('BuildEdition'))" 2>&1 \
        | sed -n 's/^edition=//p' | tr -d '\r')"
    rm -rf "$home"
    [[ "$edition" == "James Edition" ]] || { warn "The build doesn't show 'James Edition' ('$edition')"; return 1; }
    _run_test "$1" TestSectionView || return 1
    if [[ -n "${2:-}" ]]; then
        if command -v xvfb-run >/dev/null 2>&1; then
            local -n cmd_gui="$2"
            local -a xvfb_gui=(xvfb-run -a -s "-screen 0 1600x1000x24" "${cmd_gui[@]}")
            _run_test xvfb_gui TestSectionViewGui || return 1
        else
            warn "xvfb-run is not available, skipping the 3D view tests"
        fi
    fi
}

test_appimage() {
    # The AppImage unpacks itself (3 GB) into TMPDIR for every run and leaves
    # it there, so the tests use a temporary folder of their own, which is
    # removed afterwards. /tmp is often small.
    local tmp status=0
    mkdir -p "$JAMES_CACHE"
    tmp="$(mktemp -d "$JAMES_CACHE/test.XXXXXX")" || return 1
    local -a app=(env APPIMAGE_EXTRACT_AND_RUN=1 "$1" freecadcmd)
    local -a gui=(env APPIMAGE_EXTRACT_AND_RUN=1 "$1")
    (
        export TMPDIR="$tmp"
        _test_build app gui
    ) || status=1
    rm -rf "$tmp"
    return "$status"
}

test_windows() {
    local -a app=("$1/bin/freecadcmd.exe")
    _test_build app
}

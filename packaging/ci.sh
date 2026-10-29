#!/usr/bin/env bash
# ci.sh - the steps of the GitHub workflow .github/workflows/release.yml
#
#   ci.sh plan                         Which releases to build, as JSON for the
#                                      build matrix (env VERSION, REBUILD)
#   ci.sh source VERSION REF SHA DIR   FreeCAD VERSION with the feature, in DIR,
#                                      and the pixi version to build it with
#   ci.sh build DIR                    Build and install it
#   ci.sh package DIR TAG VERSION OUT  Package and test it for this platform
#   ci.sh publish TAG VERSION PRERELEASE SHA REF DIST
#                                      Create the GitHub release

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/james.sh"

summary() { [[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && cat >>"$GITHUB_STEP_SUMMARY" || cat >&2; }

# A step output; stdout is the log
output() { echo "$1" >>"${GITHUB_OUTPUT:-/dev/stdout}"; }

is_windows() { [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; }

# The newest stable release and the newest release candidate, if it is newer.
# A release with a James Edition build of the same feature commits is
# skipped, unless REBUILD is true.
cmd_plan() {
    local tags stable pre
    tags="$(git ls-remote --tags --refs "$UPSTREAM" | sed 's#.*refs/tags/##')"
    local -a targets=()
    if [[ -n "${VERSION:-}" ]]; then
        grep -qxF -- "$VERSION" <<<"$tags" || die "$VERSION is not a FreeCAD release tag"
        targets=("$VERSION")
    else
        stable="$(grep -E "$STABLE_REGEX" <<<"$tags" | sort_versions | tail -n 1)"
        pre="$(grep -E "$PRE_REGEX" <<<"$tags" | sort_versions | tail -n 1)"
        for v in "$stable" "$pre"; do
            [[ -n "$v" ]] || continue
            version_gt "$OLDEST_VERSION" "$v" && continue
            [[ " ${targets[*]} " == *" $v "* ]] || targets+=("$v")
        done
    fi

    local releases repo_url="https://github.com/$GITHUB_REPO.git"
    releases="$(gh release list -R "$GITHUB_REPO" --limit 500 --json tagName --jq '.[].tagName')"
    local builds="[]" v ref sha last n tag prerelease
    for v in "${targets[@]}"; do
        # A branch james/VERSION is a complete source made by hand, e.g. with
        # resolved conflicts. Otherwise the feature branch is applied.
        ref="james/$v"
        sha="$(git ls-remote "$repo_url" "refs/heads/$ref" | cut -f1)"
        if [[ -z "$sha" ]]; then
            ref="$FEATURE_BRANCH"
            sha="$(git ls-remote "$repo_url" "refs/heads/$ref" | cut -f1)"
        fi
        [[ -n "$sha" ]] || die "$GITHUB_REPO has neither james/$v nor $FEATURE_BRANCH"

        last="$(grep -E "^${v//./\\.}-james\.[0-9]+$" <<<"$releases" | sort -V | tail -n 1 || true)"
        n=0
        if [[ -n "$last" ]]; then
            n="${last##*.}"
            if [[ "${REBUILD:-false}" != "true" ]] \
                && gh release view "$last" -R "$GITHUB_REPO" --json body --jq .body | grep -qF "feature=$sha"; then
                echo "FreeCAD $v: $last is up to date" | summary
                continue
            fi
        fi
        tag="$v-james.$((n + 1))"
        prerelease=false
        [[ "$v" =~ $STABLE_REGEX ]] || prerelease=true
        builds="$(jq -c --arg v "$v" --arg t "$tag" --arg r "$ref" --arg s "$sha" --argjson p "$prerelease" \
            '. + [{version: $v, tag: $t, ref: $r, sha: $s, prerelease: $p}]' <<<"$builds")"
        echo "FreeCAD $v: building $tag from $ref ($sha)" | summary
    done
    output "builds=$builds"
}

cmd_source() {
    local version="$1" ref="$2" sha="$3" dir="$4"
    if is_windows; then
        # FreeCAD's scripts must keep their line endings
        git config --global core.autocrlf false
        git config --global core.longpaths true
    fi
    git clone --quiet --filter=blob:none --no-checkout --single-branch --branch main "$UPSTREAM" "$dir"
    git -C "$dir" fetch --quiet --filter=blob:none origin "refs/tags/$version:refs/tags/$version"
    # Without a filter: the clone has the files of FreeCAD's history, and only
    # the files of the feature commits are new. git couldn't fetch missing
    # ones later, it asks FreeCAD's repository for them.
    git -C "$dir" fetch --quiet --no-tags "https://github.com/$GITHUB_REPO.git" \
        "+refs/heads/$ref:refs/remotes/patch/feature"
    # The commit the plan was made for, even if the branch moved since
    git -C "$dir" cat-file -e "$sha^{commit}" 2>/dev/null \
        && git -C "$dir" update-ref refs/remotes/patch/feature "$sha"
    git -C "$dir" config rerere.enabled false

    if [[ "$ref" == "james/$version" ]]; then
        git -C "$dir" checkout --quiet --detach refs/remotes/patch/feature
        git -C "$dir" merge-base --is-ancestor "refs/tags/$version" HEAD \
            || die "james/$version is not based on FreeCAD $version"
    else
        local status=0
        apply_feature "$dir" "$version" || status=$?
        if ((status)); then
            summary <<EOF
### The feature commits conflict with FreeCAD $version

Resolve the conflicts on your computer and publish the result:

\`\`\`bash
freecad-james-update build $version
# edit the conflicting files, then in the source tree it names:
git add <files> && git cherry-pick --continue
freecad-james-update build $version --resume
freecad-james-update publish $version
\`\`\`
EOF
            die "The feature commits conflict with FreeCAD $version"
        fi
    fi
    git -C "$dir" log --oneline -n 5

    # The pixi version FreeCAD builds this release with
    local pixi
    pixi="$(grep -oE 'pixi-version: *v[0-9.]+' "$dir/.github/workflows/build_release.yml" 2>/dev/null \
        | head -n 1 | sed 's/.*: *//' || true)"
    output "pixi_version=${pixi:-latest}"
}

cmd_build() {
    build_source "$1"
}

cmd_package() {
    local src="$1" tag="$2" version="$3" out="$4"
    mkdir -p "$out"
    if is_windows; then
        bundle_windows "$src" "$tag" "$out"
        test_windows "$src/package/bundle/windows/$(windows_name "$tag")" || die "The Windows build failed its tests"
        return
    fi

    bundle_linux "$src" "$tag" "$out"
    test_appimage "$out/$(appimage_name "$tag")" || die "The AppImage failed its tests"

    # The corresponding source, as FreeCAD publishes it with its releases
    local tarball prefix="freecad-james-$tag/"
    tarball="$(cd "$out" && pwd)/freecad-james-source-$tag.tar"
    git -C "$src" archive --prefix="$prefix" -o "$tarball" HEAD
    git -C "$src" submodule foreach --quiet --recursive \
        "git archive --prefix=\"$prefix\$displaypath/\" -o \"\$toplevel/.sub.tar\" HEAD \
         && tar -A -f \"$tarball\" \"\$toplevel/.sub.tar\" && rm \"\$toplevel/.sub.tar\""
    gzip -9 "$tarball"

    # For the release notes, the commits as they are on GitHub: those of a
    # james/VERSION branch, or the feature commits (their cherry-picks only
    # exist in this build)
    local range="refs/tags/$version..HEAD"
    if ! git -C "$src" merge-base --is-ancestor HEAD refs/remotes/patch/feature; then
        range="$(git -C "$src" merge-base refs/remotes/origin/main refs/remotes/patch/feature)..refs/remotes/patch/feature"
    fi
    git -C "$src" log --reverse --no-merges \
        --format='- [`%h`](https://github.com/'"$GITHUB_REPO"'/commit/%H) %s' "$range" >"$out/feature-commits.md"
}

cmd_publish() {
    local tag="$1" version="$2" prerelease="$3" sha="$4" ref="$5" dist="$6"
    local appimage windows source n title
    appimage="$(appimage_name "$tag")"
    windows="$(windows_name "$tag").7z"
    source="freecad-james-source-$tag.tar.gz"
    [[ -f "$dist/$appimage" ]] || die "There is no Linux build of $tag, not publishing it"

    local -a files=("$dist/$appimage" "$dist/$source")
    local windows_line="- **Windows:** the build failed, see the [workflow run]($GITHUB_SERVER_URL/$GITHUB_REPO/actions/runs/$GITHUB_RUN_ID)."
    if [[ -f "$dist/$windows" ]]; then
        files+=("$dist/$windows")
        windows_line="- **Windows:** \`$windows\`. Unpack it and run \`bin\\FreeCAD.exe\`. It isn't code-signed, so Windows warns the first time it starts."
    fi
    (cd "$dist" && sha256sum -- "${files[@]##*/}" >SHA256SUMS)
    files+=("$dist/SHA256SUMS")

    n="${tag##*.}"
    title="FreeCAD $version (James Edition)"
    ((n > 1)) && title+=" build $n"
    {
        cat <<EOF
FreeCAD [$version](https://github.com/FreeCAD/FreeCAD/releases/tag/$version) with the section view feature.

- **Linux:** \`$appimage\`. Install it with \`freecad-james-update\` (see the [README]($GITHUB_SERVER_URL/$GITHUB_REPO/tree/james#readme)), or make it executable and run it.
$windows_line
- **Source:** \`$source\`, the exact source of these builds.

Built from \`$ref\` at [\`${sha:0:10}\`]($GITHUB_SERVER_URL/$GITHUB_REPO/commit/$sha):

EOF
        cat "$dist/feature-commits.md" 2>/dev/null || true
        echo
        echo "<!-- james-edition version=$version feature=$sha -->"
    } >"$dist/notes.md"

    local -a flags=()
    [[ "$prerelease" == "true" ]] && flags+=(--prerelease)
    gh release create "$tag" -R "$GITHUB_REPO" --target "$GITHUB_SHA" --title "$title" \
        --notes-file "$dist/notes.md" "${flags[@]}" "${files[@]}"
    echo "Published [$title]($GITHUB_SERVER_URL/$GITHUB_REPO/releases/tag/$tag)" | summary
}

command="${1:-}"
shift || true
case "$command" in
    plan) cmd_plan ;;
    source) cmd_source "$@" ;;
    build) cmd_build "$@" ;;
    package) cmd_package "$@" ;;
    publish) cmd_publish "$@" ;;
    *) die "Unknown step '$command'" ;;
esac

# FreeCAD (James Edition)

FreeCAD releases with one addition: a non-destructive **section view** like the
ones in SolidWorks, Onshape and Fusion 360. It cuts whole assemblies with
movable planes or boxes, and the cut faces are filled and cross-hatched.

This repository is a fork of [FreeCAD](https://github.com/FreeCAD/FreeCAD).
Builds for Linux and Windows are on the
[releases page](https://github.com/mrmaveric/FreeCAD/releases).

## Install on Linux

```bash
curl -fsSL https://raw.githubusercontent.com/mrmaveric/FreeCAD/james/install.sh | bash
```

That installs the newest build as an AppImage in `~/.local/opt/freecad-james`,
with the menu entry *FreeCAD (James Edition)* and the command `freecad-james`.
It needs `git`, `curl` and `python3`. Settings are shared with the official
FreeCAD, except with its Flatpak, which keeps its own.

Later, `freecad-james-update` installs a newer build when there is one:

| Command | |
|---|---|
| `freecad-james-update` | Install the newest build |
| `freecad-james-update check` | Is there a newer build? |
| `freecad-james-update list` | The installed builds, `*` is the current one |
| `freecad-james-update use 26.3-james.1` | Go back to another installed build |
| `freecad-james-update remove 26.3-james.1` | Delete a build |
| `freecad-james-update timer on\|off` | Check weekly and show a notification |
| `freecad-james-update default-app` | Open `.FCStd` files with James Edition |
| `freecad-james-update desktop` | Reinstall the launcher and menu entry |
| `freecad-james-update --help` | All commands and options |

Builds of release candidates are only installed with `--pre`, or while there
is no build of a final release.

## Install on Windows

Download `FreeCAD_James_Edition-…-Windows-x86_64.7z` from the
[releases page](https://github.com/mrmaveric/FreeCAD/releases), unpack it
and run `bin\FreeCAD.exe`. The builds aren't code-signed, so Windows warns the
first time it starts ("More info", "Run anyway").

## How it works

| Branch | |
|---|---|
| `feature/section-view` | The feature: FreeCAD's `main` plus its commits. All development happens here. |
| `james` | This tooling, the default branch: the installer, `freecad-james-update`, the packaging and the workflow. |
| `james/VERSION` | Optional. The source of one release made by hand, e.g. with resolved conflicts. |
| `main` | FreeCAD's `main`, as it was when the fork was made. Not used. |

The workflow [`release.yml`](.github/workflows/release.yml) runs daily. It looks
for the newest FreeCAD release (and the newest release candidate, if that is
newer). If there is no build of it with the current feature commits, it:

1. takes the release tag from FreeCAD and cherry-picks the commits of
   `feature/section-view` onto it, or uses `james/VERSION` if that exists,
2. builds it for Linux and Windows the way FreeCAD builds its releases
   (`package/bundle` and the pixi environment of the release),
3. packages it with FreeCAD's scripts, changed only in name, icon and desktop
   identity, without the update information that points to official FreeCAD,
4. runs the section view tests on the packages,
5. publishes a release `VERSION-james.N` with the AppImage, the Windows build,
   the exact source and checksums.

The release `26.3-james.2` is the second build of FreeCAD 26.3, made after the
feature changed. Builds show "(James Edition)" after the version in the title
bar, the About dialog and the splash screen, set by
[`packaging/branding.xml`](packaging/branding.xml).

The shared build steps are in [`packaging/james.sh`](packaging/james.sh), used
by the workflow ([`packaging/ci.sh`](packaging/ci.sh)) and by
`freecad-james-update build`.

## Changing the feature

Work in a clone of this repository on `feature/section-view`. To try a change,
build it on your computer (needs [pixi](https://pixi.sh)):

```bash
freecad-james-update build          # newest release, with the feature from PATCH_REPO
freecad-james-update build 26.3     # a specific release
```

That builds FreeCAD with the feature commits of the clone (default
`~/temp/FreeCAD`, or GitHub if that doesn't exist; set `PATCH_REPO` in
`~/.local/opt/freecad-james/config`), tests it and installs it as build
`26.3-local`. The first build takes about an hour, later ones are quicker
because the source tree and the compiler cache are kept.

When it's good, push `feature/section-view`. The next daily run builds and
publishes it. `freecad-james-update ci` starts it right away.

## When the feature conflicts with a new release

The run fails, and GitHub emails you. Resolve it on your computer:

```bash
freecad-james-update build 26.4
# edit the files 'git status' lists in the source tree it names, then there:
git add <files> && git cherry-pick --continue
freecad-james-update build 26.4 --resume
freecad-james-update publish 26.4
```

`publish` pushes the resolved source as `james/26.4` and builds it on GitHub.
git remembers the resolution (rerere), so the next release with the same
conflict is resolved automatically by `freecad-james-update build`. Claude Code
can do the resolving too.

## Good to know

- GitHub turns off the daily run of a public repository after 60 days without
  commits, and emails before it does. Turn it on again in the Actions tab, or
  push something.
- Files keep working with the official FreeCAD, except that section views are
  dropped when such a file is opened there.
- `freecad-james-update` updates itself from this branch before it installs a
  build.
- To remove everything: `freecad-james-update timer off`, then delete
  `~/.local/opt/freecad-james`, `~/.local/bin/freecad-james`,
  `~/.local/bin/freecad-james-update`,
  `~/.local/share/applications/freecad-james.desktop` and
  `~/.local/share/icons/hicolor/scalable/apps/freecad-james.svg`.

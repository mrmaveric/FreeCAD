# Runs only the configure step of FreeCAD's package/bundle/build.sh into
# build/$1, with the cmake $2 (default: the one found on PATH)
dir="$1" cmake="${2:-cmake}"
sed -e "s#-B build/bundle-release#-B build/$dir#" \
    -e "s#^cmake \\\\#\"$cmake\" \\\\#" \
    -e '/^cmake --build/,$d' package/bundle/build.sh >".configure-$dir.sh"
grep -n "^\"\?[^ ]*cmake\"\? \\\\" ".configure-$dir.sh" | head -n 2
echo "  cmake: $(command -v "$cmake") $("$cmake" --version | head -n 1)"
if bash ".configure-$dir.sh" >"build-$dir.log" 2>&1; then
    echo "  $dir: configure and generate OK"
else
    echo "  $dir: FAILED"
    tail -n 12 "build-$dir.log" | sed 's/^/    /'
fi

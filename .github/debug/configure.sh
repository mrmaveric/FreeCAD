# Runs only the configure step of FreeCAD's package/bundle/build.sh into
# build/$1, then tells whether generating worked
dir="$1"
sed -e "s#-B build/bundle-release#-B build/$dir#" \
    -e '/^cmake --build/,$d' package/bundle/build.sh >".configure-$dir.sh"
echo "  cmake: $(command -v cmake) $(cmake --version | head -n 1)"
if bash ".configure-$dir.sh" >"build-$dir.log" 2>&1; then
    echo "  $dir: configure and generate OK"
else
    echo "  $dir: FAILED"
    grep -E "CMake Error|moc|0xc0" "build-$dir.log" | head -n 5
fi

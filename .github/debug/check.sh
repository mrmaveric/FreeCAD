d="$(dirname "$0")"
echo "  bash PATH: ${#PATH} characters"
cmake -P "$d/check.cmake"
python "$d/diag.py" | grep -E "PATH:|moc:"

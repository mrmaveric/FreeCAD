echo "  bash PATH starts: $(echo "$PATH" | tr ':' ' ' | cut -c1-300)"
python "$(dirname "$0")/diag.py"

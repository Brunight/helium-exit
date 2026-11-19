#!/bin/sh
# Installs helium-exit for the current user. Undo: rm ~/.local/bin/helium-browser
set -e
D="$HOME/.local/lib/helium-exit"
mkdir -p "$D" "$HOME/.local/bin"
cp "$(dirname "$0")/helium_exit.c" "$D/"
gcc -O2 -Wall -Wextra -shared -fPIC -o "$D/libhelium_exit.so" "$D/helium_exit.c"
cat > "$HOME/.local/bin/helium-browser" <<'EOF'
#!/bin/sh
# Loads helium-exit (puts "Exit" back in Helium's top-level menu).
# Source: ~/.local/lib/helium-exit/helium_exit.c — delete this file to undo.
LIB="$HOME/.local/lib/helium-exit/libhelium_exit.so"
[ -r "$LIB" ] && export LD_PRELOAD="$LIB${LD_PRELOAD:+:$LD_PRELOAD}"
exec /usr/bin/helium-browser "$@"
EOF
chmod +x "$HOME/.local/bin/helium-browser"
echo "installed; now using: $(command -v helium-browser)"
HELIUM_EXIT_DEBUG=1 "$HOME/.local/bin/helium-browser" --version 2>&1 | grep -E 'patched|skipped|Helium'

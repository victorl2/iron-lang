#!/bin/sh
set -eu

REPO="victorl2/iron-lang"
IRON_HOME="$HOME/.iron"
INSTALL_DIR="$IRON_HOME/bin"
ENV_FILE="$IRON_HOME/env"

main() {
    # Detect OS
    OS="$(uname -s)"
    case "$OS" in
        Linux)  PLATFORM="linux" ;;
        Darwin) PLATFORM="macos" ;;
        *)      echo "Error: Unsupported OS: $OS" >&2; exit 1 ;;
    esac

    # Detect architecture
    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64|amd64)  ARCH="x86_64" ;;
        arm64|aarch64) ARCH="arm64" ;;
        *)             echo "Error: Unsupported architecture: $ARCH" >&2; exit 1 ;;
    esac

    TARGET="${PLATFORM}-${ARCH}"

    # Get latest release version
    if command -v curl > /dev/null 2>&1; then
        VERSION="$(curl -sSf "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | sed 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/')"
    else
        echo "Error: curl is required" >&2
        exit 1
    fi

    if [ -z "$VERSION" ]; then
        echo "Error: Could not determine latest release version" >&2
        exit 1
    fi

    ARCHIVE="iron-${VERSION}-${TARGET}.tar.gz"
    URL="https://github.com/${REPO}/releases/download/v${VERSION}/${ARCHIVE}"

    echo "Installing iron ${VERSION} for ${TARGET}..."

    # Create install directories
    mkdir -p "$IRON_HOME/bin" "$IRON_HOME/lib"

    # Download and extract
    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "$TMPDIR"' EXIT

    echo "Downloading ${URL}..."
    curl -sSfL "$URL" -o "${TMPDIR}/${ARCHIVE}"
    LC_ALL=C tar -xzf "${TMPDIR}/${ARCHIVE}" -C "$IRON_HOME"
    chmod +x "${IRON_HOME}/bin/iron" "${IRON_HOME}/bin/ironc"

    # Create env file (like rustup's ~/.cargo/env)
    cat > "$ENV_FILE" << 'ENVEOF'
# Iron language environment
# source this file to add iron to your PATH
export PATH="$HOME/.iron/bin:$PATH"
ENVEOF

    # Source env from shell profiles
    SOURCE_LINE='. "$HOME/.iron/env"'

    add_to_profile() {
        PROFILE="$1"
        if [ -f "$PROFILE" ]; then
            # Remove old-style direct PATH export from previous installs
            if grep -q '.iron/bin' "$PROFILE" 2>/dev/null && ! grep -q '.iron/env' "$PROFILE" 2>/dev/null; then
                sed -i.bak '/.iron\/bin/d' "$PROFILE"
                # Also remove the "# Iron language" comment left by old installer
                sed -i.bak '/^# Iron language$/d' "$PROFILE"
                rm -f "${PROFILE}.bak"
            fi
            if ! grep -q '.iron/env' "$PROFILE" 2>/dev/null; then
                echo "" >> "$PROFILE"
                echo "# Iron language" >> "$PROFILE"
                echo "$SOURCE_LINE" >> "$PROFILE"
                echo "  Updated $PROFILE"
            fi
        fi
    }

    echo ""
    add_to_profile "$HOME/.bashrc"
    add_to_profile "$HOME/.zshrc"
    add_to_profile "$HOME/.bash_profile"

    # Also add to .profile for login shells if no other profile exists
    if [ ! -f "$HOME/.bashrc" ] && [ ! -f "$HOME/.zshrc" ] && [ ! -f "$HOME/.bash_profile" ]; then
        add_to_profile "$HOME/.profile"
    fi

    echo ""
    echo "Iron ${VERSION} installed to ${IRON_HOME}"
    echo "  Binaries: ${IRON_HOME}/bin/iron, ${IRON_HOME}/bin/ironc"
    echo ""
    echo "To get started you may need to restart your shell or run:"
    echo '  source "$HOME/.iron/env"'
    echo ""
    echo "Then verify with:"
    echo "  iron --version"
}

main

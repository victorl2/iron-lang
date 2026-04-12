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

    # ── emsdk (Emscripten) auto-install for web target support ────────────
    EMSDK_DIR="$IRON_HOME/emsdk"
    EMSDK_VERSION_FILE="$IRON_HOME/lib/.emsdk-version"

    install_emsdk() {
        if [ ! -f "$EMSDK_VERSION_FILE" ]; then
            return 0
        fi
        EMSDK_PIN="$(cat "$EMSDK_VERSION_FILE" | tr -d '[:space:]')"
        if [ -z "$EMSDK_PIN" ]; then
            return 0
        fi

        # Skip if already installed at the right version
        if [ -x "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
            INSTALLED_VER="$("$EMSDK_DIR/upstream/emscripten/emcc" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)"
            if [ "$INSTALLED_VER" = "$EMSDK_PIN" ]; then
                echo "  emsdk ${EMSDK_PIN} already installed, skipping"
                return 0
            fi
        fi

        echo "  Installing Emscripten SDK ${EMSDK_PIN} (for iron build --target=web)..."

        if [ ! -d "$EMSDK_DIR" ]; then
            if command -v git > /dev/null 2>&1; then
                git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR" > /dev/null 2>&1
            else
                echo "  Warning: git not found, skipping emsdk install (install git and re-run)"
                return 0
            fi
        fi

        (
            cd "$EMSDK_DIR"
            ./emsdk install "$EMSDK_PIN" > /dev/null 2>&1
            ./emsdk activate "$EMSDK_PIN" > /dev/null 2>&1
        )

        if [ -x "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
            echo "  emsdk ${EMSDK_PIN} installed to ${EMSDK_DIR}"
        else
            echo "  Warning: emsdk install may have failed — run 'iron build --target=web' for diagnostics"
        fi
    }

    install_emsdk

    # Update env file to include emsdk paths
    cat > "$ENV_FILE" << ENVEOF
# Iron language environment
# source this file to add iron to your PATH
export PATH="\$HOME/.iron/bin:\$PATH"

# Emscripten SDK (for iron build --target=web)
if [ -f "\$HOME/.iron/emsdk/emsdk_env.sh" ]; then
    . "\$HOME/.iron/emsdk/emsdk_env.sh" > /dev/null 2>&1
fi
ENVEOF

    echo ""
    echo "Iron ${VERSION} installed to ${IRON_HOME}"
    echo "  Binaries: ${IRON_HOME}/bin/iron, ${IRON_HOME}/bin/ironc"
    if [ -x "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
        echo "  Web SDK:  ${EMSDK_DIR} (emcc on PATH after sourcing env)"
    fi
    echo ""
    echo "To get started you may need to restart your shell or run:"
    echo '  source "$HOME/.iron/env"'
    echo ""
    echo "Then verify with:"
    echo "  iron --version"
    echo "  iron build --target=web --help    # web builds ready"
}

main

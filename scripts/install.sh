#!/bin/sh
set -eu

REPO="victorl2/iron-lang"
INSTALL_DIR="$HOME/.iron/bin"

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

    # Create install directory
    mkdir -p "$INSTALL_DIR"

    # Download and extract
    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "$TMPDIR"' EXIT

    echo "Downloading ${URL}..."
    curl -sSfL "$URL" -o "${TMPDIR}/${ARCHIVE}"
    tar -xzf "${TMPDIR}/${ARCHIVE}" -C "$INSTALL_DIR"
    chmod +x "${INSTALL_DIR}/iron"

    # Update PATH in shell profiles
    PATH_LINE='export PATH="$HOME/.iron/bin:$PATH"'

    add_to_profile() {
        PROFILE="$1"
        if [ -f "$PROFILE" ]; then
            if ! grep -q '.iron/bin' "$PROFILE" 2>/dev/null; then
                echo "" >> "$PROFILE"
                echo "# Iron language" >> "$PROFILE"
                echo "$PATH_LINE" >> "$PROFILE"
                echo "  Updated $PROFILE"
            fi
        fi
    }

    echo ""
    add_to_profile "$HOME/.bashrc"
    add_to_profile "$HOME/.zshrc"

    # Also add to .profile for login shells if neither bashrc/zshrc exists
    if [ ! -f "$HOME/.bashrc" ] && [ ! -f "$HOME/.zshrc" ]; then
        add_to_profile "$HOME/.profile"
    fi

    echo ""
    echo "Iron ${VERSION} installed to ${INSTALL_DIR}/iron"
    echo ""
    echo "Restart your shell or run:"
    echo "  export PATH=\"\$HOME/.iron/bin:\$PATH\""
    echo ""
    echo "Then verify with:"
    echo "  iron --version"
}

main

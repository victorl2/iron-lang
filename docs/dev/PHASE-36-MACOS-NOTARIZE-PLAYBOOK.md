# Phase 36 — macOS Sign + Notarize Playbook

> Phase 36 Plan 04 (REL-11). Human-gated playbook for code-signing and
> notarizing the `ironls` macOS Mach-O release binaries (ARM64 + x86_64)
> for distribution without Gatekeeper warnings. The agent prepared
> `scripts/sign-and-notarize-macos.sh` (idempotent, fails-fast on
> missing env vars). This file is what the maintainer runs from a Mac
> with Xcode Command Line Tools installed and Apple Developer ID
> credentials in the environment.

## 0. Prerequisites

- macOS host (notarization is Apple-only — cannot run on Linux/silvaserver).
- Xcode Command Line Tools installed:
  ```bash
  xcode-select --version    # e.g. xcode-select version 2410
  xcrun --version
  ```
- Apple Developer Program membership (active, paid).
- Four environment variables exported in the current shell (or sourced
  from `~/.config/iron-release/apple-secrets.sh` — see step 1).

## 1. One-time setup

The five Apple secrets live in `docs/dev/apple-notarization-setup.md`
(Steps 1–6). The four secrets this script needs are a subset of those:

| This script env var          | apple-notarization-setup.md name | How to obtain                                                |
| ---------------------------- | -------------------------------- | ------------------------------------------------------------ |
| `APPLE_DEV_ID_APP_IDENTITY`  | derived from Step 1–3 cert       | `security find-identity -v -p codesigning` after importing the Developer ID Application cert; copy the full line, e.g. `"Developer ID Application: Your Name (TEAMID1234)"`. |
| `APPLE_ID_USERNAME`          | `APPLE_ID_EMAIL`                 | Your Apple ID login email.                                   |
| `APPLE_ID_APP_PASSWORD`      | `APPLE_ID_APP_PASSWORD`          | Step 4 — app-specific password from `appleid.apple.com` → Security → App-Specific Passwords. |
| `APPLE_TEAM_ID`              | `APPLE_TEAM_ID`                  | Step 5 — 10-char Team ID from `developer.apple.com/account` → Membership. |

Recommended: store the four lines in `~/.config/iron-release/apple-secrets.sh`
with mode `0600` so they can be sourced into the current shell on
release day without leaking into shell history.

```bash
mkdir -p ~/.config/iron-release
cat > ~/.config/iron-release/apple-secrets.sh <<'EOF'
export APPLE_DEV_ID_APP_IDENTITY="Developer ID Application: <YOUR NAME> (<TEAM_ID>)"
export APPLE_ID_USERNAME="<your apple id email>"
export APPLE_ID_APP_PASSWORD="<app-specific password>"
export APPLE_TEAM_ID="<10-char Team ID>"
EOF
chmod 600 ~/.config/iron-release/apple-secrets.sh
```

## 2. Run

```bash
source ~/.config/iron-release/apple-secrets.sh

# Default invocation (signs the conventional release-artifact paths):
bash scripts/sign-and-notarize-macos.sh \
  dist/ironls-v4.0.0-alpha-macos-arm64/ironls \
  dist/ironls-v4.0.0-alpha-macos-x86_64/ironls
```

Or, equivalently, with no args (script defaults to
`dist/ironls-macos-arm64/ironls dist/ironls-macos-x86_64/ironls`):

```bash
bash scripts/sign-and-notarize-macos.sh
```

The script processes each binary through the 5-step pipeline:

1. `codesign --sign "$APPLE_DEV_ID_APP_IDENTITY" --options runtime --timestamp --force --verbose=2 <bin>`
2. `ditto -c -k --keepParent <bin> /tmp/notarize-<basename>-$$.zip`
3. `xcrun notarytool submit <zip> --apple-id <id> --password <app-pwd> --team-id <TEAM_ID> --wait --timeout 30m`
4. `xcrun stapler staple <bin>`
5. `codesign --verify --verbose=2 <bin> && spctl --assess --type execute --verbose <bin> && xcrun stapler validate <bin>`

## 3. Expected duration

- Per binary: **5–20 minutes** — almost all of which is `notarytool`
  polling Apple's notary service. The other steps run in <1 s each.
- Two binaries (arm64 + x86_64): **10–40 minutes** total when run
  sequentially. The script does not parallelize because notarytool
  submissions are serialized at Apple's end anyway.

## 4. Verify

The script's final step already runs all three verifications and exits
non-zero on any failure. To re-verify manually after the fact:

```bash
codesign --verify --verbose=2 ./ironls
# Expected: "ironls: valid on disk" and "ironls: satisfies its Designated Requirement"

spctl --assess --type execute --verbose ./ironls
# Expected: "ironls: accepted" with "source=Notarized Developer ID"

xcrun stapler validate ./ironls
# Expected: "The validate action worked!"
```

All three must print success. If any prints an error, treat as a hard
failure — do not upload that binary to the GitHub Release.

## 5. Failure modes

### 5.1 `errSecInternalComponent` from codesign

The signing keychain is locked or the identity is not unlocked for
unattended access. Fix:

```bash
security unlock-keychain ~/Library/Keychains/login.keychain-db
# Or, for the dedicated build keychain (see apple-notarization-setup.md):
security unlock-keychain ~/Library/Keychains/iron-release.keychain-db
```

Re-run the script. The `--force` flag on codesign makes resigning safe.

### 5.2 notarytool returns "Invalid"

Download the per-submission log JSON from the URL printed in the
notarytool output and inspect the issues:

```bash
xcrun notarytool log <SUBMISSION_UUID> \
  --apple-id "$APPLE_ID_USERNAME" \
  --password "$APPLE_ID_APP_PASSWORD" \
  --team-id "$APPLE_TEAM_ID" \
  -
```

Common root causes:

- **Missing `--options runtime`** — the binary is not built with hardened
  runtime; re-codesign (script already passes this flag, so this only
  bites if another tool re-signed the binary after the script ran).
- **Unsigned dependent framework or dylib** — `otool -L ironls` reveals
  the load paths; every non-system dylib must also be signed. `ironls`
  is statically linked today, so this should not occur.
- **Timestamp service unavailable** — Apple's TSA was down during the
  submit window. Re-run the script.

### 5.3 notarytool timeout (`--timeout 30m` exceeded)

Apple's notary service is backed up. Re-run the script; idempotency
makes resubmission safe but you'll re-pay the wait. Apple's status
page (`apple.com/support/systemstatus`) shows current notary backlog.

### 5.4 "The signature was invalidated by re-signing"

Another tool (or a `chmod`, `strip`, post-build tarball repack)
modified the binary after codesign ran. Re-run the script as the FINAL
step of the release pipeline.

### 5.5 `gatekeeper-assess` fails after staple

Indicates the staple ticket couldn't be embedded (rare for raw Mach-O
binaries; common for `.app` bundles with read-only resource directories).
Confirm the binary is writable and re-run.

## 6. Cross-references

- `scripts/build-release-artifacts.sh` (Plan 36-04 Task 3) invokes this
  script as part of the macOS arm64/x86_64 build path.
- `docs/dev/PHASE-36-TAG-PUSH-PLAYBOOK.md` (Plan 36-02) references the
  signed/notarized artifacts as inputs to `gh release upload`.
- `docs/dev/apple-notarization-setup.md` documents the one-time Apple
  Developer account, certificate, and app-specific-password setup.
- `docs/dev/release-runbook.md` §8 carries the canonical verification
  one-liner that this playbook embeds.

# Apple Developer-ID signing + notarization setup (HARD-21)

One-time maintainer setup runbook for the `sign-and-notarize-macos`
GitHub Actions job that signs every `ironls` macOS binary attached to
a GitHub Release. Closes the Phase 6 Gatekeeper caveat documented in
`editors/zed/README.md` — signed + notarized + stapled binaries pass
macOS Gatekeeper silently instead of prompting "developer cannot be
verified".

**Scope (Phase 7 D-09):** `ironls` macOS binaries ONLY. `iron` and
`ironc` macOS notarization is deferred to v1.x polish (most users
invoke them from a terminal, where Gatekeeper quarantine is easier
to unblock).

**Operating cost:** $99 USD/year for Apple Developer Program
membership. The project maintainer (Victor) owns the account. Rotate
on the membership anniversary.

---

## Prerequisites

1. Apple ID (free) with 2FA enabled.
2. Apple Developer Program membership ($99/year). Enroll at
   [https://developer.apple.com/programs/enroll/](https://developer.apple.com/programs/enroll/).
   Individual enrollment (not Organization) is sufficient for v1 —
   upgrading to Organization is possible later without re-signing
   existing binaries.
3. macOS machine with Xcode Command Line Tools installed:
   ```bash
   xcode-select --install
   ```
4. `pass` (password manager) configured with the project maintainer's
   GPG key — all generated credentials are stored here, never in the
   repository and never in CI outside GitHub Secrets.

---

## Step 1 — Create Developer-ID Application certificate

1. Open **Xcode → Settings → Accounts** and add the Apple ID.
2. Click **Manage Certificates** → `+` → **Developer ID Application**.
   (If the option is greyed out, the Apple Developer Program membership
   is not yet active — enrollment takes up to 48 hours.)
3. Xcode downloads the certificate into the **login** keychain.
4. Open **Keychain Access → login** and confirm a certificate named
   `Developer ID Application: <Your Name> (<10-char-TeamID>)` exists
   with a private key attached (disclosure-triangle shows a key).

Alternative (no Xcode): generate a CSR via Keychain Access
(**Keychain Access → Certificate Assistant → Request a Certificate
From a Certificate Authority**), upload it at
[https://developer.apple.com/account/resources/certificates](https://developer.apple.com/account/resources/certificates),
download the `.cer` and double-click to install.

---

## Step 2 — Export the P12 bundle

1. In **Keychain Access**, right-click the Developer-ID Application
   certificate → **Export** → format `Personal Information Exchange
   (.p12)` → save as `DeveloperID.p12`.
2. Set an export password. Use a 32-char random string from `pass`:
   ```bash
   pass generate apple-developer/iron-lang/p12-password 32 -n
   ```
   Copy the generated password into the Keychain Access prompt.
3. Store the generated password in `pass` (step 2 already did this;
   confirm with `pass show apple-developer/iron-lang/p12-password`).

**Pitfall — special characters in P12 password:** bash `$`, `` ` ``,
`!`, and `\` may need escaping when pasted into GitHub Secrets.
Using only alphanumerics (`pass generate -n`) avoids all shell
escape issues.

---

## Step 3 — Base64-encode the P12 for GitHub Secret storage

```bash
base64 -i DeveloperID.p12 | pbcopy
```

The clipboard now holds a single long line (on macOS, `base64` defaults
to 64-column wrap; `-i` implies no wrap on modern macOS). This value
goes into the `APPLE_DEVELOPER_ID_P12_BASE64` secret in Step 6.

Delete the exported `.p12` file once encoded:
```bash
rm DeveloperID.p12
```
The keychain retains the original; the disk copy is no longer needed.

---

## Step 4 — Generate an app-specific password for notarytool

`notarytool` rejects the Apple ID account password directly — it
requires an app-specific password.

1. Visit [https://appleid.apple.com](https://appleid.apple.com) and
   sign in.
2. Under **Sign-In and Security → App-Specific Passwords**, click
   **Generate an app-specific password**.
3. Label: `iron-lang-ci-notarytool`.
4. Copy the generated 19-char password (format `xxxx-xxxx-xxxx-xxxx`).
5. Store in `pass`:
   ```bash
   pass insert apple-developer/iron-lang/app-specific-password
   # paste the password; press Ctrl-D
   ```

Apple app-specific passwords do not expire automatically, but Apple
rotates them when the account password changes. Re-generate + rotate
the GitHub Secret on account password changes.

---

## Step 5 — Look up the Team ID

1. Visit
   [https://developer.apple.com/account](https://developer.apple.com/account).
2. Under **Membership details**, the **Team ID** is a 10-character
   string like `A1B2C3D4E5`.
3. Copy it (no password manager needed — it's not a secret, but
   exposing it leaks project-maintainer identity which we prefer to
   keep private).

---

## Step 6 — Configure GitHub repository secrets

Navigate to the repo on github.com → **Settings → Secrets and variables
→ Actions → New repository secret**. Add all five:

| Secret name                      | Source                                                   |
| -------------------------------- | -------------------------------------------------------- |
| `APPLE_DEVELOPER_ID_P12_BASE64`  | Step 3 (base64 of DeveloperID.p12)                       |
| `APPLE_DEVELOPER_ID_P12_PASSWORD`| Step 2 (p12 export password)                             |
| `APPLE_ID_EMAIL`                 | Apple ID login email                                     |
| `APPLE_ID_APP_PASSWORD`          | Step 4 (app-specific password)                           |
| `APPLE_TEAM_ID`                  | Step 5 (10-char team ID)                                 |

**T-07-06-01 mitigation:** GitHub Actions automatically masks these
values in job logs when referenced via `${{ secrets.NAME }}`. The
`sign-and-notarize-macos` job in `.github/workflows/release.yml`
binds them to env vars via an `env:` block — never interpolates them
into `run:` scripts directly.

---

## Step 7 — Verify with a dry-run

On any host (macOS or Linux), run the script in `DRY_RUN=1` mode
with placeholder values to confirm the control flow:

```bash
DRY_RUN=1 \
  APPLE_DEVELOPER_ID_P12_BASE64=x \
  APPLE_DEVELOPER_ID_P12_PASSWORD=x \
  APPLE_ID_EMAIL=x \
  APPLE_ID_APP_PASSWORD=x \
  APPLE_TEAM_ID=TESTTEAMID \
  bash scripts/ci/sign_and_notarize_macos.sh /tmp/fake-ironls 1.2.0-alpha.7 x86_64
```

Expected: the script prints `[DRY]` prefixes for every `security`,
`codesign`, `xcrun`, `ditto`, and `tar` invocation and exits 0.

Real first-release rehearsal: cut a prerelease tag (e.g.
`v0.0.0-notarize-rehearsal`) on a throwaway branch, let `release.yml`
run end-to-end on a real `ironls` binary, and download the signed
asset + verify:

```bash
spctl --assess --type execute ironls
# expect: "ironls: accepted" (Gatekeeper accepts the stapled binary)
stapler validate ironls
# expect: "The validate action worked!"
```

---

## Known pitfalls

1. **notarytool --wait hang modes.** Apple has confirmed 2026 hang
   modes where `notarytool submit --wait` hangs past Apple's own
   server-side timeout. Mitigation: the script caps the primary path
   at `--timeout 1200` (20 minutes) and falls back to asynchronous
   submit + 30-minute polling loop (60 × 30s). Overall job timeout
   is `timeout-minutes: 30` at the workflow level.
   Reference: Apple Developer Forums + `electron/notarize#179`.
2. **Default keychain contamination.** If the script's per-run
   keychain is NOT explicitly deleted, subsequent runs on the same
   GitHub-hosted runner (rare but possible with self-hosted runners)
   could reuse a stale keychain. Mitigation: the script uses
   `build-$$.keychain` (PID-suffixed name) and ends with
   `security delete-keychain`.
3. **P12 password bash-escaping.** Special characters like `$` or
   ``` ` ``` in the P12 password can be consumed by bash variable
   expansion. Mitigation: `pass generate -n` produces alphanumeric-only
   passwords (no symbols), and the script quotes all env var
   expansions (`"${APPLE_DEVELOPER_ID_P12_PASSWORD}"`).
4. **Hardened Runtime entitlements.** Iron's runtime does not use
   JIT, debugger-attach, camera, microphone, or any other restricted
   entitlement. No entitlements plist is needed for v1. If a future
   Iron feature needs one (e.g., JIT compilation), add an
   `entitlements.plist` file and pass `--entitlements entitlements.plist`
   to `codesign`.
5. **PAT rotation cadence.** Rotate the Apple Developer Program
   membership on its anniversary; rotate the app-specific password
   annually OR immediately on suspected compromise. The GitHub Secret
   values MUST be re-uploaded after rotation — GitHub does not refresh
   secrets automatically.
6. **Credential storage — project-owner `pass` ONLY.** Apple
   credentials live in `~/.password-store/apple-developer/iron-lang/`
   on the maintainer's machine. NEVER commit them to the repo (even
   in `.gitignore`d files), NEVER store in a shared team password
   manager, NEVER post fragments publicly.

---

## Cross-references

- `scripts/ci/sign_and_notarize_macos.sh` — the script this doc
  configures
- `.github/workflows/release.yml` — `sign-and-notarize-macos` job
  that invokes the script with secrets from `env:`
- `docs/dev/publisher-namespace-checklist.md` — consolidated
  maintainer pre-flight checklist (VSCode publisher + Zed publisher
  + Apple Developer + GitHub Releases)
- `.planning/phases/07-m6-production-hardening/07-CONTEXT.md` §D-09
  — locked design decision
- `.planning/phases/07-m6-production-hardening/07-RESEARCH.md`
  §"Apple codesign + notarytool in GitHub Actions" — Pitfall 5
  notarytool hang modes + 1200s timeout rationale

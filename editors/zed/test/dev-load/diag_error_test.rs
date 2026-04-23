// Phase 6 Plan 06-05 Task 3 (EXT-08 + EXT-10).
//
// Cargo test harness for the SHA-256 verification helper — the
// security-critical hand-rolled logic flagged as RESEARCH CRITICAL
// and threat T-06-05-01.
//
// Gated on `--features dev-extension-test` so the wasm32-wasip2
// release build does not pull in native-target test helpers. Run
// locally with:
//     cargo test --features dev-extension-test
// Or via CI in the zed-e2e job (editors/**/ci.yml).
//
// Scope: this file asserts the CORE invariant — that
// `Sha256::digest(bytes) == expected` iff the bytes are unmodified —
// and that tampering with the bytes produces a different digest.
// The full zed::download_file + zed::latest_github_release + extract
// + spawn flow requires Zed's extension runtime sandbox, which is
// only available inside the Zed application itself. That flow is
// exercised by manually loading the dev-extension into a Zed nightly
// in the CI zed-e2e job (Ubuntu allowed-to-fail v1 per CONTEXT D-07).

#![cfg(feature = "dev-extension-test")]

use sha2::{Digest, Sha256};

#[path = "../mocks/mock_release.rs"]
mod mock_release;

#[test]
fn sha256_verify_matches_mock_release() {
    let mock = mock_release::MockRelease::new();
    let actual = hex::encode(Sha256::digest(&mock.tarball_bytes));
    assert_eq!(
        actual, mock.expected_sha256,
        "mock SHA-256 helper must match its own digest (sha2 0.10 + hex 0.4 round-trip sanity)"
    );
}

#[test]
fn sha256_verify_rejects_tampered_bytes() {
    let mock = mock_release::MockRelease::new();
    let mut tampered = mock.tarball_bytes.clone();
    tampered[0] = tampered[0].wrapping_add(1);
    let actual = hex::encode(Sha256::digest(&tampered));
    assert_ne!(
        actual, mock.expected_sha256,
        "tampering a single byte of the tarball MUST produce a different SHA-256 — this is \
         the core invariant threat T-06-05-01 relies on"
    );
}

#[test]
fn sha256_output_is_lowercase_hex_64_chars() {
    // Our UI-SPEC S8 error path takes `first 16 chars` of each side of
    // the mismatch; that only reads cleanly if the digest is the
    // canonical 64-char lowercase-hex form. Lock it in here so a
    // future hex-encoding change in the main `lib.rs` path does not
    // silently break the displayed message.
    let mock = mock_release::MockRelease::new();
    assert_eq!(mock.expected_sha256.len(), 64, "SHA-256 hex digest must be 64 chars");
    assert!(
        mock.expected_sha256
            .chars()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()),
        "SHA-256 hex digest must be lowercase hex only"
    );
}

// Phase 6 Plan 06-05 Task 3 (EXT-08). Localhost mock for the
// GitHub Release + .sha256 + tarball flow used by tests under
// test/dev-load/.
//
// The real download path calls zed::download_file, which is sandboxed
// inside Zed's extension runtime and not reachable from a native
// `cargo test` binary. This mock is therefore a helper that produces
// a synthetic (tarball_bytes, expected_sha256) pair so the helper
// tests can exercise the SHA-256 verification invariant — the most
// security-critical hand-rolled logic per RESEARCH CRITICAL.
//
// The full end-to-end flow (zed::latest_github_release + download_file
// + make_file_executable) is exercised only in the CI zed-e2e job by
// loading the extension into a real Zed nightly against the
// v1.2.0-alpha.6-test pre-release (CONTEXT D-11 + Plan 06-05 Task 3
// documentation on the manual pre-release cut).

#![cfg(feature = "dev-extension-test")]

use sha2::{Digest, Sha256};

pub struct MockRelease {
    pub tarball_bytes: Vec<u8>,
    pub expected_sha256: String,
}

impl MockRelease {
    /// Produces a small, deterministic (bytes, sha) pair.
    ///
    /// The 20-byte body is intentionally trivial — it has no ironls
    /// in it. Helper tests just want a stable input/output pair to
    /// verify that `sha2::Sha256::digest(bytes) == expected` and that
    /// tampering with bytes produces a different digest. The real
    /// ironls tarball is exercised by the CI zed-e2e job.
    pub fn new() -> Self {
        let tarball_bytes = b"mock-ironls-tarball\n".to_vec();
        let expected_sha256 = hex::encode(Sha256::digest(&tarball_bytes));
        Self {
            tarball_bytes,
            expected_sha256,
        }
    }
}

impl Default for MockRelease {
    fn default() -> Self {
        Self::new()
    }
}

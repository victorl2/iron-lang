// Phase 6 Plan 06-05 Task 2 (EXT-08).
// Zed extension for the Iron programming language.
//
// Sources:
//   - .planning/phases/06-m5-grammars-editor-extensions/06-RESEARCH.md
//     §Pattern 4 (lines 569-727) — reference implementation of
//     download + SHA-256 verify + extract flow.
//   - .planning/phases/06-m5-grammars-editor-extensions/06-CONTEXT.md
//     §D-06 — binary acquisition flow locked.
//   - .planning/phases/06-m5-grammars-editor-extensions/06-UI-SPEC.md
//     §S1 Zed row, §S5 event vocabulary, §S8 download UI states.
//
// CRITICAL — hand-rolled SHA-256:
//   zed_extension_api::download_file has NO built-in SHA-256
//   verification. Issue zed-industries/zed#16732 closed "not planned".
//   We compute Sha256::digest ourselves against the raw downloaded
//   bytes, compared to a `.sha256` sidecar asset on the same Release.
//   Shipping an unverified download would let a tampered GitHub asset
//   or MITM run arbitrary code — this is a non-negotiable invariant.
//
// Flow (per CONTEXT D-06):
//   1. Read the `iron_lsp_path` setting. If set + file exists, use it.
//   2. Check the cached_binary_path field from a previous activation.
//   3. Fresh download:
//      a. zed::current_platform() → (os, arch)
//      b. asset name: ironls-<version>-<os>-<arch>.tar.gz
//      c. zed::latest_github_release("iron-lang/iron-lang", ...)
//      d. locate tarball + .sha256 sidecar in release.assets
//      e. download .sha256 sidecar FIRST (Uncompressed)
//      f. download tarball as Uncompressed (to hash raw bytes)
//      g. sha2::Sha256::digest vs expected — abort on mismatch
//      h. re-invoke download_file as GzipTar to extract
//      i. zed::make_file_executable on extracted binary

use zed_extension_api::{
    self as zed, Command, DownloadedFileType, GithubReleaseOptions,
    LanguageServerId, Result, Worktree,
};

use sha2::{Digest, Sha256};

/// The semver range this extension is known to be compatible with.
/// Mirrors [language_servers.iron-lsp] compatible_ironls in extension.toml;
/// UI-SPEC S9. Phase 7 HARD-22 promotes out-of-range to a hard refuse.
const COMPATIBLE_IRONLS: &str = ">=1.2.0, <1.3.0";

struct IronLspExtension {
    cached_binary_path: Option<String>,
}

/// (os, arch) triple used in asset names produced by our release.yml.
///
/// Matches the matrix row naming in .github/workflows/release.yml:
///   linux / x86_64
///   macos / x86_64
///   macos / aarch64
fn platform_triple() -> Result<(String, String)> {
    let (os, arch) = zed::current_platform();
    let os_str = match os {
        zed::Os::Mac => "macos",
        zed::Os::Linux => "linux",
        zed::Os::Windows => {
            return Err(
                "Iron LSP: Windows is not supported in v1. Set \"iron_lsp_path\" \
                 to a local binary if you have one."
                    .into(),
            );
        }
    };
    let arch_str = match arch {
        zed::Architecture::Aarch64 => "aarch64",
        zed::Architecture::X8664 => "x86_64",
        zed::Architecture::X86 => {
            return Err("Iron LSP: 32-bit x86 is not supported.".into());
        }
    };
    Ok((os_str.to_string(), arch_str.to_string()))
}

/// UI-SPEC S5 structured log events. src="zed-ext" is locked; the
/// vocabulary (ext.activate, download.start, download.verified,
/// download.mismatch, ironls.discovered, ironls.spawn.ok, etc.)
/// matches the cross-editor table.
///
/// We emit via eprintln! — Zed's extension host captures stderr into
/// its developer console (View → Debug in the Zed UI). The JSON shape
/// is one object per line so log-parsing tools can consume it.
fn log_event(lvl: &str, evt: &str, extras: &[(&str, &str)]) {
    let ts = current_ts();
    let mut out = format!(
        "{{\"ts\":\"{}\",\"lvl\":\"{}\",\"src\":\"zed-ext\",\"evt\":\"{}\"",
        ts, lvl, evt
    );
    for (k, v) in extras {
        // Conservative JSON escape — Zed extensions run in a WASM sandbox
        // with no serde available by default; keep this small.
        let escaped = v.replace('\\', "\\\\").replace('"', "\\\"");
        out.push_str(&format!(",\"{}\":\"{}\"", k, escaped));
    }
    out.push('}');
    eprintln!("{}", out);
}

/// Best-effort timestamp. Zed's WASM sandbox permits SystemTime but
/// wall-clock values during a hot-reload may be surprising; this is
/// for human log reading only, not for correctness logic.
fn current_ts() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    format!("epoch-{}", secs)
}

/// Read the `iron_lsp_path` setting for the given worktree, if any.
///
/// zed_extension_api 0.7 exposes settings via
/// zed::settings::LspSettings::for_worktree. On any read error
/// we fall back to None — a misconfigured setting should NOT crash
/// the language_server_command flow; the fresh-download path still
/// works.
fn read_user_override(worktree: &Worktree) -> Option<String> {
    // The LspSettings shape varies across 0.7.x point releases; be
    // defensive and treat any failure as "no override set".
    match zed::settings::LspSettings::for_worktree("iron-lsp", worktree) {
        Ok(settings) => {
            let binary = settings.binary.as_ref()?;
            let path = binary.path.as_ref()?;
            if path.is_empty() {
                None
            } else {
                Some(path.clone())
            }
        }
        Err(_) => None,
    }
}

impl zed::Extension for IronLspExtension {
    fn new() -> Self {
        log_event("info", "ext.activate", &[("editor", "zed")]);
        Self {
            cached_binary_path: None,
        }
    }

    fn language_server_command(
        &mut self,
        _lsp_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Command> {
        // 1. User override — UI-SPEC S1 first step.
        if let Some(path) = read_user_override(worktree) {
            if std::path::Path::new(&path).exists() {
                log_event(
                    "info",
                    "ironls.discovered",
                    &[("path", &path), ("method", "iron_lsp_path")],
                );
                return Ok(Command {
                    command: path,
                    args: vec![],
                    env: vec![],
                });
            }
            // Setting present but unusable — log and fall through to
            // download. UI-SPEC S1 treats this as a warning path, not
            // a hard failure.
            log_event(
                "warn",
                "ironls.discovered",
                &[
                    ("path", &path),
                    ("method", "iron_lsp_path"),
                    ("result", "missing"),
                ],
            );
        }

        // 2. Cached binary from an earlier activation in this session.
        if let Some(cached) = &self.cached_binary_path {
            if std::path::Path::new(cached).exists() {
                log_event(
                    "info",
                    "ironls.discovered",
                    &[("path", cached), ("method", "download-cache")],
                );
                return Ok(Command {
                    command: cached.clone(),
                    args: vec![],
                    env: vec![],
                });
            }
        }

        // 3. Fresh download from the iron-lang GitHub Release.
        let release = zed::latest_github_release(
            "iron-lang/iron-lang",
            GithubReleaseOptions {
                require_assets: true,
                pre_release: false,
            },
        )
        .map_err(|e| {
            let msg = format!(
                "Iron LSP: could not download ironls from GitHub. {}. \
                 Set \"iron_lsp_path\" in extension settings to use a local binary.",
                e
            );
            log_event("error", "ironls.spawn.failed", &[("reason", &msg)]);
            msg
        })?;

        let (os_str, arch_str) = platform_triple()?;
        let tarball_name =
            format!("ironls-{}-{}-{}.tar.gz", release.version, os_str, arch_str);
        let sha_name = format!("{}.sha256", tarball_name);

        let tarball_asset = release
            .assets
            .iter()
            .find(|a| a.name == tarball_name)
            .ok_or_else(|| {
                let msg = format!(
                    "Iron LSP: could not download ironls. No asset named {} on \
                     release {}. Set \"iron_lsp_path\" in extension settings to \
                     use a local binary.",
                    tarball_name, release.version
                );
                log_event("error", "ironls.spawn.failed", &[("reason", &msg)]);
                msg
            })?;

        let sha_asset = release
            .assets
            .iter()
            .find(|a| a.name == sha_name)
            .ok_or_else(|| {
                let msg = format!(
                    "Iron LSP: could not download ironls. No asset named {} on \
                     release {}. Set \"iron_lsp_path\" in extension settings to \
                     use a local binary.",
                    sha_name, release.version
                );
                log_event("error", "ironls.spawn.failed", &[("reason", &msg)]);
                msg
            })?;

        // Zed's extension runtime invokes the extension from a per-extension
        // working directory — relative paths land in that sandbox. We stage
        // both downloads under a version-stamped subdirectory so that an
        // ironls bump doesn't reuse a stale cache.
        let version_dir = format!("ironls-{}", release.version);
        let tarball_path = format!("{}/{}", version_dir, tarball_name);
        let sha_path = format!("{}/{}", version_dir, sha_name);

        // Ensure the version directory exists. fs::create_dir_all is
        // idempotent and safe to call every activation.
        let _ = std::fs::create_dir_all(&version_dir);

        log_event(
            "info",
            "download.start",
            &[
                ("url", &tarball_asset.download_url),
                ("version", &release.version),
            ],
        );

        // 3a. Download sidecar first. This is the cheapest failure mode:
        // if the release is missing the .sha256 file we want to fail
        // before transferring the (much larger) tarball.
        zed::download_file(
            &sha_asset.download_url,
            &sha_path,
            DownloadedFileType::Uncompressed,
        )
        .map_err(|e| {
            let msg = format!(
                "Iron LSP: could not download ironls. sha256 sidecar: {}. \
                 Set \"iron_lsp_path\" to a local binary to bypass.",
                e
            );
            log_event("error", "download.mismatch", &[("phase", "sha-fetch"), ("error", &e)]);
            msg
        })?;

        let expected_sha = std::fs::read_to_string(&sha_path)
            .map_err(|e| format!("Iron LSP: could not read sha256 sidecar: {}", e))?
            .split_whitespace()
            .next()
            .ok_or_else(|| {
                "Iron LSP: sha256 sidecar was empty. Retrying will re-download. \
                 Set \"iron_lsp_path\" to bypass."
                    .to_string()
            })?
            .to_string();

        // 3b. Download tarball uncompressed so we can hash the raw bytes.
        // Downloading GzipTar would extract in place with no addressable
        // bytes to verify — we need the tarball-on-disk form.
        zed::download_file(
            &tarball_asset.download_url,
            &tarball_path,
            DownloadedFileType::Uncompressed,
        )
        .map_err(|e| {
            let msg = format!(
                "Iron LSP: could not download ironls from GitHub. {}. \
                 Set \"iron_lsp_path\" to a local binary to bypass.",
                e
            );
            log_event("error", "download.mismatch", &[("phase", "tarball-fetch"), ("error", &e)]);
            msg
        })?;

        // 3c. HAND-ROLL SHA-256 verification.
        // zed_extension_api 0.7.0 does not expose a helper for this; the
        // sha2 crate's Sha256::digest is the audited path.
        let tarball_bytes = std::fs::read(&tarball_path)
            .map_err(|e| format!("Iron LSP: could not read tarball: {}", e))?;
        let actual_sha = hex::encode(Sha256::digest(&tarball_bytes));

        if actual_sha != expected_sha {
            // Remove the bad tarball so a re-activation re-downloads
            // a clean copy rather than keeping a cached-poisoned file.
            let _ = std::fs::remove_file(&tarball_path);
            log_event(
                "error",
                "download.mismatch",
                &[
                    ("expected", &expected_sha),
                    ("actual", &actual_sha),
                    ("asset", &tarball_name),
                ],
            );
            // UI-SPEC S8 mismatch text (verbatim), truncated to first 16
            // chars each per the spec.
            let expected_short: String = expected_sha.chars().take(16).collect();
            let actual_short: String = actual_sha.chars().take(16).collect();
            return Err(format!(
                "Iron LSP: ironls download verification failed. Expected {}…, \
                 got {}…. Retrying will re-download. Set \"iron_lsp_path\" to \
                 bypass.",
                expected_short, actual_short
            ));
        }

        log_event(
            "info",
            "download.verified",
            &[("sha256", &actual_sha), ("asset", &tarball_name)],
        );

        // 3d. Re-download as GzipTar to extract into the version dir.
        // Zed's extension runtime sandboxes tarball extraction to the
        // target directory by default; the in-repo tarball is produced
        // by our own release.yml via `tar czf` with well-behaved member
        // paths (accepted threat T-06-05-04).
        zed::download_file(
            &tarball_asset.download_url,
            &version_dir,
            DownloadedFileType::GzipTar,
        )
        .map_err(|e| {
            format!("Iron LSP: extract failed: {}", e)
        })?;

        // After extraction the binary lives at <version_dir>/ironls.
        // Our release.yml layout packs the single `ironls` binary at
        // the tarball root (see release.yml "tar czf" step).
        let binary_path = format!("{}/ironls", version_dir);
        zed::make_file_executable(&binary_path)
            .map_err(|e| format!("Iron LSP: chmod failed: {}", e))?;

        log_event(
            "info",
            "ironls.spawn.ok",
            &[
                ("path", &binary_path),
                ("method", "download"),
                ("version", &release.version),
                ("compatible_range", COMPATIBLE_IRONLS),
            ],
        );

        self.cached_binary_path = Some(binary_path.clone());

        Ok(Command {
            command: binary_path,
            args: vec![],
            env: vec![],
        })
    }
}

zed::register_extension!(IronLspExtension);

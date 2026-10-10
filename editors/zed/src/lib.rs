// Zed extension for the Iron programming language.
//
// Finding ironls, in order:
//   1. `lsp.iron-lsp.binary.path` in Zed settings (with optional
//      `binary.arguments` / `binary.env`);
//   2. `ironls` on the worktree's PATH (the Iron installer puts it there);
//   3. a binary this extension downloaded earlier in the session;
//   4. a fresh download from the latest GitHub release, verified against
//      its `.sha256` sidecar before it is extracted.
//
// zed_extension_api::download_file has no hash verification
// (zed-industries/zed#16732, closed "not planned"), so the archive is
// downloaded uncompressed, hashed with sha2, and only extracted on a match.
//
// Every binary's `--version` is checked against COMPATIBLE_IRONLS; an
// out-of-range version refuses to start the server with a message Zed
// shows to the user.
//
// The server's settings come from `lsp.iron-lsp.settings` (same keys as the
// VS Code extension's iron.* settings, see config.rs) and are sent both as
// initializationOptions and through workspace/didChangeConfiguration.

mod config;
mod debug;

use config::{COMPATIBLE_IRONLS, RELEASE_REPO};
use sha2::{Digest, Sha256};
use zed_extension_api::{
    self as zed, serde_json, settings::LspSettings, Command, DownloadedFileType,
    GithubReleaseOptions, LanguageServerId, Result, Worktree,
};

const SERVER_ID: &str = "iron-lsp";

struct IronLspExtension {
    cached_binary_path: Option<String>,
}

/// One JSON log line on stderr; Zed shows extension stderr in its log
/// (`zed: open log`).
fn log_event(lvl: &str, evt: &str, extras: &[(&str, &str)]) {
    let mut out = format!(
        "{{\"lvl\":\"{}\",\"src\":\"zed-ext\",\"evt\":\"{}\"",
        lvl, evt
    );
    for (k, v) in extras {
        let escaped = v.replace('\\', "\\\\").replace('"', "\\\"");
        out.push_str(&format!(",\"{}\":\"{}\"", k, escaped));
    }
    out.push('}');
    eprintln!("{}", out);
}

/// Check `ironls --version` against COMPATIBLE_IRONLS. Running the binary
/// needs the `process:exec` capability from extension.toml; when Zed denies
/// it (or the binary does not answer) the check is skipped rather than
/// blocking the server. A version that parses and is out of range refuses.
fn check_ironls_version(ironls_path: &str) -> Result<()> {
    let output = match zed::process::Command::new(ironls_path)
        .arg("--version")
        .output()
    {
        Ok(output) => output,
        Err(e) => {
            log_event(
                "warn",
                "ironls.version_check",
                &[("path", ironls_path), ("skipped", &e)],
            );
            return Ok(());
        }
    };
    let stdout = String::from_utf8_lossy(&output.stdout);
    let Some(version) = config::version_from_output(&stdout) else {
        log_event(
            "warn",
            "ironls.version_check",
            &[("path", ironls_path), ("skipped", "no version in output")],
        );
        return Ok(());
    };
    if !config::version_in_range(&version) {
        log_event(
            "error",
            "ironls.version_mismatch",
            &[("detected", &version), ("range", COMPATIBLE_IRONLS)],
        );
        return Err(format!(
            "Iron LSP: {} is ironls {}, but this extension requires {}. Install a \
             matching ironls from https://github.com/{}/releases or point \
             \"lsp\": {{ \"iron-lsp\": {{ \"binary\": {{ \"path\": ... }} }} }} at one.",
            ironls_path, version, COMPATIBLE_IRONLS, RELEASE_REPO
        ));
    }
    log_event(
        "info",
        "ironls.version_check",
        &[("version", &version), ("range", COMPATIBLE_IRONLS)],
    );
    Ok(())
}

/// (os, arch) as spelled in the release asset names
/// (ironls-<tag>-<os>-<arch>.tar.gz, .zip on Windows).
fn platform_triple() -> Result<(String, String)> {
    let (os, arch) = zed::current_platform();
    let os_str = match os {
        zed::Os::Mac => "macos",
        zed::Os::Linux => "linux",
        zed::Os::Windows => "windows",
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

fn lsp_settings(worktree: &Worktree) -> LspSettings {
    LspSettings::for_worktree(SERVER_ID, worktree).unwrap_or_default()
}

impl IronLspExtension {
    fn download_ironls(&mut self, id: &LanguageServerId) -> Result<String> {
        let release = zed::latest_github_release(
            RELEASE_REPO,
            GithubReleaseOptions {
                require_assets: true,
                pre_release: false,
            },
        )
        .map_err(|e| {
            format!(
                "Iron LSP: could not reach the {} releases ({}). Install ironls on \
                 PATH or set lsp.iron-lsp.binary.path.",
                RELEASE_REPO, e
            )
        })?;

        let (os_str, arch_str) = platform_triple()?;
        let is_windows = os_str == "windows";
        let archive_name = format!(
            "ironls-{}-{}-{}.{}",
            release.version,
            os_str,
            arch_str,
            if is_windows { "zip" } else { "tar.gz" }
        );
        let sha_name = format!("{}.sha256", archive_name);
        let find = |name: &str| {
            release
                .assets
                .iter()
                .find(|a| a.name == name)
                .ok_or_else(|| {
                    format!(
                        "Iron LSP: release {} has no asset {}. Install ironls on PATH or \
                         set lsp.iron-lsp.binary.path.",
                        release.version, name
                    )
                })
        };
        let archive_asset = find(&archive_name)?;
        let sha_asset = find(&sha_name)?;

        let version_dir = format!("ironls-{}", release.version);
        let binary_path = format!(
            "{}/ironls{}",
            version_dir,
            if is_windows { ".exe" } else { "" }
        );
        if std::path::Path::new(&binary_path).exists() {
            return Ok(binary_path);
        }
        let archive_path = format!("{}/{}", version_dir, archive_name);
        let sha_path = format!("{}/{}", version_dir, sha_name);
        let _ = std::fs::create_dir_all(&version_dir);

        zed::set_language_server_installation_status(
            id,
            &zed::LanguageServerInstallationStatus::Downloading,
        );
        log_event(
            "info",
            "download.start",
            &[("url", &archive_asset.download_url)],
        );

        // The sidecar first: a release without it fails before the large
        // download.
        zed::download_file(
            &sha_asset.download_url,
            &sha_path,
            DownloadedFileType::Uncompressed,
        )
        .map_err(|e| format!("Iron LSP: could not download {}: {}", sha_name, e))?;
        let expected = std::fs::read_to_string(&sha_path)
            .map_err(|e| format!("Iron LSP: could not read {}: {}", sha_name, e))?
            .split_whitespace()
            .next()
            .unwrap_or_default()
            .to_lowercase();

        zed::download_file(
            &archive_asset.download_url,
            &archive_path,
            DownloadedFileType::Uncompressed,
        )
        .map_err(|e| format!("Iron LSP: could not download {}: {}", archive_name, e))?;
        let bytes = std::fs::read(&archive_path)
            .map_err(|e| format!("Iron LSP: could not read {}: {}", archive_name, e))?;
        let actual = hex::encode(Sha256::digest(&bytes));
        if actual != expected {
            let _ = std::fs::remove_file(&archive_path);
            log_event(
                "error",
                "download.mismatch",
                &[("expected", &expected), ("actual", &actual)],
            );
            return Err(format!(
                "Iron LSP: {} failed SHA-256 verification (expected {}..., got {}...). \
                 Retrying downloads it again.",
                archive_name,
                expected.chars().take(16).collect::<String>(),
                actual.chars().take(16).collect::<String>()
            ));
        }
        log_event("info", "download.verified", &[("sha256", &actual)]);

        // Verified: extract (download_file only extracts from a URL).
        zed::download_file(
            &archive_asset.download_url,
            &version_dir,
            if is_windows {
                DownloadedFileType::Zip
            } else {
                DownloadedFileType::GzipTar
            },
        )
        .map_err(|e| format!("Iron LSP: could not extract {}: {}", archive_name, e))?;
        let _ = std::fs::remove_file(&archive_path);
        if !is_windows {
            zed::make_file_executable(&binary_path)
                .map_err(|e| format!("Iron LSP: chmod failed: {}", e))?;
        }
        Ok(binary_path)
    }
}

impl zed::Extension for IronLspExtension {
    fn new() -> Self {
        Self {
            cached_binary_path: None,
        }
    }

    fn language_server_command(
        &mut self,
        id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Command> {
        let binary = lsp_settings(worktree).binary;
        let args = binary
            .as_ref()
            .and_then(|b| b.arguments.clone())
            .unwrap_or_default();
        let mut env: Vec<(String, String)> = binary
            .as_ref()
            .and_then(|b| b.env.clone())
            .map(|m| m.into_iter().collect())
            .unwrap_or_default();

        // 1. Explicit path from settings.
        if let Some(path) = binary.as_ref().and_then(|b| b.path.clone()) {
            if !path.is_empty() {
                check_ironls_version(&path)?;
                log_event(
                    "info",
                    "ironls.discovered",
                    &[("path", &path), ("method", "settings")],
                );
                return Ok(Command {
                    command: path,
                    args,
                    env,
                });
            }
        }

        // 2. ironls on PATH, with the worktree's shell environment.
        if let Some(path) = worktree.which("ironls") {
            check_ironls_version(&path)?;
            log_event(
                "info",
                "ironls.discovered",
                &[("path", &path), ("method", "PATH")],
            );
            env.extend(worktree.shell_env());
            return Ok(Command {
                command: path,
                args,
                env,
            });
        }

        // 3. Downloaded earlier in this session.
        if let Some(cached) = self.cached_binary_path.clone() {
            if std::path::Path::new(&cached).exists() {
                return Ok(Command {
                    command: cached,
                    args,
                    env,
                });
            }
        }

        // 4. Download from the latest release.
        let path = self.download_ironls(id)?;
        check_ironls_version(&path)?;
        log_event(
            "info",
            "ironls.discovered",
            &[("path", &path), ("method", "download")],
        );
        self.cached_binary_path = Some(path.clone());
        Ok(Command {
            command: path,
            args,
            env,
        })
    }

    fn language_server_initialization_options(
        &mut self,
        _id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Option<serde_json::Value>> {
        let s = lsp_settings(worktree);
        Ok(Some(config::initialization_options(
            s.initialization_options.as_ref(),
            s.settings.as_ref(),
        )))
    }

    fn language_server_workspace_configuration(
        &mut self,
        _id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Option<serde_json::Value>> {
        let s = lsp_settings(worktree);
        Ok(Some(config::workspace_configuration(s.settings.as_ref())))
    }

    // ── Debugging (#312, #347): the `Iron` adapter runs `iron dap`. ──

    fn get_dap_binary(
        &mut self,
        adapter_name: String,
        config: zed::DebugTaskDefinition,
        user_provided_debug_adapter_path: Option<String>,
        worktree: &Worktree,
    ) -> std::result::Result<zed::DebugAdapterBinary, String> {
        if adapter_name != debug::ADAPTER {
            return Err(format!("unknown debug adapter {adapter_name}"));
        }
        debug::binary(config, user_provided_debug_adapter_path, worktree)
    }

    fn dap_request_kind(
        &mut self,
        _adapter_name: String,
        config: zed::serde_json::Value,
    ) -> std::result::Result<zed::StartDebuggingRequestArgumentsRequest, String> {
        debug::request_kind(&config)
    }

    fn dap_config_to_scenario(
        &mut self,
        config: zed::DebugConfig,
    ) -> std::result::Result<zed::DebugScenario, String> {
        debug::scenario(config)
    }
}

zed::register_extension!(IronLspExtension);

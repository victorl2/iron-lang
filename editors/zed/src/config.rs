// Pure helpers of the Zed extension: version range and the settings ironls
// receives. No zed_extension_api types here, so the native tests under
// test/dev-load/ include this file directly (#[path]).

use serde_json::{json, Map, Value};

/// The semver range this extension works with. Mirrors
/// [version_constraints] ironls in extension.toml (a test checks they agree).
pub const COMPATIBLE_IRONLS: &str = ">= 4.0.0, < 5.0.0";
pub const COMPATIBLE_MIN: (u32, u32, u32) = (4, 0, 0);
pub const COMPATIBLE_MAX_EXCLUSIVE: (u32, u32, u32) = (5, 0, 0);

/// The GitHub repository whose releases carry the ironls archives.
pub const RELEASE_REPO: &str = "victorl2/iron-lang";

/// Parse "X.Y.Z" with an optional "-pre" / "+build" suffix.
pub fn parse_semver(s: &str) -> Option<(u32, u32, u32)> {
    let s = s.strip_prefix('v').unwrap_or(s);
    let core = s.split(['-', '+']).next()?;
    let mut it = core.split('.');
    let major: u32 = it.next()?.parse().ok()?;
    let minor: u32 = it.next()?.parse().ok()?;
    let patch: u32 = it.next()?.parse().ok()?;
    Some((major, minor, patch))
}

/// True when `version` lies in [COMPATIBLE_MIN, COMPATIBLE_MAX_EXCLUSIVE).
/// Pre-release suffixes within the range are accepted.
pub fn version_in_range(version: &str) -> bool {
    match parse_semver(version) {
        Some(v) => v >= COMPATIBLE_MIN && v < COMPATIBLE_MAX_EXCLUSIVE,
        None => false,
    }
}

/// The version token of `ironls --version` output
/// ("ironls 4.10.0-alpha (a14e8c58, 2026-10-10)" -> "4.10.0-alpha").
pub fn version_from_output(stdout: &str) -> Option<String> {
    stdout
        .split_whitespace()
        .find(|s| s.chars().next().is_some_and(|c| c.is_ascii_digit()))
        .map(|s| s.to_string())
}

/// workspace/didChangeConfiguration payload. Users write the same keys as
/// VS Code's iron.* settings, either nested under "iron" or bare:
///
///   "lsp": { "iron-lsp": { "settings": { "inlayHints": { "parameterNames": false } } } }
///   "lsp": { "iron-lsp": { "settings": { "iron": { "inlayHints": { ... } } } } }
///
/// ironls reads settings.iron.inlayHints, so bare keys are wrapped.
pub fn workspace_configuration(user: Option<&Value>) -> Value {
    let iron = match user {
        Some(Value::Object(obj)) => match obj.get("iron") {
            Some(Value::Object(inner)) => Value::Object(inner.clone()),
            _ => Value::Object(obj.clone()),
        },
        _ => Value::Object(Map::new()),
    };
    json!({ "iron": iron })
}

/// initializationOptions: the user's own initialization_options plus the
/// inlay hint settings, so the first response already honors them.
pub fn initialization_options(user_init: Option<&Value>, user_settings: Option<&Value>) -> Value {
    let mut out = match user_init {
        Some(Value::Object(obj)) => obj.clone(),
        _ => Map::new(),
    };
    out.entry("clientName").or_insert_with(|| json!("zed"));
    let cfg = workspace_configuration(user_settings);
    if let Some(hints) = cfg.get("iron").and_then(|i| i.get("inlayHints")) {
        if hints.is_object() {
            out.insert("inlayHints".to_string(), hints.clone());
        }
    }
    Value::Object(out)
}

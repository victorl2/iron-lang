// Checks what Zed reads from this directory when it loads the extension,
// without Zed itself: extension.toml, languages/iron/config.toml, the
// grammar pin and the query files, plus the settings ironls receives.
//
//     cargo test --features dev-extension-test
//
// That every query compiles against the grammar is checked by
// tests/grammars/tree_sitter/check_queries.sh (ctest test_tree_sitter_queries),
// which has the tree-sitter CLI.

#![cfg(feature = "dev-extension-test")]

use serde_json::json;
use std::path::{Path, PathBuf};
use std::process::Command;

#[path = "../../src/config.rs"]
#[allow(dead_code)]
mod config;

fn ext_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn repo_dir() -> PathBuf {
    ext_dir().join("../..").canonicalize().unwrap()
}

fn read_toml(path: &Path) -> toml::Table {
    let text =
        std::fs::read_to_string(path).unwrap_or_else(|e| panic!("{}: {}", path.display(), e));
    text.parse::<toml::Table>()
        .unwrap_or_else(|e| panic!("{} is not valid TOML: {}", path.display(), e))
}

#[test]
fn manifest_declares_server_grammar_and_capabilities() {
    let m = read_toml(&ext_dir().join("extension.toml"));
    assert_eq!(m["id"].as_str(), Some("iron-lsp"));
    assert_eq!(m["schema_version"].as_integer(), Some(1));

    let server = m["language_servers"]["iron-lsp"]
        .as_table()
        .expect("language server");
    let langs: Vec<_> = server["languages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap())
        .collect();
    assert_eq!(langs, ["Iron"]);

    assert_eq!(
        m["version_constraints"]["ironls"].as_str(),
        Some(config::COMPATIBLE_IRONLS),
        "extension.toml [version_constraints] and src/config.rs disagree"
    );

    let caps = m["capabilities"].as_array().expect("capabilities");
    let kinds: Vec<_> = caps.iter().map(|c| c["kind"].as_str().unwrap()).collect();
    assert!(kinds.contains(&"download_file"));
    // Zed 1.23 rejects the whole manifest when a download_file capability
    // has no `path` (#356).
    for c in caps
        .iter()
        .filter(|c| c["kind"].as_str() == Some("download_file"))
    {
        assert!(
            c.get("host").is_some(),
            "download_file capability without host"
        );
        assert!(
            c.get("path")
                .and_then(|p| p.as_array())
                .is_some_and(|p| !p.is_empty()),
            "download_file capability without path"
        );
    }
    assert!(
        kinds.contains(&"process:exec"),
        "the --version check needs a process:exec capability"
    );
}

#[test]
fn language_config_matches_the_server_and_grammar() {
    let m = read_toml(&ext_dir().join("extension.toml"));
    let c = read_toml(&ext_dir().join("languages/iron/config.toml"));
    assert_eq!(c["name"].as_str(), Some("Iron"));
    let grammar = c["grammar"].as_str().unwrap();
    assert!(
        m["grammars"].get(grammar).is_some(),
        "config.toml names grammar {grammar:?} but extension.toml has no [grammars.{grammar}]"
    );
    // Top-level keys must sit before the [[brackets]] tables, or TOML files
    // them under the last bracket entry and Zed never sees them.
    for key in ["tab_size", "hard_tabs", "word_characters", "line_comments"] {
        assert!(
            c.contains_key(key),
            "config.toml: {key} is not a top-level key"
        );
    }
    let comments: Vec<_> = c["line_comments"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap())
        .collect();
    assert_eq!(
        comments.first(),
        Some(&"-- "),
        "Iron line comments start with --"
    );
    assert_eq!(
        c["path_suffixes"].as_array().unwrap()[0].as_str(),
        Some("iron")
    );
    // Zed asks the debug locators for a run button's Debug entry only with
    // the language's first debugger (editor::code_actions::debug_scenarios):
    // without it the gutter menu has no Debug at all.
    let debuggers = c.get("debuggers").and_then(|d| d.as_array());
    let first = debuggers.and_then(|d| d.first()).and_then(|d| d.as_str());
    assert_eq!(first, Some("Iron"), "config.toml: debuggers must list the Iron adapter first");
    assert!(
        m["debug_adapters"].get("Iron").is_some(),
        "config.toml names the Iron debugger but extension.toml has no [debug_adapters.Iron]"
    );
}

#[test]
fn grammar_pin_points_at_this_repository_and_holds_the_parser() {
    let m = read_toml(&ext_dir().join("extension.toml"));
    let g = m["grammars"]["iron"].as_table().unwrap();
    let repo = g["repository"].as_str().unwrap();
    assert!(
        repo.ends_with("/iron-lang"),
        "unexpected grammar repository {repo}"
    );
    let path = g["path"].as_str().unwrap();
    assert_eq!(path, "grammars/tree-sitter/iron");
    assert!(repo_dir().join(path).join("src/parser.c").is_file());

    // When git history is available, the pinned commit must contain the
    // generated parser (Zed compiles it; it never runs tree-sitter generate).
    let commit = g["commit"].as_str().unwrap();
    assert_eq!(commit.len(), 40, "pin a full commit SHA");
    let have_git = Command::new("git")
        .args(["cat-file", "-e", &format!("{commit}^{{commit}}")])
        .current_dir(repo_dir())
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if have_git {
        let spec = format!("{commit}:{path}/src/parser.c");
        let ok = Command::new("git")
            .args(["cat-file", "-e", &spec])
            .current_dir(repo_dir())
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        assert!(
            ok,
            "pinned grammar commit {commit} has no {path}/src/parser.c"
        );
    } else {
        eprintln!("grammar pin {commit} not in local history; skipping the parser.c check");
    }
}

#[test]
fn every_query_zed_loads_is_present_and_highlights_are_in_sync() {
    let dir = ext_dir().join("languages/iron");
    for q in [
        "highlights",
        "brackets",
        "indents",
        "outline",
        "textobjects",
        "overrides",
    ] {
        let p = dir.join(format!("{q}.scm"));
        let text =
            std::fs::read_to_string(&p).unwrap_or_else(|_| panic!("missing {}", p.display()));
        assert!(text.contains('@'), "{} has no captures", p.display());
    }
    // highlights.scm is generated from the canonical queries.
    let status = Command::new("bash")
        .arg(repo_dir().join("scripts/sync-editor-queries.sh"))
        .arg("--check")
        .status()
        .expect("run sync-editor-queries.sh");
    assert!(
        status.success(),
        "editor queries are stale: run scripts/sync-editor-queries.sh"
    );
}

#[test]
fn version_range_accepts_5x_and_refuses_others() {
    assert!(config::version_in_range("5.0.0"));
    assert!(config::version_in_range("5.0.0-alpha"));
    assert!(config::version_in_range("v5.3.0-alpha"));
    assert!(!config::version_in_range("4.12.0-alpha"));
    assert!(!config::version_in_range("6.0.0"));
    assert!(!config::version_in_range("garbage"));
    assert_eq!(
        config::version_from_output("ironls 4.10.0-alpha (a14e8c58, 2026-10-10)\n").as_deref(),
        Some("4.10.0-alpha")
    );
}

#[test]
fn settings_reach_ironls_under_iron() {
    // Bare keys are wrapped under "iron"; nested ones pass through.
    let bare = json!({ "inlayHints": { "parameterNames": false } });
    let nested = json!({ "iron": { "inlayHints": { "parameterNames": false } } });
    let want = json!({ "iron": { "inlayHints": { "parameterNames": false } } });
    assert_eq!(config::workspace_configuration(Some(&bare)), want);
    assert_eq!(config::workspace_configuration(Some(&nested)), want);
    assert_eq!(config::workspace_configuration(None), json!({ "iron": {} }));

    let init = config::initialization_options(Some(&json!({ "extra": 1 })), Some(&bare));
    assert_eq!(init["inlayHints"], json!({ "parameterNames": false }));
    assert_eq!(init["extra"], json!(1));
    assert_eq!(init["clientName"], json!("zed"));
}

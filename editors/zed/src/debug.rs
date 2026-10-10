//! The `Iron` debug adapter for Zed's debugger (#312, #347).
//!
//! Zed runs `iron dap`, the Iron debug adapter shared with VS Code and
//! Neovim: it builds the program (a .iron file or a package directory)
//! with `iron build --debug`, runs lldb-dap or gdb with the value
//! formatters, shows locals under their Iron names and stops on panics.
//! The extension only says how to start it and what a launch looks like.

use zed_extension_api::{
    serde_json, DebugAdapterBinary, DebugConfig, DebugRequest, DebugScenario,
    DebugTaskDefinition, StartDebuggingRequestArguments, StartDebuggingRequestArgumentsRequest,
    TaskTemplate, Worktree,
};

/// The adapter name: `"adapter": "Iron"` in .zed/debug.json.
pub const ADAPTER: &str = "Iron";

/// `iron` for `iron dap`: the adapter path the user set in Zed's
/// settings (`dap.Iron.binary`), else `iron` on the worktree's PATH.
fn iron_cli(user_path: Option<String>, worktree: &Worktree) -> Result<String, String> {
    if let Some(p) = user_path.filter(|p| !p.is_empty()) {
        return Ok(p);
    }
    worktree
        .which("iron")
        .ok_or_else(|| "Iron debugging needs the `iron` CLI on PATH (or set dap.Iron.binary)".to_string())
}

pub fn request_kind(config: &serde_json::Value) -> Result<StartDebuggingRequestArgumentsRequest, String> {
    match config.get("request").and_then(|r| r.as_str()).unwrap_or("launch") {
        "launch" => Ok(StartDebuggingRequestArgumentsRequest::Launch),
        other => Err(format!("the Iron debug adapter does not support `{other}` requests, only `launch`")),
    }
}

pub fn binary(
    config: DebugTaskDefinition,
    user_path: Option<String>,
    worktree: &Worktree,
) -> Result<DebugAdapterBinary, String> {
    let parsed: serde_json::Value =
        serde_json::from_str(&config.config).map_err(|e| format!("invalid Iron debug configuration: {e}"))?;
    let request = request_kind(&parsed)?;
    Ok(DebugAdapterBinary {
        command: Some(iron_cli(user_path, worktree)?),
        arguments: vec!["dap".to_string()],
        envs: vec![],
        cwd: Some(worktree.root_path()),
        connection: None,
        request_args: StartDebuggingRequestArguments { configuration: config.config, request },
    })
}

/// A scenario from Zed's "new session" form: program, arguments, cwd, env.
pub fn scenario(config: DebugConfig) -> Result<DebugScenario, String> {
    let launch = match config.request {
        DebugRequest::Launch(l) => l,
        DebugRequest::Attach(_) => return Err("the Iron debug adapter cannot attach to a process".into()),
    };
    let env: serde_json::Map<String, serde_json::Value> =
        launch.envs.into_iter().map(|(k, v)| (k, serde_json::Value::String(v))).collect();
    let mut obj = serde_json::json!({
        "request": "launch",
        "program": launch.program,
        "args": launch.args,
        "env": env,
        "stopOnEntry": config.stop_on_entry.unwrap_or(false),
    });
    if let Some(cwd) = launch.cwd {
        obj["cwd"] = serde_json::Value::String(cwd);
    }
    Ok(DebugScenario {
        label: config.label,
        adapter: config.adapter,
        build: None,
        config: obj.to_string(),
        tcp_connection: None,
    })
}

/// The locator's name: `[debug_locators.iron]` in extension.toml.
pub const LOCATOR: &str = "iron";

/// A run button's task as a debug session: `iron run <file>` debugs the
/// file, `iron test <file> <name>` debugs that test alone (`iron dap`
/// builds it with --test). Other tasks are not ours.
pub fn locate(task: &TaskTemplate, label: &str) -> Option<DebugScenario> {
    let program_name = task.command.rsplit(['/', '\\']).next().unwrap_or("");
    if program_name != "iron" && program_name != "iron.exe" {
        return None;
    }
    let unquote = |s: &str| s.trim().trim_matches('"').replace("\\\"", "\"");
    let args: Vec<String> = task.args.iter().map(|a| unquote(a)).collect();
    let mut config = match args.as_slice() {
        [cmd, file] if cmd == "run" && file.ends_with(".iron") => {
            serde_json::json!({ "request": "launch", "program": file })
        }
        [cmd, file, test] if cmd == "test" && file.ends_with(".iron") && !test.is_empty() => {
            serde_json::json!({ "request": "launch", "program": file, "test": test })
        }
        _ => return None,
    };
    if let Some(cwd) = &task.cwd {
        config["cwd"] = serde_json::Value::String(cwd.clone());
    }
    Some(DebugScenario {
        label: format!("Debug: {}", label),
        adapter: ADAPTER.to_string(),
        build: None,
        config: config.to_string(),
        tcp_connection: None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn task(command: &str, args: &[&str]) -> TaskTemplate {
        TaskTemplate {
            label: "t".into(),
            command: command.into(),
            args: args.iter().map(|a| a.to_string()).collect(),
            env: vec![],
            cwd: None,
        }
    }

    fn config(s: &DebugScenario) -> serde_json::Value {
        serde_json::from_str(&s.config).unwrap()
    }

    #[test]
    fn run_task_debugs_the_file() {
        let s = locate(&task("iron", &["run", "/p/main.iron"]), "iron run main.iron").unwrap();
        assert_eq!(s.adapter, "Iron");
        assert!(s.build.is_none());
        assert_eq!(config(&s)["program"], "/p/main.iron");
        assert!(config(&s).get("test").is_none());
    }

    #[test]
    fn test_task_debugs_that_test() {
        let s = locate(&task("/usr/local/bin/iron", &["test", "/p/a.iron", "\"sums the first ten\""]),
                       "iron test").unwrap();
        assert_eq!(config(&s)["program"], "/p/a.iron");
        assert_eq!(config(&s)["test"], "sums the first ten");
    }

    #[test]
    fn other_tasks_are_not_ours() {
        assert!(locate(&task("cargo", &["run"]), "cargo run").is_none());
        assert!(locate(&task("iron", &["test", "/p/a.iron"]), "all tests").is_none());
        assert!(locate(&task("iron", &["build", "/p/a.iron"]), "build").is_none());
    }
}

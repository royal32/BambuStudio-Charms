use omnitrix::{App, Command, OptionSpec, Result, RunOptions, StatusProbe, Value};
use std::path::PathBuf;
use std::time::Duration;

fn app() -> Result<App> {
    let root = std::env::var_os("CHARM_LIBRARY_REPO")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .parent()
                .unwrap()
                .parent()
                .unwrap()
                .to_owned()
        });
    let home = std::env::var_os("CHARM_LIBRARY_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join(".charm-library"));
    let python = std::env::var("CHARM_LIBRARY_PYTHON").unwrap_or_else(|_| {
        let runtime = home.join("venv/bin/python3");
        if runtime.exists() {
            runtime.to_string_lossy().into_owned()
        } else {
            "python3".into()
        }
    });
    let backend = root
        .join("tools/library_backend.py")
        .to_string_lossy()
        .into_owned();
    let manifest = home.join("manifest.json");
    let mut app = App::new("Charm & Pair Library")
        .description(
            "Prepare A1 mini projects, slice plates and maintain searchable vector previews",
        )
        .display_command(["./charm-library"])
        .root(&root)
        .status(
            StatusProbe::new("Library", move || {
                let Ok(raw) = std::fs::read_to_string(&manifest) else {
                    return Ok("Not initialized · Maintenance → Setup".into());
                };
                let data: serde_json::Value = serde_json::from_str(&raw)
                    .map_err(|e| omnitrix::CommandError::new(e.to_string()))?;
                let projects = data["projects"].as_object();
                let count = projects.map_or(0, |p| p.len());
                let plates: std::collections::BTreeSet<&str> = projects
                    .into_iter()
                    .flat_map(|p| p.values())
                    .flat_map(|p| p["plates"].as_array().into_iter().flatten())
                    .filter_map(|p| p["filename"].as_str())
                    .collect();
                Ok(format!("{count} projects · {} plates", plates.len()).into())
            })
            .timeout(Duration::from_millis(500)),
        );
    for (group, description) in [
        ("library", "Plan, update and inspect the collection"),
        ("sources", "Register editable projects or folders"),
        ("settings", "Printer profiles and output location"),
        (
            "maintenance",
            "Runtime setup, integrity checks and migration",
        ),
    ] {
        app.group(group, description)?;
    }
    let text = |name, description| OptionSpec::new(name, description).default("").allow_empty();
    let batch = || {
        vec![
            text("project", "Filter by project name or ID; blank selects all"),
            OptionSpec::new("limit", "Maximum projects; 0 processes all")
                .integer()
                .default("0"),
            OptionSpec::boolean(
                "force",
                "Rebuild unchanged projects too (previews only in Previews)",
            ),
        ]
    };
    let commands = vec![
        (
            "library status",
            "Show tracked projects and folders",
            "status",
            vec![],
        ),
        (
            "library projects",
            "Find project IDs, plate indices and saved editable projects",
            "projects",
            vec![text(
                "project",
                "Filter by project name or ID; blank lists all",
            )],
        ),
        (
            "library plan",
            "Dry run: show new, changed, missing and conflicting projects",
            "plan",
            batch(),
        ),
        (
            "library update",
            "Process new or changed projects; retain backups of replaced files",
            "update",
            batch(),
        ),
        (
            "library previews",
            "Regenerate artwork without reslicing unchanged projects",
            "previews",
            batch(),
        ),
        (
            "library exceptions",
            "Explain plates using their original printer thumbnail",
            "exceptions",
            vec![],
        ),
        (
            "library rename",
            "Override a plate name; apply with the next update",
            "rename",
            vec![
                OptionSpec::new("project", "Exact project name or ID").required(),
                OptionSpec::new("plate", "Plate index")
                    .integer()
                    .positive()
                    .required(),
                OptionSpec::new("name", "New plate name, including SB when appropriate").required(),
            ],
        ),
        (
            "sources list",
            "List watched folders and availability",
            "sources",
            vec![],
        ),
        (
            "sources add",
            "Watch a project or folder; input files are never changed",
            "add-source",
            vec![
                OptionSpec::new("path", "Editable .3mf file or source folder").required(),
                OptionSpec::new("category", "How to name the models")
                    .choices(["Bogg Charm", "Pair Eyewear"])
                    .default("Bogg Charm"),
            ],
        ),
        (
            "sources replace",
            "Use a newer source for an existing project; apply with Update",
            "use-source",
            vec![
                OptionSpec::new("project", "Exact existing project name or ID").required(),
                OptionSpec::new("path", "New editable .3mf source file").required(),
            ],
        ),
        (
            "settings show",
            "Show all saved configuration",
            "settings",
            vec![],
        ),
        (
            "settings configure",
            "Change settings; blank fields keep their existing value",
            "configure",
            vec![
                text("library", "Sliced output directory"),
                text("slicer", "BambuStudio executable"),
                text(
                    "profile-root",
                    "BambuStudio profile directory containing user and system",
                ),
                text("machine", "Printer preset name"),
                text("filament", "Filament preset name"),
                text("process", "Print preset name"),
            ],
        ),
        (
            "maintenance doctor",
            "Check slicer, profiles and vector dependencies",
            "doctor",
            vec![],
        ),
        (
            "maintenance verify",
            "Hash-check every managed output",
            "verify",
            vec![],
        ),
        (
            "maintenance setup",
            "Create an isolated Python runtime and install vector dependencies",
            "setup",
            vec![],
        ),
        (
            "maintenance adopt",
            "Import the previous batch records without altering print files",
            "adopt",
            vec![
                OptionSpec::new("folder", "Directory containing the original batch reports")
                    .required(),
            ],
        ),
    ];
    for (path, description, operation, options) in commands {
        let python = python.clone();
        let backend = backend.clone();
        let mut command = Command::new(path, description, move |ctx, args| {
            let mut argv = vec![python.clone(), backend.clone(), operation.to_owned()];
            for (name, value) in &args.values {
                match value {
                    Value::Bool(true) => argv.push(format!("--{name}")),
                    Value::Bool(false) => (),
                    value if !value.to_string().is_empty() => {
                        argv.push(format!("--{name}"));
                        argv.push(value.to_string());
                    }
                    _ => (),
                }
            }
            Ok(ctx
                .run(
                    &argv,
                    RunOptions {
                        check: false,
                        echo: false,
                        ..RunOptions::default()
                    },
                )?
                .code)
        });
        for option in options {
            command = command.option(option);
        }
        app.add_command(command)?;
    }
    Ok(app)
}

fn main() -> std::process::ExitCode {
    match app() {
        Ok(app) => std::process::ExitCode::from(app.run().clamp(0, 255) as u8),
        Err(error) => {
            eprintln!("{error}");
            std::process::ExitCode::FAILURE
        }
    }
}

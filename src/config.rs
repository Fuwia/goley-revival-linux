use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    pub prefix_path: PathBuf,
    pub runner_id: String,
    pub use_gamemode: bool,
    pub use_mangohud: bool,
    pub use_gamescope: bool,
    pub gamescope_args: String,
    pub extra_env: String,
    pub show_debug_window: bool,
    pub hide_launcher_on_play: bool,
    pub color_scheme: String,
}

impl Default for AppConfig {
    fn default() -> Self {
        let prefix_path = detect_initial_prefix();
        let use_gamemode = which_binary("gamemoderun").is_some();

        Self {
            prefix_path,
            runner_id: "auto".to_string(),
            use_gamemode,
            use_mangohud: false,
            use_gamescope: false,
            gamescope_args: "-W 1920 -H 1080 -f".to_string(),
            extra_env: String::new(),
            show_debug_window: false,
            hide_launcher_on_play: false,
            color_scheme: "dark".to_string(),
        }
    }
}

impl AppConfig {
    pub fn config_dir() -> PathBuf {
        dirs::config_dir()
            .unwrap_or_else(|| PathBuf::from("."))
            .join("goley-revival-linux")
    }

    pub fn cache_dir() -> PathBuf {
        dirs::cache_dir()
            .unwrap_or_else(|| PathBuf::from("/tmp"))
            .join("goley-revival-linux")
    }

    #[allow(dead_code)]
    pub fn data_dir() -> PathBuf {
        dirs::data_local_dir()
            .unwrap_or_else(|| PathBuf::from("."))
            .join("goley-revival-linux")
    }

    pub fn config_file() -> PathBuf {
        Self::config_dir().join("config.json")
    }

    pub fn load() -> Self {
        let path = Self::config_file();
        if let Ok(raw) = fs::read_to_string(&path) {
            if let Ok(cfg) = serde_json::from_str::<Self>(&raw) {
                return cfg;
            }
        }
        let cfg = Self::default();
        let _ = cfg.save();
        cfg
    }

    pub fn save(&self) -> Result<(), String> {
        let dir = Self::config_dir();
        fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let json = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        fs::write(Self::config_file(), json).map_err(|e| e.to_string())
    }
}

pub fn which_binary(name: &str) -> Option<PathBuf> {
    let path_var = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path_var) {
        let candidate = dir.join(name);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

fn detect_initial_prefix() -> PathBuf {
    if let Some(home) = dirs::home_dir() {
        // Check common existing prefixes where Goley Revival may already be installed
        let candidates = [
            home.join("Faugus/default"),
            home.join("Faugus/revival"),
            home.join("Faugus/goley-revival"),
            home.join("Games/goley-revival"),
            home.join(".local/share/goley-revival-linux/pfx"),
            home.join(".wine"),
        ];

        for candidate in &candidates {
            if prefix_contains_revival(candidate) {
                return candidate.clone();
            }
        }

        return home.join(".local/share/goley-revival-linux/pfx");
    }

    PathBuf::from("/tmp/goley-revival-linux-pfx")
}

fn prefix_contains_revival(prefix: &PathBuf) -> bool {
    let users_dir = prefix.join("drive_c/users");
    if let Ok(entries) = fs::read_dir(users_dir) {
        for entry in entries.flatten() {
            let exe = entry
                .path()
                .join("AppData/Local/Programs/Revival/revival_launcher.exe");
            if exe.is_file() {
                return true;
            }
        }
    }
    false
}

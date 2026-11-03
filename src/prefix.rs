use crate::config::which_binary;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

pub const NCRYPT64_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/ncrypt64.dll"));
pub const NCRYPT32_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/ncrypt32.dll"));
pub const DINPUT8_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/dinput8.dll"));

#[derive(Debug, Clone)]
pub struct RunnerOption {
    pub id: String,
    pub label: String,
    pub kind: RunnerKind,
}

#[derive(Debug, Clone)]
pub enum RunnerKind {
    Umu {
        umu_bin: PathBuf,
        proton_dir: Option<PathBuf>,
    },
    ProtonDirect {
        proton_script: PathBuf,
    },
    Wine {
        wine_bin: PathBuf,
    },
}

#[derive(Debug, Clone, Default)]
#[allow(dead_code)]
pub struct SessionInfo {
    pub username: Option<String>,
    pub has_session_token: bool,
    pub remember: bool,
}

pub fn discover_runners() -> Vec<RunnerOption> {
    let mut out = Vec::new();
    let umu_bin = find_umu_bin();
    let proton_dirs = find_proton_dirs();

    if let Some(ref umu) = umu_bin {
        if proton_dirs.is_empty() {
            out.push(RunnerOption {
                id: "umu:default".to_string(),
                label: "UMU-Launcher (Varsayılan GE-Proton)".to_string(),
                kind: RunnerKind::Umu {
                    umu_bin: umu.clone(),
                    proton_dir: None,
                },
            });
        } else {
            for pdir in &proton_dirs {
                let name = pdir
                    .file_name()
                    .and_then(|s| s.to_str())
                    .unwrap_or("Proton");
                out.push(RunnerOption {
                    id: format!("umu:{}", pdir.display()),
                    label: format!("{} (UMU-Run)", name),
                    kind: RunnerKind::Umu {
                        umu_bin: umu.clone(),
                        proton_dir: Some(pdir.clone()),
                    },
                });
            }
        }
    } else {
        for pdir in &proton_dirs {
            let script = pdir.join("proton");
            if script.is_file() {
                let name = pdir
                    .file_name()
                    .and_then(|s| s.to_str())
                    .unwrap_or("Proton");
                out.push(RunnerOption {
                    id: format!("proton:{}", pdir.display()),
                    label: format!("{} (Doğrudan)", name),
                    kind: RunnerKind::ProtonDirect {
                        proton_script: script,
                    },
                });
            }
        }
    }

    if let Some(wine_bin) = which_binary("wine") {
        out.push(RunnerOption {
            id: format!("wine:{}", wine_bin.display()),
            label: format!("Sistem Wine ({})", wine_bin.display()),
            kind: RunnerKind::Wine { wine_bin },
        });
    }

    out
}

pub fn select_runner(preferred_id: &str) -> Option<RunnerOption> {
    let runners = discover_runners();
    if preferred_id != "auto" {
        if let Some(found) = runners.iter().find(|r| r.id == preferred_id) {
            return Some(found.clone());
        }
    }
    // Prefer Proton-GE Latest / GE-Proton / Proton-CachyOS via UMU if available
    if let Some(best) = runners
        .iter()
        .find(|r| r.label.contains("Proton-GE") || r.label.contains("GE-Proton"))
    {
        return Some(best.clone());
    }
    runners.into_iter().next()
}

fn find_umu_bin() -> Option<PathBuf> {
    if let Some(p) = which_binary("umu-run") {
        return Some(p);
    }
    if let Some(home) = dirs::home_dir() {
        let candidates = [
            home.join(".local/share/faugus-launcher/umu-run"),
            home.join(".local/bin/umu-run"),
        ];
        for c in candidates {
            if c.is_file() {
                return Some(c);
            }
        }
    }
    None
}

fn find_proton_dirs() -> Vec<PathBuf> {
    let mut dirs_found = Vec::new();
    let mut roots = vec![
        PathBuf::from("/usr/share/steam/compatibilitytools.d"),
        PathBuf::from("/usr/local/share/steam/compatibilitytools.d"),
    ];
    if let Some(home) = dirs::home_dir() {
        roots.insert(0, home.join(".steam/root/compatibilitytools.d"));
        roots.insert(0, home.join(".local/share/Steam/compatibilitytools.d"));
        roots.push(home.join(".var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d"));
    }

    for root in roots {
        if let Ok(entries) = fs::read_dir(root) {
            for entry in entries.flatten() {
                let p = entry.path();
                if p.is_dir() && p.join("proton").is_file() {
                    if !dirs_found
                        .iter()
                        .any(|existing: &PathBuf| existing.file_name() == p.file_name())
                    {
                        dirs_found.push(p);
                    }
                }
            }
        }
    }

    dirs_found.sort_by(|a, b| {
        let sa = a.file_name().and_then(|s| s.to_str()).unwrap_or("");
        let sb = b.file_name().and_then(|s| s.to_str()).unwrap_or("");
        sb.cmp(sa)
    });
    dirs_found
}

pub fn resolve_wine_user_dir(prefix: &Path) -> PathBuf {
    let users = prefix.join("drive_c/users");
    let steamuser = users.join("steamuser");
    let sys_user = std::env::var("USER").unwrap_or_else(|_| "steamuser".to_string());
    let local_user = users.join(&sys_user);

    // Check which user dir already has Revival installed
    for candidate in [&steamuser, &local_user] {
        if candidate
            .join("AppData/Local/Programs/Revival/revival_launcher.exe")
            .is_file()
        {
            return candidate.clone();
        }
    }

    if let Ok(entries) = fs::read_dir(&users) {
        for entry in entries.flatten() {
            let p = entry.path();
            if p.join("AppData/Local/Programs/Revival/revival_launcher.exe")
                .is_file()
            {
                return p;
            }
        }
    }

    if steamuser.is_dir() {
        return steamuser;
    }
    if local_user.is_dir() {
        return local_user;
    }
    steamuser
}

pub fn revival_root_dir(prefix: &Path) -> PathBuf {
    resolve_wine_user_dir(prefix).join("AppData/Local/Programs/Revival")
}

pub fn revival_game_dir(prefix: &Path) -> PathBuf {
    revival_root_dir(prefix).join("Game")
}

pub fn revival_local_appdata_dir(prefix: &Path) -> PathBuf {
    resolve_wine_user_dir(prefix).join("AppData/Local/Revival")
}

pub fn revival_launcher_log_path(prefix: &Path) -> PathBuf {
    resolve_wine_user_dir(prefix).join("AppData/Roaming/Revival/logs/launcher.log")
}

pub fn atomic_write_file(target: &Path, bytes: &[u8]) -> Result<(), String> {
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("Klasör oluşturulamadı ({:?}): {}", parent, e))?;
    }
    let tmp = target.with_extension(format!("goley_tmp_{}", std::process::id()));
    {
        let mut f = fs::File::create(&tmp)
            .map_err(|e| format!("Geçici dosya yazılamadı ({:?}): {}", tmp, e))?;
        f.write_all(bytes)
            .map_err(|e| format!("Yazma hatası ({:?}): {}", tmp, e))?;
        f.sync_all().ok();
    }
    fs::rename(&tmp, target).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("Dosya taşınamadı ({:?}): {}", target, e)
    })
}

pub fn install_ncrypt_shims(prefix: &Path) -> Result<(), String> {
    let rev_root = revival_root_dir(prefix);
    let bin_tr = revival_game_dir(prefix).join("BinaryTr");
    let sys32 = prefix.join("drive_c/windows/system32");
    let syswow64 = prefix.join("drive_c/windows/syswow64");

    // Always place next to revival_launcher.exe (x64) and BinaryTrU.exe (x86)
    atomic_write_file(&rev_root.join("ncrypt.dll"), NCRYPT64_BYTES)?;
    atomic_write_file(&bin_tr.join("ncrypt.dll"), NCRYPT32_BYTES)?;
    // DirectInput8 keyboard / arrow key shim next to BinaryTrU.exe
    atomic_write_file(&bin_tr.join("dinput8.dll"), DINPUT8_BYTES)?;

    // Also install into system32 (64-bit) & syswow64 (32-bit) if prefix windows dirs exist
    if sys32.is_dir() {
        let _ = atomic_write_file(&sys32.join("ncrypt.dll"), NCRYPT64_BYTES);
    }
    if syswow64.is_dir() {
        let _ = atomic_write_file(&syswow64.join("ncrypt.dll"), NCRYPT32_BYTES);
    }

    // Ensure DllOverrides in user.reg if Wine isn't actively locking it
    if find_running_pids().1.is_none() {
        let _ = ensure_dll_override_in_user_reg(prefix);
    }

    Ok(())
}

pub fn check_shims_installed(prefix: &Path) -> bool {
    let x64 = revival_root_dir(prefix).join("ncrypt.dll");
    let x86 = revival_game_dir(prefix).join("BinaryTr/ncrypt.dll");
    let di8 = revival_game_dir(prefix).join("BinaryTr/dinput8.dll");
    x64.is_file() && x86.is_file() && di8.is_file()
}

pub fn ensure_dll_override_in_user_reg(prefix: &Path) -> Result<(), String> {
    let reg_path = prefix.join("user.reg");
    if !reg_path.is_file() {
        return Ok(());
    }
    let content = fs::read_to_string(&reg_path).map_err(|e| e.to_string())?;

    let mut dirty = false;
    let mut overrides = Vec::new();
    if !content.contains("\"ncrypt\"=\"native,builtin\"") {
        overrides.push("\"ncrypt\"=\"native,builtin\"");
        dirty = true;
    }
    if !content.contains("\"dinput8\"=\"native,builtin\"") {
        overrides.push("\"dinput8\"=\"native,builtin\"");
        dirty = true;
    }

    if !dirty {
        return Ok(());
    }

    let insert_text = format!("{}\n", overrides.join("\n"));
    let mut updated = content.clone();
    if let Some(pos) = updated.find("[Software\\\\Wine\\\\DllOverrides]") {
        if let Some(line_end) = updated[pos..].find('\n') {
            let insert_at = pos + line_end + 1;
            updated.insert_str(insert_at, &insert_text);
        }
    } else {
        updated.push_str(&format!("\n[Software\\\\Wine\\\\DllOverrides] 1793659200\n{}", insert_text));
    }

    atomic_write_file(&reg_path, updated.as_bytes())
}

pub fn read_session_info(prefix: &Path) -> SessionInfo {
    let reg_path = prefix.join("user.reg");
    let Ok(content) = fs::read_to_string(&reg_path) else {
        return SessionInfo::default();
    };

    let has_session_token =
        content.contains("[Software\\\\Wine\\\\Credential Manager\\\\Generic: Revival.LauncherSession.v1]");

    let mut username = None;
    let mut remember = false;

    if let Some(pos) = content.find("[Software\\\\Revival\\\\Revival Launcher\\\\login]") {
        let tail = &content[pos..];
        let section_end = tail[1..]
            .find("\n[")
            .map(|idx| idx + 1)
            .unwrap_or(tail.len());
        let section = &tail[..section_end];

        for line in section.lines() {
            let trimmed = line.trim();
            if let Some(val) = trimmed.strip_prefix("\"username\"=\"") {
                if let Some(end_q) = val.rfind('"') {
                    let u = &val[..end_q];
                    if !u.is_empty() {
                        username = Some(u.to_string());
                    }
                }
            } else if trimmed == "\"remember\"=\"true\"" {
                remember = true;
            }
        }
    }

    SessionInfo {
        username,
        has_session_token,
        remember,
    }
}

pub fn write_login_prefs_to_user_reg(prefix: &Path, username: &str, remember: bool) -> Result<(), String> {
    let reg_path = prefix.join("user.reg");
    if !reg_path.is_file() {
        return Ok(());
    }
    let content = fs::read_to_string(&reg_path).map_err(|e| e.to_string())?;
    let rem_str = if remember { "true" } else { "false" };
    let escaped_user = username.replace('\\', "\\\\").replace('"', "\\\"");
    let new_section = format!(
        "[Software\\\\Revival\\\\Revival Launcher\\\\login] 1793659323\n\"remember\"=\"{}\"\n\"username\"=\"{}\"\n",
        rem_str, escaped_user
    );

    let updated = if let Some(pos) = content.find("[Software\\\\Revival\\\\Revival Launcher\\\\login]") {
        let tail = &content[pos..];
        let section_end = tail[1..]
            .find("\n[")
            .map(|idx| pos + idx + 1)
            .unwrap_or(content.len());
        format!("{}{}{}", &content[..pos], new_section, &content[section_end..])
    } else {
        format!("{}\n{}", content, new_section)
    };

    atomic_write_file(&reg_path, updated.as_bytes())
}

pub fn clear_session_in_user_reg(prefix: &Path) -> Result<(), String> {
    let reg_path = prefix.join("user.reg");
    if !reg_path.is_file() {
        return Ok(());
    }
    let content = fs::read_to_string(&reg_path).map_err(|e| e.to_string())?;
    let header = "[Software\\\\Wine\\\\Credential Manager\\\\Generic: Revival.LauncherSession.v1]";

    if let Some(pos) = content.find(header) {
        let tail = &content[pos..];
        let section_end = tail[1..]
            .find("\n[")
            .map(|idx| pos + idx + 1)
            .unwrap_or(content.len());
        let updated = format!("{}{}", &content[..pos], &content[section_end..]);
        atomic_write_file(&reg_path, updated.as_bytes())?;
    }
    Ok(())
}

/// Returns `(Option<game_pid>, Option<launcher_pid>)` by scanning `/proc` directly.
pub fn find_running_pids() -> (Option<u32>, Option<u32>) {
    let mut game_pid = None;
    let mut launcher_pid = None;

    let Ok(entries) = fs::read_dir("/proc") else {
        return (None, None);
    };

    for entry in entries.flatten() {
        let fname = entry.file_name();
        let Some(pid_str) = fname.to_str() else { continue };
        let Ok(pid) = pid_str.parse::<u32>() else { continue };

        let cmdline_path = entry.path().join("cmdline");
        let Ok(raw) = fs::read(&cmdline_path) else { continue };
        if raw.is_empty() {
            continue;
        }
        let text = String::from_utf8_lossy(&raw).to_lowercase();
        // Skip wrapper python/umu/steam.exe processes so we get the real Wine processes
        if text.contains("python") || text.contains("umu-run") || text.contains("srt-bwrap") || text.contains("pv-adverb") || text.contains("steam.exe") {
            continue;
        }
        if text.contains("binarytru.exe") {
            game_pid = Some(pid);
        } else if text.contains("revival_launcher.exe") {
            launcher_pid = Some(pid);
        }
    }

    (game_pid, launcher_pid)
}

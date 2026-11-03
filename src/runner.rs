use crate::config::{which_binary, AppConfig};
use crate::prefix::{
    atomic_write_file, find_running_pids, install_ncrypt_shims, revival_launcher_log_path,
    revival_local_appdata_dir, revival_root_dir, select_runner, write_login_prefs_to_user_reg,
    RunnerKind,
};
use async_channel::Sender;
use std::fs;
use std::io::{BufRead, BufReader, Seek, SeekFrom};
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct LoginCredentials {
    pub username: String,
    pub password: String,
    pub remember: bool,
}

#[derive(Debug, Clone)]
pub enum RunnerEvent {
    LogLine(String),
    Status {
        headline: String,
        detail: String,
        pulsing: bool,
    },
    NeedLogin {
        reason: Option<String>,
    },
    LoginSucceeded {
        username: String,
    },
    LaunchError(String),
    GameRunning {
        pid: u32,
    },
    GameExited,
}

pub fn send_bridge_command(prefix: &Path, cmd: &str) -> Result<(), String> {
    let dir = revival_local_appdata_dir(prefix);
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let cmd_file = dir.join("goley_bridge_cmd.txt");
    atomic_write_file(&cmd_file, cmd.as_bytes())
}

pub fn stop_running_game(prefix: &Path) {
    let _ = send_bridge_command(prefix, "QUIT\n");
    let (game_pid, launcher_pid) = find_running_pids();
    if let Some(pid) = game_pid {
        let _ = Command::new("kill").args(["-15", &pid.to_string()]).status();
    }
    if let Some(pid) = launcher_pid {
        let _ = Command::new("kill").args(["-15", &pid.to_string()]).status();
    }
}

pub fn launch_headless_session(
    cfg: AppConfig,
    creds: Option<LoginCredentials>,
    tx: Sender<RunnerEvent>,
) {
    std::thread::spawn(move || {
        if let Err(err) = run_session_inner(&cfg, creds, &tx) {
            let _ = tx.send_blocking(RunnerEvent::LaunchError(err));
        }
    });
}

fn run_session_inner(
    cfg: &AppConfig,
    creds: Option<LoginCredentials>,
    tx: &Sender<RunnerEvent>,
) -> Result<(), String> {
    let prefix = &cfg.prefix_path;
    let rev_root = revival_root_dir(prefix);
    let launcher_exe = rev_root.join("revival_launcher.exe");

    if !launcher_exe.is_file() {
        return Err(
            "revival_launcher.exe bulunamadı. Lütfen önce 'Oyunu İndir / Güncelle' butonuna tıklayın."
                .to_string(),
        );
    }

    // 1. Atomically install x64 + x86 ncrypt.dll shims
    install_ncrypt_shims(prefix)?;

    let (existing_game_pid, existing_launcher_pid) = find_running_pids();
    if let Some(gpid) = existing_game_pid {
        let _ = tx.send_blocking(RunnerEvent::GameRunning { pid: gpid });
        monitor_until_exit(prefix, tx);
        return Ok(());
    }

    let log_path = revival_launcher_log_path(prefix);
    let mut log_offset: u64 = 0;

    // 2. If revival_launcher.exe is already waiting invisibly in background (e.g. after login prompt)
    if existing_launcher_pid.is_some() {
        if let Ok(meta) = fs::metadata(&log_path) {
            log_offset = meta.len();
        }
        if let Some(ref c) = creds {
            let _ = tx.send_blocking(RunnerEvent::Status {
                headline: "Hesaba giriş yapılıyor...".to_string(),
                detail: format!("{} hesabı doğrulanıyor", c.username),
                pulsing: true,
            });
            send_bridge_command(prefix, &format!("LOGIN\t{}\t{}\n", c.username, c.password))?;
        } else {
            let _ = tx.send_blocking(RunnerEvent::Status {
                headline: "Oyun başlatılıyor...".to_string(),
                detail: "Arka plan oturumundan bilet isteniyor".to_string(),
                pulsing: true,
            });
            send_bridge_command(prefix, "PLAY\n")?;
        }
    } else {
        // Fresh headless spawn
        let Some(runner) = select_runner(&cfg.runner_id) else {
            return Err(
                "Uyumluluk katmanı (UMU-Run, Proton veya Wine) bulunamadı! Ayarlar'dan kontrol edin."
                    .to_string(),
            );
        };

        // Pre-populate username in user.reg if credentials provided
        if let Some(ref c) = creds {
            let _ = write_login_prefs_to_user_reg(prefix, &c.username, c.remember);
        }

        // Clear stale IPC files & rotate previous launcher.log so we only parse fresh lines
        let bridge_dir = revival_local_appdata_dir(prefix);
        let _ = fs::remove_file(bridge_dir.join("goley_bridge_cmd.txt"));
        let _ = fs::remove_file(bridge_dir.join("goley_bridge_status.txt"));
        if log_path.is_file() {
            let prev = log_path.with_extension("prev.log");
            let _ = fs::rename(&log_path, prev);
        }

        let _ = tx.send_blocking(RunnerEvent::Status {
            headline: "Goley Revival Başlatılıyor...".to_string(),
            detail: format!("{} çalıştırılıyor", runner.label),
            pulsing: true,
        });

        let mut cmd = build_runner_command(cfg, &runner.kind, &launcher_exe)?;
        let _ = tx.send_blocking(RunnerEvent::LogLine(format!(
            "[goley-revival-linux] Başlatılıyor: {} (Prefix: {})",
            runner.label,
            prefix.display()
        )));

        cmd.current_dir(&rev_root)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());

        cmd.spawn()
            .map_err(|e| format!("Çalıştırıcı başlatılamadı: {}", e))?;
    }

    // 3. Tail launcher.log & drive headless QML bridge
    let start_wait = Instant::now();
    let mut phase_ready = existing_launcher_pid.is_some();
    let mut no_stored_session = false;
    let mut login_sent = existing_launcher_pid.is_some() && creds.is_some();
    let mut play_sent_at: Option<Instant> = if existing_launcher_pid.is_some() && creds.is_none() {
        Some(Instant::now())
    } else {
        None
    };
    let mut ticket_requested = false;
    let mut ready_since: Option<Instant> = if phase_ready { Some(Instant::now()) } else { None };

    loop {
        if start_wait.elapsed() > Duration::from_secs(150) {
            return Err("Başlatma zaman aşımına uğradı (150 sn). Canlı Günlükler'i kontrol edin.".to_string());
        }

        // Check if BinaryTrU.exe is already running!
        let (game_pid, launcher_pid) = find_running_pids();
        if let Some(gpid) = game_pid {
            let _ = tx.send_blocking(RunnerEvent::LogLine(format!(
                "[goley-revival-linux] BinaryTrU.exe çalışıyor (PID {}).",
                gpid
            )));
            let _ = tx.send_blocking(RunnerEvent::GameRunning { pid: gpid });
            monitor_until_exit(prefix, tx);
            return Ok(());
        }

        // If launcher exited unexpectedly after >8s without starting game
        if start_wait.elapsed() > Duration::from_secs(12) && phase_ready && launcher_pid.is_none() {
            let _ = tx.send_blocking(RunnerEvent::GameExited);
            return Ok(());
        }

        // Read newly appended lines from launcher.log
        if let Ok(mut file) = fs::File::open(&log_path) {
            if let Ok(meta) = file.metadata() {
                if meta.len() < log_offset {
                    log_offset = 0;
                }
                if meta.len() > log_offset && file.seek(SeekFrom::Start(log_offset)).is_ok() {
                    let mut reader = BufReader::new(file);
                    let mut line = String::new();
                    while let Ok(bytes_read) = reader.read_line(&mut line) {
                        if bytes_read == 0 {
                            break;
                        }
                        log_offset += bytes_read as u64;
                        let trimmed = line.trim().to_string();
                        line.clear();
                        if trimmed.is_empty() {
                            continue;
                        }

                        let _ = tx.send_blocking(RunnerEvent::LogLine(trimmed.clone()));

                        if trimmed.contains("PatchUpdater: checking") {
                            let _ = tx.send_blocking(RunnerEvent::Status {
                                headline: "Oyun dosyaları denetleniyor...".to_string(),
                                detail: "Resmi sunucu manifestosu doğrulanıyor".to_string(),
                                pulsing: true,
                            });
                        } else if trimmed.contains("phase Verifying -> Downloading")
                            || trimmed.contains("game files to download")
                        {
                            let _ = tx.send_blocking(RunnerEvent::Status {
                                headline: "Ek yama dosyaları indiriliyor...".to_string(),
                                detail: trimmed.clone(),
                                pulsing: true,
                            });
                        } else if trimmed.contains("AuthClient: no stored session") {
                            no_stored_session = true;
                        } else if trimmed.contains("-> Ready") {
                            phase_ready = true;
                            ready_since = Some(Instant::now());
                            let _ = tx.send_blocking(RunnerEvent::Status {
                                headline: "Goley Revival Hazır".to_string(),
                                detail: "Resmi başlatıcı açıldı · Oyuna girmek için 'OYNA' butonuna tıklayın".to_string(),
                                pulsing: false,
                            });
                        } else if let Some(idx) = trimmed.find("AuthClient: login failed ->") {
                            let reason = trimmed[idx + "AuthClient: login failed ->".len()..]
                                .trim()
                                .to_string();
                            let _ = tx.send_blocking(RunnerEvent::NeedLogin {
                                reason: Some(reason),
                            });
                            return Ok(());
                        } else if trimmed.contains("AuthClient: login succeeded") {
                            no_stored_session = false;
                            let uname = creds
                                .as_ref()
                                .map(|c| c.username.clone())
                                .unwrap_or_else(|| "Oyuncu".to_string());
                            let _ = tx.send_blocking(RunnerEvent::LoginSucceeded { username: uname });
                            let _ = tx.send_blocking(RunnerEvent::Status {
                                headline: "Giriş başarılı! Oyun başlatılıyor...".to_string(),
                                detail: "120 saniyelik tek kullanımlık oyun bileti alınıyor".to_string(),
                                pulsing: true,
                            });
                            std::thread::sleep(Duration::from_millis(350));
                            send_bridge_command(prefix, "PLAY\n")?;
                            play_sent_at = Some(Instant::now());
                        } else if trimmed.contains("AuthClient: ticket requested") {
                            ticket_requested = true;
                            let _ = tx.send_blocking(RunnerEvent::Status {
                                headline: "Sunucudan giriş bileti alınıyor...".to_string(),
                                detail: "Oyun istemcisi (BinaryTrU.exe) hazırlanıyor".to_string(),
                                pulsing: true,
                            });
                        } else if let Some(idx) = trimmed.find("ticket failed") {
                            let reason = trimmed[idx..].to_string();
                            return Err(format!("Oyun bileti alınamadı: {}", reason));
                        } else if trimmed.contains("GameLauncher: starting the game")
                            || trimmed.contains("GameLauncher: game started")
                        {
                            ticket_requested = true;
                            let _ = tx.send_blocking(RunnerEvent::Status {
                                headline: "Goley Revival açılıyor...".to_string(),
                                detail: "İyi oyunlar!".to_string(),
                                pulsing: true,
                            });
                        }
                    }
                }
            }
        }

        // Act once phase_ready is reached
        if phase_ready {
            let elapsed_ready = ready_since
                .map(|t| t.elapsed())
                .unwrap_or(Duration::ZERO);

            if no_stored_session {
                if let Some(ref c) = creds {
                    if !login_sent && elapsed_ready >= Duration::from_millis(400) {
                        login_sent = true;
                        let _ = tx.send_blocking(RunnerEvent::Status {
                            headline: "Hesaba giriş yapılıyor...".to_string(),
                            detail: format!("{} hesabı doğrulanıyor", c.username),
                            pulsing: true,
                        });
                        send_bridge_command(
                            prefix,
                            &format!("LOGIN\t{}\t{}\n", c.username, c.password),
                        )?;
                    }
                } else if elapsed_ready >= Duration::from_millis(300) {
                    let _ = tx.send_blocking(RunnerEvent::NeedLogin { reason: None });
                    return Ok(());
                }
            } else if play_sent_at.is_none() && elapsed_ready >= Duration::from_millis(650) {
                // Stored session is valid -> click StartButton!
                let _ = tx.send_blocking(RunnerEvent::Status {
                    headline: "Oturum doğrulandı, oyun başlatılıyor...".to_string(),
                    detail: "Sunucudan oyun bileti isteniyor".to_string(),
                    pulsing: true,
                });
                send_bridge_command(prefix, "PLAY\n")?;
                play_sent_at = Some(Instant::now());
            } else if let Some(sent_time) = play_sent_at {
                // Retry PLAY once if Qt6 window was still fading in during the first click
                if !ticket_requested && sent_time.elapsed() >= Duration::from_millis(2400) {
                    send_bridge_command(prefix, "PLAY\n")?;
                    play_sent_at = Some(Instant::now());
                }
            }
        }

        std::thread::sleep(Duration::from_millis(150));
    }
}

fn monitor_until_exit(prefix: &Path, tx: &Sender<RunnerEvent>) {
    loop {
        std::thread::sleep(Duration::from_millis(1500));
        let (game_pid, _) = find_running_pids();
        if game_pid.is_none() {
            // Game closed -> cleanly close hidden revival_launcher.exe too
            let _ = send_bridge_command(prefix, "QUIT\n");
            let _ = tx.send_blocking(RunnerEvent::LogLine(
                "[goley-revival-linux] Oyun kapandı, arka plan başlatıcısı sonlandırıldı.".to_string(),
            ));
            let _ = tx.send_blocking(RunnerEvent::GameExited);
            break;
        }
    }
}

fn build_runner_command(
    cfg: &AppConfig,
    kind: &RunnerKind,
    launcher_exe: &Path,
) -> Result<Command, String> {
    let mut wrappers: Vec<String> = Vec::new();

    if cfg.use_gamemode && which_binary("gamemoderun").is_some() {
        wrappers.push("gamemoderun".to_string());
    }
    if cfg.use_mangohud && which_binary("mangohud").is_some() {
        wrappers.push("mangohud".to_string());
    }
    if cfg.use_gamescope && which_binary("gamescope").is_some() {
        wrappers.push("gamescope".to_string());
        for arg in cfg.gamescope_args.split_whitespace() {
            wrappers.push(arg.to_string());
        }
        wrappers.push("--".to_string());
    }

    let mut base_args: Vec<String> = Vec::new();
    match kind {
        RunnerKind::Umu {
            umu_bin,
            proton_dir,
        } => {
            base_args.push(umu_bin.to_string_lossy().to_string());
            base_args.push(launcher_exe.to_string_lossy().to_string());
            let mut cmd = assemble_wrapped_command(&wrappers, &base_args);
            cmd.env("WINEPREFIX", &cfg.prefix_path);
            cmd.env("GAMEID", "revival");
            cmd.env("FAUGUSID", "revival");
            cmd.env("LOG_DIR", "revival");
            cmd.env("UMU_CONTAINER_NSENTER", "1");
            cmd.env("UMU_USE_STEAM", "1");
            if let Some(pdir) = proton_dir {
                if let Some(name) = pdir.file_name().and_then(|s| s.to_str()) {
                    cmd.env("PROTONPATH", name);
                } else {
                    cmd.env("PROTONPATH", pdir);
                }
            }
            apply_common_envs(&mut cmd, cfg);
            Ok(cmd)
        }
        RunnerKind::ProtonDirect { proton_script } => {
            base_args.push(proton_script.to_string_lossy().to_string());
            base_args.push("waitforexitandrun".to_string());
            base_args.push(launcher_exe.to_string_lossy().to_string());
            let mut cmd = assemble_wrapped_command(&wrappers, &base_args);
            cmd.env("STEAM_COMPAT_DATA_PATH", &cfg.prefix_path);
            let steam_client = dirs::home_dir()
                .map(|h| h.join(".local/share/Steam"))
                .unwrap_or_else(|| cfg.prefix_path.clone());
            cmd.env("STEAM_COMPAT_CLIENT_INSTALL_PATH", steam_client);
            apply_common_envs(&mut cmd, cfg);
            Ok(cmd)
        }
        RunnerKind::Wine { wine_bin } => {
            base_args.push(wine_bin.to_string_lossy().to_string());
            base_args.push(launcher_exe.to_string_lossy().to_string());
            let mut cmd = assemble_wrapped_command(&wrappers, &base_args);
            cmd.env("WINEPREFIX", &cfg.prefix_path);
            apply_common_envs(&mut cmd, cfg);
            Ok(cmd)
        }
    }
}

fn assemble_wrapped_command(wrappers: &[String], base_args: &[String]) -> Command {
    if wrappers.is_empty() {
        let mut c = Command::new(&base_args[0]);
        c.args(&base_args[1..]);
        c
    } else {
        let mut c = Command::new(&wrappers[0]);
        c.args(&wrappers[1..]);
        c.args(base_args);
        c
    }
}

fn apply_common_envs(cmd: &mut Command, cfg: &AppConfig) {
    cmd.env("WINEDLLOVERRIDES", "ncrypt=n,b;winedbg.exe=d");
    cmd.env("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "0");

    for token in cfg.extra_env.split_whitespace() {
        if let Some((k, v)) = token.split_once('=') {
            if !k.is_empty() {
                cmd.env(k, v);
            }
        }
    }
}

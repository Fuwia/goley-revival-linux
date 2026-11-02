use crate::config::AppConfig;
use crate::prefix::{install_ncrypt_shims, revival_game_dir, revival_root_dir};
use reqwest::blocking::Client;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const API_BASE: &str = "https://api.playrevival.co";
const DEFAULT_PATCH_BASE: &str = "https://patch.playrevival.co/";

#[derive(Debug, Clone, Deserialize)]
pub struct SlidesResponse {
    pub slides: Vec<SlideItem>,
}

#[derive(Debug, Clone, Deserialize)]
#[allow(dead_code)]
pub struct SlideItem {
    pub id: String,
    pub title: String,
    pub description: String,
    pub image_path: Option<String>,
}

#[derive(Debug, Clone)]
pub struct DecodedImage {
    pub width: i32,
    pub height: i32,
    pub rgba: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct PreparedSlide {
    pub item: SlideItem,
    pub image: Option<DecodedImage>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct NewsListResponse {
    pub news: Vec<NewsItem>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct NewsDetailResponse {
    pub news: NewsDetail,
}

#[derive(Debug, Clone, Deserialize)]
#[allow(dead_code)]
pub struct NewsItem {
    pub id: String,
    pub title: String,
    pub slug: Option<String>,
    pub summary: Option<String>,
    pub published_at: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[allow(dead_code)]
pub struct NewsDetail {
    pub id: String,
    pub title: String,
    pub summary: Option<String>,
    pub body: Option<String>,
    pub published_at: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[allow(dead_code)]
pub struct PatchManifest {
    pub schema_version: u32,
    pub version: String,
    pub files: Vec<ManifestFile>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ManifestFile {
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Debug, Clone)]
pub struct RemoteManifests {
    pub patch_base_url: String,
    pub launcher: PatchManifest,
    pub game: PatchManifest,
}

#[derive(Debug, Clone)]
pub enum LocalInstallState {
    NotInstalled {
        remote_version: String,
        total_bytes: u64,
        total_files: usize,
    },
    UpdateRequired {
        remote_version: String,
        missing_files: usize,
        download_bytes: u64,
    },
    UpToDate {
        version: String,
    },
}

#[derive(Debug, Clone)]
pub enum PatchProgress {
    Verifying {
        checked: usize,
        total: usize,
        current_file: String,
    },
    Downloading {
        downloaded_bytes: u64,
        total_bytes: u64,
        speed_mbps: f64,
        eta_secs: u64,
        files_done: usize,
        files_total: usize,
    },
}

pub fn build_http_client() -> Result<Client, String> {
    Client::builder()
        .user_agent("goley-revival-linux/0.1.0")
        .connect_timeout(Duration::from_secs(12))
        .timeout(Duration::from_secs(180))
        .build()
        .map_err(|e| format!("HTTP istemcisi oluşturulamadı: {}", e))
}

pub fn fetch_portal_content() -> Result<(Vec<PreparedSlide>, Vec<NewsItem>), String> {
    let client = build_http_client()?;

    let slides_resp: SlidesResponse = client
        .get(format!("{}/api/v1/content/slides", API_BASE))
        .send()
        .map_err(|e| format!("Sunucuya bağlanılamadı: {}", e))?
        .error_for_status()
        .map_err(|e| format!("Slayt yanıtı hatalı: {}", e))?
        .json()
        .map_err(|e| format!("Slayt JSON ayrıştırılamadı: {}", e))?;

    let news_resp: NewsListResponse = client
        .get(format!("{}/api/v1/content/news?limit=12&offset=0", API_BASE))
        .send()
        .map_err(|e| format!("Duyurular alınamadı: {}", e))?
        .error_for_status()
        .map_err(|e| format!("Duyuru yanıtı hatalı: {}", e))?
        .json()
        .map_err(|e| format!("Duyuru JSON ayrıştırılamadı: {}", e))?;

    let cache_slides_dir = AppConfig::cache_dir().join("slides");
    let _ = fs::create_dir_all(&cache_slides_dir);

    let mut prepared_slides = Vec::new();
    for slide in slides_resp.slides {
        let mut decoded = None;
        if let Some(ref rel_path) = slide.image_path {
            let url = if rel_path.starts_with("http") {
                rel_path.clone()
            } else {
                format!("{}{}", API_BASE, rel_path)
            };
            let cache_name = sanitize_cache_filename(rel_path);
            let cache_file = cache_slides_dir.join(cache_name);

            let img_bytes = if let Ok(bytes) = fs::read(&cache_file) {
                Some(bytes)
            } else if let Ok(resp) = client.get(&url).send() {
                if let Ok(bytes) = resp.bytes() {
                    let vec = bytes.to_vec();
                    let _ = fs::write(&cache_file, &vec);
                    Some(vec)
                } else {
                    None
                }
            } else {
                None
            };

            if let Some(bytes) = img_bytes {
                if let Ok(dyn_img) = image::load_from_memory(&bytes) {
                    let rgba_img = dyn_img.to_rgba8();
                    decoded = Some(DecodedImage {
                        width: rgba_img.width() as i32,
                        height: rgba_img.height() as i32,
                        rgba: rgba_img.into_raw(),
                    });
                }
            }
        }
        prepared_slides.push(PreparedSlide {
            item: slide,
            image: decoded,
        });
    }

    Ok((prepared_slides, news_resp.news))
}

pub fn fetch_news_detail(id: &str) -> Result<NewsDetail, String> {
    let client = build_http_client()?;
    let resp: NewsDetailResponse = client
        .get(format!("{}/api/v1/content/news/{}", API_BASE, id))
        .send()
        .map_err(|e| format!("Duyuru detayı alınamadı: {}", e))?
        .error_for_status()
        .map_err(|e| format!("Duyuru sunucu hatası: {}", e))?
        .json()
        .map_err(|e| format!("Duyuru içeriği okunamadı: {}", e))?;
    Ok(resp.news)
}

pub fn fetch_remote_manifests() -> Result<RemoteManifests, String> {
    let client = build_http_client()?;

    let launcher_resp = client
        .get(format!("{}/api/v1/patch/launcher", API_BASE))
        .send()
        .map_err(|e| format!("Başlatıcı manifestosu alınamadı: {}", e))?
        .error_for_status()
        .map_err(|e| format!("Başlatıcı manifest hatası: {}", e))?;

    let mut patch_base = launcher_resp
        .headers()
        .get("x-patch-base-url")
        .and_then(|h| h.to_str().ok())
        .unwrap_or(DEFAULT_PATCH_BASE)
        .to_string();
    if !patch_base.ends_with('/') {
        patch_base.push('/');
    }

    let launcher: PatchManifest = launcher_resp
        .json()
        .map_err(|e| format!("Başlatıcı manifest JSON hatası: {}", e))?;

    let game: PatchManifest = client
        .get(format!("{}/api/v1/patch/live", API_BASE))
        .send()
        .map_err(|e| format!("Oyun manifestosu alınamadı: {}", e))?
        .error_for_status()
        .map_err(|e| format!("Oyun manifest hatası: {}", e))?
        .json()
        .map_err(|e| format!("Oyun manifest JSON hatası: {}", e))?;

    Ok(RemoteManifests {
        patch_base_url: patch_base,
        launcher,
        game,
    })
}

pub fn check_install_status_fast(prefix: &Path, manifests: &RemoteManifests) -> LocalInstallState {
    let rev_root = revival_root_dir(prefix);
    let game_dir = revival_game_dir(prefix);

    let launcher_exe = rev_root.join("revival_launcher.exe");
    let game_exe = game_dir.join("BinaryTr/BinaryTrU.exe");

    let total_all_bytes: u64 = manifests.launcher.files.iter().map(|f| f.size).sum::<u64>()
        + manifests.game.files.iter().map(|f| f.size).sum::<u64>();
    let total_all_files = manifests.launcher.files.len() + manifests.game.files.len();

    if !launcher_exe.is_file() && !game_exe.is_file() {
        return LocalInstallState::NotInstalled {
            remote_version: manifests.game.version.clone(),
            total_bytes: total_all_bytes,
            total_files: total_all_files,
        };
    }

    let mut missing_files = 0usize;
    let mut download_bytes = 0u64;

    for f in &manifests.launcher.files {
        let local_path = rev_root.join(&f.path);
        match fs::metadata(&local_path) {
            Ok(meta) if meta.is_file() && meta.len() == f.size => {}
            _ => {
                missing_files += 1;
                download_bytes += f.size;
            }
        }
    }

    for f in &manifests.game.files {
        let local_path = game_dir.join(&f.path);
        match fs::metadata(&local_path) {
            Ok(meta) if meta.is_file() && meta.len() == f.size => {}
            _ => {
                missing_files += 1;
                download_bytes += f.size;
            }
        }
    }

    if missing_files > 0 {
        LocalInstallState::UpdateRequired {
            remote_version: manifests.game.version.clone(),
            missing_files,
            download_bytes,
        }
    } else {
        LocalInstallState::UpToDate {
            version: manifests.game.version.clone(),
        }
    }
}

#[derive(Clone)]
struct DownloadTask {
    dest_path: PathBuf,
    rel_name: String,
    size: u64,
    sha256: String,
}

pub fn verify_and_patch<F>(
    prefix: &Path,
    manifests: &RemoteManifests,
    full_sha256_check: bool,
    mut on_progress: F,
) -> Result<usize, String>
where
    F: FnMut(PatchProgress) + Send,
{
    let rev_root = revival_root_dir(prefix);
    let game_dir = revival_game_dir(prefix);

    fs::create_dir_all(&rev_root).map_err(|e| format!("Kurulum dizini açılamadı: {}", e))?;
    fs::create_dir_all(&game_dir).map_err(|e| format!("Oyun dizini açılamadı: {}", e))?;

    let mut all_targets = Vec::with_capacity(manifests.launcher.files.len() + manifests.game.files.len());
    for f in &manifests.launcher.files {
        all_targets.push(DownloadTask {
            dest_path: rev_root.join(&f.path),
            rel_name: f.path.clone(),
            size: f.size,
            sha256: f.sha256.to_lowercase(),
        });
    }
    for f in &manifests.game.files {
        all_targets.push(DownloadTask {
            dest_path: game_dir.join(&f.path),
            rel_name: f.path.clone(),
            size: f.size,
            sha256: f.sha256.to_lowercase(),
        });
    }

    let total_count = all_targets.len();
    let mut to_download = Vec::new();

    for (idx, task) in all_targets.into_iter().enumerate() {
        on_progress(PatchProgress::Verifying {
            checked: idx + 1,
            total: total_count,
            current_file: task.rel_name.clone(),
        });

        let needs_dl = match fs::metadata(&task.dest_path) {
            Ok(meta) if meta.is_file() && meta.len() == task.size => {
                if full_sha256_check {
                    match compute_file_sha256(&task.dest_path) {
                        Ok(hash) => hash != task.sha256,
                        Err(_) => true,
                    }
                } else {
                    false
                }
            }
            _ => true,
        };

        if needs_dl {
            to_download.push(task);
        }
    }

    let files_total = to_download.len();
    if files_total == 0 {
        install_ncrypt_shims(prefix)?;
        return Ok(0);
    }

    let total_bytes: u64 = to_download.iter().map(|t| t.size).sum();
    let downloaded_bytes = Arc::new(AtomicU64::new(0));
    let files_done = Arc::new(AtomicU64::new(0));
    let cancelled = Arc::new(AtomicBool::new(false));
    let first_error: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let queue = Arc::new(Mutex::new(to_download));

    let client = build_http_client()?;
    let worker_count = files_total.min(6).max(1);
    let patch_base = manifests.patch_base_url.clone();

    let mut handles = Vec::new();
    for _ in 0..worker_count {
        let q = Arc::clone(&queue);
        let dl_bytes = Arc::clone(&downloaded_bytes);
        let f_done = Arc::clone(&files_done);
        let cancel = Arc::clone(&cancelled);
        let err_slot = Arc::clone(&first_error);
        let http = client.clone();
        let base_url = patch_base.clone();

        handles.push(std::thread::spawn(move || {
            loop {
                if cancel.load(Ordering::Relaxed) {
                    break;
                }
                let next_task = {
                    let mut guard = q.lock().unwrap();
                    guard.pop()
                };
                let Some(task) = next_task else { break };

                if let Err(e) = download_single_object(&http, &base_url, &task, &dl_bytes, &cancel) {
                    cancel.store(true, Ordering::Relaxed);
                    let mut g = err_slot.lock().unwrap();
                    if g.is_none() {
                        *g = Some(e);
                    }
                    break;
                }
                f_done.fetch_add(1, Ordering::Relaxed);
            }
        }));
    }

    let start_instant = Instant::now();
    loop {
        let done = files_done.load(Ordering::Relaxed) as usize;
        let bytes = downloaded_bytes.load(Ordering::Relaxed);
        let elapsed = start_instant.elapsed().as_secs_f64().max(0.1);
        let speed_bps = bytes as f64 / elapsed;
        let speed_mbps = speed_bps / (1024.0 * 1024.0);
        let rem_bytes = total_bytes.saturating_sub(bytes);
        let eta_secs = if speed_bps > 1024.0 {
            (rem_bytes as f64 / speed_bps) as u64
        } else {
            0
        };

        on_progress(PatchProgress::Downloading {
            downloaded_bytes: bytes,
            total_bytes,
            speed_mbps,
            eta_secs,
            files_done: done,
            files_total,
        });

        if done >= files_total || cancelled.load(Ordering::Relaxed) {
            break;
        }
        std::thread::sleep(Duration::from_millis(120));
    }

    for h in handles {
        let _ = h.join();
    }

    if let Some(err) = first_error.lock().unwrap().take() {
        return Err(err);
    }

    // Ensure our x64 + x86 ncrypt.dll shims are freshly deployed after patching!
    install_ncrypt_shims(prefix)?;

    Ok(files_total)
}

fn download_single_object(
    client: &Client,
    patch_base: &str,
    task: &DownloadTask,
    dl_bytes: &Arc<AtomicU64>,
    cancel: &Arc<AtomicBool>,
) -> Result<(), String> {
    if let Some(parent) = task.dest_path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("Klasör hatası ({}): {}", task.rel_name, e))?;
    }

    let url = format!("{}objects/{}", patch_base, task.sha256);
    let mut resp = client
        .get(&url)
        .send()
        .map_err(|e| format!("İndirme hatası ({}): {}", task.rel_name, e))?
        .error_for_status()
        .map_err(|e| format!("CDN HTTP hatası ({}): {}", task.rel_name, e))?;

    let tmp_path = task.dest_path.with_extension("goley_part");
    let mut out_file = fs::File::create(&tmp_path)
        .map_err(|e| format!("Dosya oluşturulamadı ({}): {}", task.rel_name, e))?;

    let mut hasher = Sha256::new();
    let mut buf = [0u8; 65536];
    let mut written_this_file = 0u64;

    loop {
        if cancel.load(Ordering::Relaxed) {
            let _ = fs::remove_file(&tmp_path);
            return Err("İndirme iptal edildi".to_string());
        }
        let n = resp
            .read(&mut buf)
            .map_err(|e| {
                let _ = fs::remove_file(&tmp_path);
                format!("Ağ okuma hatası ({}): {}", task.rel_name, e)
            })?;
        if n == 0 {
            break;
        }
        out_file.write_all(&buf[..n]).map_err(|e| {
            let _ = fs::remove_file(&tmp_path);
            format!("Disk yazma hatası ({}): {}", task.rel_name, e)
        })?;
        hasher.update(&buf[..n]);
        written_this_file += n as u64;
        dl_bytes.fetch_add(n as u64, Ordering::Relaxed);
    }

    out_file.sync_all().ok();
    drop(out_file);

    let got_hash = hex::encode(hasher.finalize());
    if got_hash != task.sha256 {
        let _ = fs::remove_file(&tmp_path);
        dl_bytes.fetch_sub(written_this_file, Ordering::Relaxed);
        return Err(format!(
            "SHA-256 doğrulama hatası ({}): beklenen {}, alınan {}",
            task.rel_name, task.sha256, got_hash
        ));
    }

    fs::rename(&tmp_path, &task.dest_path).map_err(|e| {
        let _ = fs::remove_file(&tmp_path);
        format!("Dosya yerleştirilemedi ({}): {}", task.rel_name, e)
    })
}

pub fn compute_file_sha256(path: &Path) -> Result<String, String> {
    let mut f = fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 65536];
    loop {
        let n = f.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(hex::encode(hasher.finalize()))
}

fn sanitize_cache_filename(rel: &str) -> String {
    rel.chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' { c } else { '_' })
        .collect()
}

pub fn format_turkish_date(iso: Option<&str>) -> String {
    let Some(raw) = iso else {
        return "Duyuru".to_string();
    };
    let date_part = raw.split('T').next().unwrap_or(raw);
    let parts: Vec<&str> = date_part.split('-').collect();
    if parts.len() == 3 {
        let year = parts[0];
        let month = match parts[1] {
            "01" => "Oca",
            "02" => "Şub",
            "03" => "Mar",
            "04" => "Nis",
            "05" => "May",
            "06" => "Haz",
            "07" => "Tem",
            "08" => "Ağu",
            "09" => "Eyl",
            "10" => "Eki",
            "11" => "Kas",
            "12" => "Ara",
            m => m,
        };
        let day = parts[2].trim_start_matches('0');
        return format!("{} {} {}", if day.is_empty() { "1" } else { day }, month, year);
    }
    date_part.to_string()
}

pub fn format_bytes_human(bytes: u64) -> String {
    const MB: f64 = 1024.0 * 1024.0;
    const GB: f64 = 1024.0 * 1024.0 * 1024.0;
    if bytes as f64 >= GB {
        format!("{:.2} GB", bytes as f64 / GB)
    } else {
        format!("{:.1} MB", (bytes as f64 / MB).max(0.1))
    }
}

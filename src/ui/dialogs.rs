use crate::api::{fetch_news_detail, format_turkish_date, NewsItem};
use crate::config::{which_binary, AppConfig};
use crate::prefix::{
    discover_runners, install_ncrypt_shims, revival_game_dir, revival_launcher_log_path,
};
use crate::runner::LoginCredentials;
use crate::ui::style::apply_color_scheme;
use adw::prelude::*;
use gtk::{gdk, gio, glib};
use std::cell::RefCell;
use std::fs;
use std::path::Path;
use std::process::Command;
use std::rc::Rc;

pub fn open_uri_or_path(parent: &impl IsA<gtk::Window>, uri: &str) {
    let launcher = gtk::UriLauncher::new(uri);
    launcher.launch(Some(parent), gio::Cancellable::NONE, |_| {});
}

pub fn open_local_folder(parent: &impl IsA<gtk::Window>, path: &Path) {
    let _ = fs::create_dir_all(path);
    let uri = format!("file://{}", path.display());
    open_uri_or_path(parent, &uri);
}

pub fn present_login_dialog<F>(
    window: &adw::ApplicationWindow,
    initial_username: Option<String>,
    error_reason: Option<String>,
    on_submit: F,
) where
    F: Fn(LoginCredentials) + 'static,
{
    let dialog = adw::Dialog::builder()
        .title("Goley Revival Hesap Girişi")
        .content_width(430)
        .build();

    let toolbar = adw::ToolbarView::new();
    let header = adw::HeaderBar::new();
    toolbar.add_top_bar(&header);

    let outer = gtk::Box::new(gtk::Orientation::Vertical, 16);
    outer.set_margin_top(8);
    outer.set_margin_bottom(22);
    outer.set_margin_start(22);
    outer.set_margin_end(22);

    if let Some(reason) = error_reason {
        let banner = adw::Banner::new(&reason);
        banner.set_revealed(true);
        outer.append(&banner);
    }

    let intro = gtk::Label::new(Some(
        "Hesap bilgileriniz doğrudan resmi Goley Revival kimlik doğrulama istemcisine iletilir. Şifreniz diske kaydedilmez.",
    ));
    intro.set_wrap(true);
    intro.set_xalign(0.0);
    intro.add_css_class("dim-label");
    intro.add_css_class("caption");
    outer.append(&intro);

    let group = adw::PreferencesGroup::new();

    let user_row = adw::EntryRow::builder()
        .title("Kullanıcı Adı")
        .show_apply_button(false)
        .build();
    if let Some(u) = initial_username {
        user_row.set_text(&u);
    }

    let pass_row = adw::PasswordEntryRow::builder()
        .title("Şifre")
        .build();

    let remember_row = adw::SwitchRow::builder()
        .title("Beni Hatırla")
        .subtitle("Oturum anahtarını ve kullanıcı adını sonraki girişler için sakla")
        .active(true)
        .build();

    group.add(&user_row);
    group.add(&pass_row);
    group.add(&remember_row);
    outer.append(&group);

    let btn_row = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    btn_row.set_halign(gtk::Align::Fill);

    let reg_btn = gtk::Button::with_label("Hesap Oluştur");
    reg_btn.add_css_class("flat");
    {
        let win_weak = window.downgrade();
        reg_btn.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                open_uri_or_path(&w, "https://playrevival.co/register");
            }
        });
    }

    let spacer = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    spacer.set_hexpand(true);

    let submit_btn = gtk::Button::with_label("Giriş Yap ve Oyna");
    submit_btn.add_css_class("suggested-action");
    submit_btn.add_css_class("pill");

    btn_row.append(&reg_btn);
    btn_row.append(&spacer);
    btn_row.append(&submit_btn);
    outer.append(&btn_row);

    let on_submit_rc = Rc::new(on_submit);

    let trigger_submit = {
        let dialog_weak = dialog.downgrade();
        let u_row = user_row.clone();
        let p_row = pass_row.clone();
        let r_row = remember_row.clone();
        let cb = Rc::clone(&on_submit_rc);
        move || {
            let username = u_row.text().trim().to_string();
            let password = p_row.text().to_string();
            if username.is_empty() || password.is_empty() {
                return;
            }
            if let Some(d) = dialog_weak.upgrade() {
                d.close();
            }
            cb(LoginCredentials {
                username,
                password,
                remember: r_row.is_active(),
            });
        }
    };

    let ts_click = trigger_submit.clone();
    submit_btn.connect_clicked(move |_| ts_click());

    let ts_enter = trigger_submit;
    pass_row.connect_entry_activated(move |_| ts_enter());

    toolbar.set_content(Some(&outer));
    dialog.set_child(Some(&toolbar));
    dialog.present(Some(window));
}

pub fn present_news_dialog(window: &adw::ApplicationWindow, item: &NewsItem) {
    let dialog = adw::Dialog::builder()
        .title(&item.title)
        .content_width(640)
        .content_height(500)
        .build();

    let toolbar = adw::ToolbarView::new();
    let header = adw::HeaderBar::new();
    toolbar.add_top_bar(&header);

    let scroller = gtk::ScrolledWindow::builder()
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .build();

    let body_box = gtk::Box::new(gtk::Orientation::Vertical, 14);
    body_box.set_margin_top(16);
    body_box.set_margin_bottom(24);
    body_box.set_margin_start(24);
    body_box.set_margin_end(24);

    let date_lbl = gtk::Label::new(Some(&format_turkish_date(item.published_at.as_deref())));
    date_lbl.set_xalign(0.0);
    date_lbl.add_css_class("dim-label");
    date_lbl.add_css_class("caption-heading");
    body_box.append(&date_lbl);

    let title_lbl = gtk::Label::new(Some(&item.title));
    title_lbl.set_wrap(true);
    title_lbl.set_xalign(0.0);
    title_lbl.add_css_class("title-2");
    body_box.append(&title_lbl);

    let sep = gtk::Separator::new(gtk::Orientation::Horizontal);
    body_box.append(&sep);

    let content_lbl = gtk::Label::new(Some(
        item.summary
            .as_deref()
            .unwrap_or("Duyuru içeriği yükleniyor..."),
    ));
    content_lbl.set_wrap(true);
    content_lbl.set_selectable(true);
    content_lbl.set_xalign(0.0);
    content_lbl.set_yalign(0.0);
    body_box.append(&content_lbl);

    let spinner = gtk::Spinner::new();
    spinner.set_halign(gtk::Align::Center);
    spinner.start();
    body_box.append(&spinner);

    scroller.set_child(Some(&body_box));
    toolbar.set_content(Some(&scroller));
    dialog.set_child(Some(&toolbar));
    dialog.present(Some(window));

    let news_id = item.id.clone();
    let fallback_summary = item.summary.clone().unwrap_or_default();
    let (tx, rx) = async_channel::bounded(1);
    std::thread::spawn(move || {
        let res = fetch_news_detail(&news_id);
        let _ = tx.send_blocking(res);
    });

    glib::spawn_future_local(async move {
        if let Ok(result) = rx.recv().await {
            spinner.stop();
            spinner.set_visible(false);
            match result {
                Ok(detail) => {
                    let text = detail
                        .body
                        .filter(|b| !b.trim().is_empty())
                        .or(detail.summary)
                        .unwrap_or(fallback_summary);
                    content_lbl.set_text(&strip_simple_html(&text));
                }
                Err(err) => {
                    if fallback_summary.is_empty() {
                        content_lbl.set_text(&err);
                    }
                }
            }
        }
    });
}

fn strip_simple_html(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len());
    let mut in_tag = false;
    let normalized = raw
        .replace("<br>", "\n")
        .replace("<br/>", "\n")
        .replace("<br />", "\n")
        .replace("</p>", "\n\n")
        .replace("</li>", "\n")
        .replace("<li>", "  • ");

    for ch in normalized.chars() {
        match ch {
            '<' => in_tag = true,
            '>' => in_tag = false,
            _ if !in_tag => out.push(ch),
            _ => {}
        }
    }
    out.trim().to_string()
}

pub fn present_logs_dialog(
    window: &adw::ApplicationWindow,
    prefix: &Path,
    session_logs: &Rc<RefCell<Vec<String>>>,
    toast_overlay: &adw::ToastOverlay,
) {
    let dialog = adw::Dialog::builder()
        .title("Canlı Günlükler (Logs)")
        .content_width(760)
        .content_height(520)
        .build();

    let toolbar = adw::ToolbarView::new();
    let header = adw::HeaderBar::new();

    let log_file_path = revival_launcher_log_path(prefix);

    let copy_btn = gtk::Button::from_icon_name("edit-copy-symbolic");
    copy_btn.set_tooltip_text(Some("Günlükleri Panoya Kopyala"));

    let folder_btn = gtk::Button::from_icon_name("folder-open-symbolic");
    folder_btn.set_tooltip_text(Some("Log Klasörünü Aç"));

    header.pack_start(&folder_btn);
    header.pack_end(&copy_btn);
    toolbar.add_top_bar(&header);

    let scroller = gtk::ScrolledWindow::builder()
        .vexpand(true)
        .hexpand(true)
        .build();

    let text_view = gtk::TextView::builder()
        .editable(false)
        .monospace(true)
        .wrap_mode(gtk::WrapMode::WordChar)
        .top_margin(12)
        .bottom_margin(12)
        .left_margin(14)
        .right_margin(14)
        .build();
    text_view.add_css_class("log-view");

    let combined_text = build_combined_logs(&log_file_path, &session_logs.borrow());
    text_view.buffer().set_text(&combined_text);

    {
        let tv = text_view.clone();
        let toast_ov = toast_overlay.clone();
        copy_btn.connect_clicked(move |_| {
            let buf = tv.buffer();
            let text = buf.text(&buf.start_iter(), &buf.end_iter(), false);
            if let Some(display) = gdk::Display::default() {
                display.clipboard().set_text(&text);
                toast_ov.add_toast(adw::Toast::new("Günlükler panoya kopyalandı"));
            }
        });
    }

    {
        let win_weak = window.downgrade();
        let log_dir = log_file_path
            .parent()
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| prefix.to_path_buf());
        folder_btn.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                open_local_folder(&w, &log_dir);
            }
        });
    }

    scroller.set_child(Some(&text_view));
    toolbar.set_content(Some(&scroller));
    dialog.set_child(Some(&toolbar));
    dialog.present(Some(window));
}

fn build_combined_logs(log_path: &Path, mem_lines: &[String]) -> String {
    let mut out = String::new();
    if !mem_lines.is_empty() {
        out.push_str("=== Oturum Kayıtları ===\n");
        for l in mem_lines {
            out.push_str(l);
            out.push('\n');
        }
        out.push('\n');
    }
    if let Ok(disk_log) = fs::read_to_string(log_path) {
        out.push_str(&format!("=== {} ===\n", log_path.display()));
        let lines: Vec<&str> = disk_log.lines().collect();
        let start = lines.len().saturating_sub(250);
        for l in &lines[start..] {
            out.push_str(l);
            out.push('\n');
        }
    } else if out.is_empty() {
        out.push_str("Henüz günlük kaydı oluşmadı.");
    }
    out
}

pub fn present_preferences_dialog<FRepair, FVerify>(
    window: &adw::ApplicationWindow,
    config_rc: &Rc<RefCell<AppConfig>>,
    toast_overlay: &adw::ToastOverlay,
    on_config_changed: Rc<dyn Fn()>,
    on_repair_shims: FRepair,
    on_verify_files: FVerify,
) where
    FRepair: Fn() + 'static,
    FVerify: Fn() + 'static,
{
    let prefs = adw::PreferencesDialog::builder()
        .title("Başlatıcı Ayarları")
        .build();

    // ── PAGE 1: Oyun ve Uyumluluk ──────────────────────────────
    let page_compat = adw::PreferencesPage::builder()
        .title("Uyumluluk")
        .icon_name("applications-games-symbolic")
        .build();

    let group_runner = adw::PreferencesGroup::builder()
        .title("Uyumluluk Katmanı (Wine / Proton / UMU)")
        .description("Goley Revival'ın çalışacağı uyumluluk katmanını ve Wine önekini (prefix) yapılandırın.")
        .build();

    let discovered = discover_runners();
    let mut runner_ids = vec!["auto".to_string()];
    let mut runner_labels = vec!["Otomatik Seç (Önerilen)".to_string()];
    for r in &discovered {
        runner_ids.push(r.id.clone());
        runner_labels.push(r.label.clone());
    }
    let label_refs: Vec<&str> = runner_labels.iter().map(|s| s.as_str()).collect();
    let model = gtk::StringList::new(&label_refs);

    let current_runner_id = config_rc.borrow().runner_id.clone();
    let selected_idx = runner_ids
        .iter()
        .position(|id| id == &current_runner_id)
        .unwrap_or(0);

    let runner_combo = adw::ComboRow::builder()
        .title("Çalıştırıcı (Runner)")
        .subtitle("Proton-GE + UMU-Run en yüksek uyumluluğu sağlar")
        .model(&model)
        .selected(selected_idx as u32)
        .build();

    {
        let cfg = Rc::clone(config_rc);
        let ids = runner_ids.clone();
        let notify_cb = Rc::clone(&on_config_changed);
        runner_combo.connect_selected_notify(move |combo| {
            let idx = combo.selected() as usize;
            if let Some(chosen) = ids.get(idx) {
                cfg.borrow_mut().runner_id = chosen.clone();
                let _ = cfg.borrow().save();
                notify_cb();
            }
        });
    }
    group_runner.add(&runner_combo);

    // Prefix row
    let prefix_str = config_rc.borrow().prefix_path.display().to_string();
    let prefix_row = adw::ActionRow::builder()
        .title("Wine Prefix Dizini")
        .subtitle(&prefix_str)
        .build();

    let btn_open_pfx = gtk::Button::from_icon_name("folder-open-symbolic");
    btn_open_pfx.add_css_class("flat");
    btn_open_pfx.set_valign(gtk::Align::Center);
    btn_open_pfx.set_tooltip_text(Some("Prefix Klasörünü Aç"));

    let btn_pick_pfx = gtk::Button::with_label("Değiştir...");
    btn_pick_pfx.set_valign(gtk::Align::Center);

    prefix_row.add_suffix(&btn_open_pfx);
    prefix_row.add_suffix(&btn_pick_pfx);

    {
        let win_weak = window.downgrade();
        let cfg = Rc::clone(config_rc);
        btn_open_pfx.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                open_local_folder(&w, &cfg.borrow().prefix_path);
            }
        });
    }

    {
        let win_weak = window.downgrade();
        let cfg = Rc::clone(config_rc);
        let row_clone = prefix_row.clone();
        let notify_cb = Rc::clone(&on_config_changed);
        btn_pick_pfx.connect_clicked(move |_| {
            let Some(win) = win_weak.upgrade() else { return };
            let file_dlg = gtk::FileDialog::builder()
                .title("Wine Prefix Klasörünü Seçin")
                .build();
            let cfg_inner = Rc::clone(&cfg);
            let row_inner = row_clone.clone();
            let cb_inner = Rc::clone(&notify_cb);
            file_dlg.select_folder(Some(&win), gio::Cancellable::NONE, move |res| {
                if let Ok(folder) = res {
                    if let Some(path) = folder.path() {
                        row_inner.set_subtitle(&path.display().to_string());
                        cfg_inner.borrow_mut().prefix_path = path;
                        let _ = cfg_inner.borrow().save();
                        cb_inner();
                    }
                }
            });
        });
    }
    group_runner.add(&prefix_row);

    // Game directory quick open row
    let game_dir_str = revival_game_dir(&config_rc.borrow().prefix_path)
        .display()
        .to_string();
    let game_dir_row = adw::ActionRow::builder()
        .title("Oyun Dosyaları Dizini")
        .subtitle(&game_dir_str)
        .build();
    let btn_open_game = gtk::Button::from_icon_name("folder-open-symbolic");
    btn_open_game.add_css_class("flat");
    btn_open_game.set_valign(gtk::Align::Center);
    {
        let win_weak = window.downgrade();
        let cfg = Rc::clone(config_rc);
        btn_open_game.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                let p = revival_game_dir(&cfg.borrow().prefix_path);
                open_local_folder(&w, &p);
            }
        });
    }
    game_dir_row.add_suffix(&btn_open_game);
    group_runner.add(&game_dir_row);
    page_compat.add(&group_runner);

    // Performance group
    let group_perf = adw::PreferencesGroup::builder()
        .title("Performans ve Görüntü")
        .build();

    let has_gamemode = which_binary("gamemoderun").is_some();
    let sw_gamemode = adw::SwitchRow::builder()
        .title("Feral GameMode")
        .subtitle(if has_gamemode {
            "Oyun sırasında işlemci valisini (CPU governor) performans moduna alır"
        } else {
            "Sistemde 'gamemode' paketi bulunamadı"
        })
        .sensitive(has_gamemode)
        .active(config_rc.borrow().use_gamemode && has_gamemode)
        .build();
    {
        let cfg = Rc::clone(config_rc);
        sw_gamemode.connect_active_notify(move |sw| {
            cfg.borrow_mut().use_gamemode = sw.is_active();
            let _ = cfg.borrow().save();
        });
    }
    group_perf.add(&sw_gamemode);

    let has_mangohud = which_binary("mangohud").is_some();
    let sw_mangohud = adw::SwitchRow::builder()
        .title("MangoHud Göster")
        .subtitle(if has_mangohud {
            "Oyun içinde FPS, kare süresi ve donanım kullanımını gösterir"
        } else {
            "Sistemde 'mangohud' paketi bulunamadı"
        })
        .sensitive(has_mangohud)
        .active(config_rc.borrow().use_mangohud && has_mangohud)
        .build();
    {
        let cfg = Rc::clone(config_rc);
        sw_mangohud.connect_active_notify(move |sw| {
            cfg.borrow_mut().use_mangohud = sw.is_active();
            let _ = cfg.borrow().save();
        });
    }
    group_perf.add(&sw_mangohud);

    let has_gamescope = which_binary("gamescope").is_some();
    let sw_gamescope = adw::SwitchRow::builder()
        .title("Gamescope Mikro-Kompozitör")
        .subtitle(if has_gamescope {
            "Oyunu izole Wayland mikro-kompozitörü içinde çalıştırır"
        } else {
            "Sistemde 'gamescope' paketi bulunamadı"
        })
        .sensitive(has_gamescope)
        .active(config_rc.borrow().use_gamescope && has_gamescope)
        .build();
    {
        let cfg = Rc::clone(config_rc);
        sw_gamescope.connect_active_notify(move |sw| {
            cfg.borrow_mut().use_gamescope = sw.is_active();
            let _ = cfg.borrow().save();
        });
    }
    group_perf.add(&sw_gamescope);

    let env_row = adw::EntryRow::builder()
        .title("Ek Ortam Değişkenleri (Örn: DXVK_HUD=fps)")
        .text(&config_rc.borrow().extra_env)
        .build();
    {
        let cfg = Rc::clone(config_rc);
        env_row.connect_changed(move |row| {
            cfg.borrow_mut().extra_env = row.text().to_string();
            let _ = cfg.borrow().save();
        });
    }
    group_perf.add(&env_row);

    // Theme selector
    let theme_model = gtk::StringList::new(&["Koyu (Varsayılan)", "Sistem Teması", "Açık"]);
    let cur_scheme = config_rc.borrow().color_scheme.clone();
    let scheme_idx = match cur_scheme.as_str() {
        "system" => 1,
        "light" => 2,
        _ => 0,
    };
    let theme_combo = adw::ComboRow::builder()
        .title("Arayüz Görünümü")
        .model(&theme_model)
        .selected(scheme_idx)
        .build();
    {
        let cfg = Rc::clone(config_rc);
        theme_combo.connect_selected_notify(move |combo| {
            let val = match combo.selected() {
                1 => "system",
                2 => "light",
                _ => "dark",
            };
            apply_color_scheme(val);
            cfg.borrow_mut().color_scheme = val.to_string();
            let _ = cfg.borrow().save();
        });
    }
    group_perf.add(&theme_combo);
    page_compat.add(&group_perf);
    prefs.add(&page_compat);

    // ── PAGE 2: Bakım ve Sorun Giderme ─────────────────────────
    let page_maint = adw::PreferencesPage::builder()
        .title("Bakım ve Onarım")
        .icon_name("emblem-system-symbolic")
        .build();

    let group_fixes = adw::PreferencesGroup::builder()
        .title("Linux Uyumluluk Yamaları ve Dosya Onarımı")
        .description("Goley Revival'ın Linux'ta kesintisiz çalışması için gereken bileşenler.")
        .build();

    let row_shim = adw::ActionRow::builder()
        .title("Linux Uyumluluk Köprüleri (ncrypt.dll & dinput8.dll)")
        .subtitle("ECDSA_P256 oturum şifreleme ve oyun içi DirectInput8 ok/yön tuşları köprüsü")
        .build();
    let btn_shim = gtk::Button::with_label("Yeniden Kur");
    btn_shim.set_valign(gtk::Align::Center);
    {
        let cfg = Rc::clone(config_rc);
        let toast_ov = toast_overlay.clone();
        btn_shim.connect_clicked(move |_| {
            match install_ncrypt_shims(&cfg.borrow().prefix_path) {
                Ok(_) => {
                    on_repair_shims();
                    toast_ov.add_toast(adw::Toast::new(
                        "ncrypt.dll ve dinput8.dll köprüleri başarıyla kuruldu",
                    ));
                }
                Err(e) => {
                    toast_ov.add_toast(adw::Toast::new(&format!("Hata: {}", e)));
                }
            }
        });
    }
    row_shim.add_suffix(&btn_shim);
    group_fixes.add(&row_shim);

    let row_verify = adw::ActionRow::builder()
        .title("Oyun Dosyalarını Doğrula (SHA-256)")
        .subtitle("Tüm 168 oyun dosyasını resmi sunucu özetleriyle karşılaştırır ve eksikleri indirir")
        .build();
    let btn_verify = gtk::Button::with_label("Şimdi Doğrula");
    btn_verify.set_valign(gtk::Align::Center);
    {
        let prefs_weak = prefs.downgrade();
        btn_verify.connect_clicked(move |_| {
            if let Some(p) = prefs_weak.upgrade() {
                p.close();
            }
            on_verify_files();
        });
    }
    row_verify.add_suffix(&btn_verify);
    group_fixes.add(&row_verify);

    let has_winetricks = which_binary("winetricks").is_some();
    let row_physx = adw::ActionRow::builder()
        .title("NVIDIA PhysX Runtime Kurulumu")
        .subtitle(if has_winetricks {
            "Oyun açılışta PhysX hatası verirse tek tıkla prefix içine PhysX kurar"
        } else {
            "Bu özellik için sisteminize 'winetricks' paketini kurun"
        })
        .build();
    let btn_physx = gtk::Button::with_label("PhysX Kur");
    btn_physx.set_valign(gtk::Align::Center);
    btn_physx.set_sensitive(has_winetricks);
    {
        let cfg = Rc::clone(config_rc);
        let toast_ov = toast_overlay.clone();
        btn_physx.connect_clicked(move |btn| {
            btn.set_sensitive(false);
            btn.set_label("Kuruluyor...");
            let pfx = cfg.borrow().prefix_path.clone();
            let (tx, rx) = async_channel::bounded(1);
            std::thread::spawn(move || {
                let res = Command::new("winetricks")
                    .env("WINEPREFIX", &pfx)
                    .args(["-q", "physx"])
                    .status();
                let _ = tx.send_blocking(res.map(|s| s.success()).unwrap_or(false));
            });
            let btn_clone = btn.clone();
            let toast_clone = toast_ov.clone();
            glib::spawn_future_local(async move {
                let ok = rx.recv().await.unwrap_or(false);
                btn_clone.set_sensitive(true);
                btn_clone.set_label("PhysX Kur");
                if ok {
                    toast_clone.add_toast(adw::Toast::new("NVIDIA PhysX başarıyla kuruldu!"));
                } else {
                    toast_clone.add_toast(adw::Toast::new("PhysX kurulumu tamamlanamadı"));
                }
            });
        });
    }
    row_physx.add_suffix(&btn_physx);
    group_fixes.add(&row_physx);
    page_maint.add(&group_fixes);

    let group_debug = adw::PreferencesGroup::builder()
        .title("Geliştirici / Hata Ayıklama")
        .build();

    let sw_debug_win = adw::SwitchRow::builder()
        .title("Arka Plan Windows Başlatıcısını Göster")
        .subtitle("Yalnızca sorun giderme için açın; normalde gizli çalışır ve %0 CPU tüketir")
        .active(config_rc.borrow().show_debug_window)
        .build();
    {
        let cfg = Rc::clone(config_rc);
        let notify_cb = Rc::clone(&on_config_changed);
        sw_debug_win.connect_active_notify(move |sw| {
            cfg.borrow_mut().show_debug_window = sw.is_active();
            let _ = cfg.borrow().save();
            notify_cb();
        });
    }
    group_debug.add(&sw_debug_win);
    page_maint.add(&group_debug);

    prefs.add(&page_maint);
    prefs.present(Some(window));
}

use crate::api::{
    check_github_launcher_update, check_install_status_fast, fetch_portal_content,
    fetch_remote_manifests, format_bytes_human, format_turkish_date, verify_and_patch,
    LauncherReleaseInfo, LocalInstallState, NewsItem, PatchProgress, PreparedSlide,
    RemoteManifests,
};
use crate::config::AppConfig;
use crate::prefix::{
    check_shims_installed, clear_session_in_user_reg, find_running_pids, install_ncrypt_shims,
    read_session_info, select_runner,
};
use crate::runner::{
    launch_headless_session, stop_running_game, LoginCredentials, RunnerEvent,
};
use crate::ui::dialogs::{
    open_uri_or_path, present_login_dialog, present_logs_dialog, present_news_dialog,
    present_preferences_dialog, present_updater_dialog,
};
use crate::ui::style::{apply_color_scheme, load_css};
use adw::prelude::*;
use gtk::{gdk, glib};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::Duration;

#[derive(Debug, Clone, PartialEq, Eq)]
enum PrimaryActionMode {
    Checking,
    Install,
    Update,
    Play,
    Busy,
    Running,
}

pub fn build_ui(app: &adw::Application, snapshot_target: Option<String>) {
    load_css();
    let initial_cfg = AppConfig::load();
    apply_color_scheme(&initial_cfg.color_scheme);

    let config_rc = Rc::new(RefCell::new(initial_cfg));
    let manifests_rc: Rc<RefCell<Option<RemoteManifests>>> = Rc::new(RefCell::new(None));
    let release_info_rc: Rc<RefCell<Option<LauncherReleaseInfo>>> = Rc::new(RefCell::new(None));
    let slides_rc: Rc<RefCell<Vec<PreparedSlide>>> = Rc::new(RefCell::new(Vec::new()));
    let slide_idx_rc = Rc::new(Cell::new(0usize));
    let session_logs_rc: Rc<RefCell<Vec<String>>> = Rc::new(RefCell::new(Vec::new()));
    let action_mode_rc = Rc::new(RefCell::new(PrimaryActionMode::Checking));
    let pulse_active_rc = Rc::new(Cell::new(false));

    let window = adw::ApplicationWindow::builder()
        .application(app)
        .title("Goley Revival")
        .default_width(1040)
        .default_height(670)
        .width_request(860)
        .height_request(580)
        .build();

    let toast_overlay = adw::ToastOverlay::new();
    let toolbar_view = adw::ToolbarView::new();

    // ── HeaderBar ──────────────────────────────────────────────────────────
    let header = adw::HeaderBar::new();
    let win_title = adw::WindowTitle::new("Goley Revival", "Linux Başlatıcısı · Bağlanılıyor...");
    header.set_title_widget(Some(&win_title));

    // Left quick links
    let left_links = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    let btn_register = gtk::Button::with_label("Üye Ol");
    btn_register.add_css_class("flat");
    let btn_support = gtk::Button::with_label("Destek");
    btn_support.add_css_class("flat");
    left_links.append(&btn_register);
    left_links.append(&btn_support);
    header.pack_start(&left_links);

    // Right controls: Account MenuButton, Refresh, Logs, Preferences
    let account_btn_content = adw::ButtonContent::builder()
        .icon_name("avatar-default-symbolic")
        .label("Giriş Yap")
        .build();
    let account_menu_btn = gtk::MenuButton::builder()
        .child(&account_btn_content)
        .tooltip_text("Goley Revival Hesabı")
        .build();

    // Account Popover
    let account_popover = gtk::Popover::new();
    let pop_box = gtk::Box::new(gtk::Orientation::Vertical, 8);
    pop_box.set_margin_top(10);
    pop_box.set_margin_bottom(10);
    pop_box.set_margin_start(12);
    pop_box.set_margin_end(12);

    let pop_status_lbl = gtk::Label::new(Some("Henüz oturum açılmadı"));
    pop_status_lbl.add_css_class("caption-heading");
    pop_status_lbl.set_xalign(0.0);

    let btn_switch_acc = gtk::Button::with_label("Giriş Yap / Hesap Değiştir...");
    btn_switch_acc.add_css_class("flat");

    let btn_logout_acc = gtk::Button::with_label("Oturumu Kapat");
    btn_logout_acc.add_css_class("flat");
    btn_logout_acc.add_css_class("error");

    pop_box.append(&pop_status_lbl);
    pop_box.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    pop_box.append(&btn_switch_acc);
    pop_box.append(&btn_logout_acc);
    account_popover.set_child(Some(&pop_box));
    account_menu_btn.set_popover(Some(&account_popover));

    let self_upd_content = adw::ButtonContent::builder()
        .icon_name("software-update-available-symbolic")
        .label("Güncelle")
        .build();
    let btn_self_update = gtk::Button::builder()
        .child(&self_upd_content)
        .tooltip_text("GitHub üzerinden başlatıcı güncellemelerini denetle ve indir")
        .build();
    btn_self_update.add_css_class("flat");

    let btn_refresh = gtk::Button::from_icon_name("view-refresh-symbolic");
    btn_refresh.set_tooltip_text(Some("Duyuruları ve Güncellemeleri Yenile"));

    let btn_logs = gtk::Button::from_icon_name("utilities-terminal-symbolic");
    btn_logs.set_tooltip_text(Some("Canlı Günlükler (Logs)"));

    let btn_prefs = gtk::Button::from_icon_name("emblem-system-symbolic");
    btn_prefs.set_tooltip_text(Some("Başlatıcı Ayarları"));

    header.pack_end(&btn_prefs);
    header.pack_end(&btn_logs);
    header.pack_end(&btn_refresh);
    header.pack_end(&btn_self_update);
    header.pack_end(&account_menu_btn);
    toolbar_view.add_top_bar(&header);

    // ── Main Content (2-Column AAGL / Faugus Layout) ───────────────────────
    let main_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 20);
    main_hbox.set_margin_top(18);
    main_hbox.set_margin_bottom(18);
    main_hbox.set_margin_start(22);
    main_hbox.set_margin_end(22);
    main_hbox.set_vexpand(true);

    // Left Column: Hero Banner + Info Strip
    let left_col = gtk::Box::new(gtk::Orientation::Vertical, 14);
    left_col.set_hexpand(true);
    left_col.set_vexpand(true);

    let hero_overlay = gtk::Overlay::new();
    hero_overlay.add_css_class("hero-card");
    hero_overlay.set_overflow(gtk::Overflow::Hidden);
    hero_overlay.set_vexpand(true);
    hero_overlay.set_height_request(330);

    let hero_picture = gtk::Picture::new();
    hero_picture.set_content_fit(gtk::ContentFit::Cover);
    hero_picture.set_can_shrink(true);
    hero_picture.set_hexpand(true);
    hero_picture.set_vexpand(true);
    hero_overlay.set_child(Some(&hero_picture));

    // Top badge + slide nav
    let hero_top_bar = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    hero_top_bar.set_valign(gtk::Align::Start);
    hero_top_bar.set_halign(gtk::Align::Fill);
    hero_top_bar.set_margin_top(16);
    hero_top_bar.set_margin_start(18);
    hero_top_bar.set_margin_end(18);

    let hero_badge = gtk::Label::new(Some("ETKİNLİK & GÜNDEM"));
    hero_badge.add_css_class("hero-badge");
    hero_top_bar.append(&hero_badge);

    let top_spacer = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    top_spacer.set_hexpand(true);
    hero_top_bar.append(&top_spacer);

    let slide_nav_box = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    slide_nav_box.set_visible(false);
    let btn_prev_slide = gtk::Button::from_icon_name("go-previous-symbolic");
    btn_prev_slide.add_css_class("osd");
    btn_prev_slide.add_css_class("circular");
    let btn_next_slide = gtk::Button::from_icon_name("go-next-symbolic");
    btn_next_slide.add_css_class("osd");
    btn_next_slide.add_css_class("circular");
    slide_nav_box.append(&btn_prev_slide);
    slide_nav_box.append(&btn_next_slide);
    hero_top_bar.append(&slide_nav_box);
    hero_overlay.add_overlay(&hero_top_bar);

    // Bottom Scrim
    let hero_scrim = gtk::Box::new(gtk::Orientation::Vertical, 6);
    hero_scrim.add_css_class("hero-scrim");
    hero_scrim.set_valign(gtk::Align::End);
    hero_scrim.set_halign(gtk::Align::Fill);

    let hero_title = gtk::Label::new(Some("Goley Revival Linux"));
    hero_title.add_css_class("hero-title");
    hero_title.set_xalign(0.0);
    hero_title.set_wrap(true);

    let hero_desc = gtk::Label::new(Some("Resmi sunucudan etkinlik ve duyurular yükleniyor..."));
    hero_desc.add_css_class("hero-subtitle");
    hero_desc.set_xalign(0.0);
    hero_desc.set_wrap(true);

    hero_scrim.append(&hero_title);
    hero_scrim.append(&hero_desc);
    hero_overlay.add_overlay(&hero_scrim);
    left_col.append(&hero_overlay);

    // Info Strip (3 native Libadwaita `.card` status tiles)
    let info_strip = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    info_strip.set_homogeneous(true);

    let (tile_runner, lbl_runner_val) = create_info_tile(
        "UYUMLULUK KATMANI",
        "Algılanıyor...",
        "applications-games-symbolic",
    );
    let (tile_shim, lbl_shim_val) = create_info_tile(
        "CNG ECDSA KÖPRÜSÜ",
        "Hazır (x64 + x86)",
        "security-high-symbolic",
    );
    let (tile_mode, lbl_mode_val) = create_info_tile(
        "ÇALIŞMA MODU",
        "Doğal Arayüz (%0 Arka Plan CPU)",
        "speedometer-symbolic",
    );

    info_strip.append(&tile_runner);
    info_strip.append(&tile_shim);
    info_strip.append(&tile_mode);
    left_col.append(&info_strip);

    // Right Column: Official News & Patch Notes
    let right_col = gtk::Box::new(gtk::Orientation::Vertical, 10);
    right_col.set_width_request(345);
    right_col.set_vexpand(true);

    let news_hdr_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    let news_hdr_lbl = gtk::Label::new(Some("Duyurular & Yama Notları"));
    news_hdr_lbl.add_css_class("title-4");
    news_hdr_lbl.set_xalign(0.0);
    news_hdr_lbl.set_hexpand(true);

    let news_count_lbl = gtk::Label::new(None);
    news_count_lbl.add_css_class("dim-label");
    news_count_lbl.add_css_class("caption");

    news_hdr_box.append(&news_hdr_lbl);
    news_hdr_box.append(&news_count_lbl);
    right_col.append(&news_hdr_box);

    let news_scroller = gtk::ScrolledWindow::builder()
        .hscrollbar_policy(gtk::PolicyType::Never)
        .vexpand(true)
        .build();

    let news_listbox = gtk::ListBox::new();
    news_listbox.set_selection_mode(gtk::SelectionMode::None);
    news_listbox.add_css_class("boxed-list");

    let placeholder_row = adw::ActionRow::builder()
        .title("Duyurular yükleniyor...")
        .subtitle("api.playrevival.co üzerinden alınıyor")
        .build();
    news_listbox.append(&placeholder_row);

    news_scroller.set_child(Some(&news_listbox));
    right_col.append(&news_scroller);

    main_hbox.append(&left_col);
    main_hbox.append(&right_col);
    toolbar_view.set_content(Some(&main_hbox));

    // ── Bottom Action Dock (An Anime Game Launcher / Faugus Style) ─────────
    let dock = gtk::Box::new(gtk::Orientation::Horizontal, 18);
    dock.add_css_class("dock-footer");

    let status_col = gtk::Box::new(gtk::Orientation::Vertical, 6);
    status_col.set_hexpand(true);
    status_col.set_valign(gtk::Align::Center);

    let status_top_row = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    let lbl_status_head = gtk::Label::new(Some("Durum denetleniyor..."));
    lbl_status_head.add_css_class("status-headline");
    lbl_status_head.set_xalign(0.0);
    lbl_status_head.set_hexpand(true);

    let lbl_status_pct = gtk::Label::new(Some("%100"));
    lbl_status_pct.add_css_class("status-percent");
    lbl_status_pct.set_visible(false);

    status_top_row.append(&lbl_status_head);
    status_top_row.append(&lbl_status_pct);

    let progress_bar = gtk::ProgressBar::new();
    progress_bar.set_fraction(1.0);

    let lbl_status_detail = gtk::Label::new(Some("Sunucu manifestosu kontrol ediliyor..."));
    lbl_status_detail.add_css_class("status-detail");
    lbl_status_detail.set_xalign(0.0);
    lbl_status_detail.set_ellipsize(gtk::pango::EllipsizeMode::End);

    status_col.append(&status_top_row);
    status_col.append(&progress_bar);
    status_col.append(&lbl_status_detail);

    let actions_box = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    actions_box.set_valign(gtk::Align::Center);

    let btn_verify_quick = gtk::Button::from_icon_name("document-properties-symbolic");
    btn_verify_quick.add_css_class("circular");
    btn_verify_quick.set_valign(gtk::Align::Center);
    btn_verify_quick.set_tooltip_text(Some("Oyun Dosyalarını Doğrula / Onar (SHA-256)"));

    let primary_btn_content = adw::ButtonContent::builder()
        .icon_name("media-playback-start-symbolic")
        .label("Denetleniyor...")
        .build();
    let btn_primary = gtk::Button::builder()
        .child(&primary_btn_content)
        .sensitive(false)
        .build();
    btn_primary.add_css_class("suggested-action");
    btn_primary.add_css_class("pill");
    btn_primary.add_css_class("play-pill");

    actions_box.append(&btn_verify_quick);
    actions_box.append(&btn_primary);

    dock.append(&status_col);
    dock.append(&actions_box);
    toolbar_view.add_bottom_bar(&dock);

    toast_overlay.set_child(Some(&toolbar_view));
    window.set_content(Some(&toast_overlay));

    // Pulse timer for indeterminate states
    {
        let pb = progress_bar.clone();
        let active = Rc::clone(&pulse_active_rc);
        glib::timeout_add_local(Duration::from_millis(110), move || {
            if active.get() {
                pb.pulse();
            }
            glib::ControlFlow::Continue
        });
    }

    // Helper: Refresh local account & environment tiles
    let refresh_local_badges: Rc<dyn Fn()> = {
        let cfg = Rc::clone(&config_rc);
        let acc_content = account_btn_content.clone();
        let pop_lbl = pop_status_lbl.clone();
        let btn_out = btn_logout_acc.clone();
        let l_runner = lbl_runner_val.clone();
        let l_shim = lbl_shim_val.clone();
        let l_mode = lbl_mode_val.clone();

        Rc::new(move || {
            let c = cfg.borrow();
            let sess = read_session_info(&c.prefix_path);
            match (&sess.username, sess.has_session_token) {
                (Some(u), true) => {
                    acc_content.set_label(u);
                    pop_lbl.set_text(&format!("Aktif Oturum: {}", u));
                    btn_out.set_sensitive(true);
                }
                (Some(u), false) => {
                    acc_content.set_label(&format!("{} (Giriş)", u));
                    pop_lbl.set_text(&format!("Kayıtlı Kullanıcı: {} (Şifre gerekli)", u));
                    btn_out.set_sensitive(false);
                }
                (None, true) => {
                    acc_content.set_label("Oturum Açık");
                    pop_lbl.set_text("Kayıtlı oturum anahtarı mevcut");
                    btn_out.set_sensitive(true);
                }
                (None, false) => {
                    acc_content.set_label("Giriş Yap");
                    pop_lbl.set_text("Henüz oturum açılmadı");
                    btn_out.set_sensitive(false);
                }
            }

            if let Some(r) = select_runner(&c.runner_id) {
                l_runner.set_text(&r.label);
            } else {
                l_runner.set_text("Bulunamadı (Wine/UMU kurun)");
            }

            if check_shims_installed(&c.prefix_path) {
                l_shim.set_text("Aktif (ncrypt)");
            } else {
                l_shim.set_text("Otomatik Kurulacak");
            }

            if c.show_debug_window {
                l_mode.set_text("Hata Ayıklama (Pencere Açık)");
            } else {
                l_mode.set_text("Gizli Köprü (%0 Arka Plan CPU)");
            }
        })
    };
    refresh_local_badges();

    // Helper: Render current slide
    let render_slide: Rc<dyn Fn()> = {
        let slides = Rc::clone(&slides_rc);
        let idx_cell = Rc::clone(&slide_idx_rc);
        let pic = hero_picture.clone();
        let title_l = hero_title.clone();
        let desc_l = hero_desc.clone();
        let nav = slide_nav_box.clone();

        Rc::new(move || {
            let borrowed = slides.borrow();
            nav.set_visible(borrowed.len() > 1);
            if borrowed.is_empty() {
                return;
            }
            let idx = idx_cell.get() % borrowed.len();
            let s = &borrowed[idx];
            title_l.set_text(&s.item.title);
            desc_l.set_text(&s.item.description);
            if let Some(ref img) = s.image {
                let bytes = glib::Bytes::from(&img.rgba);
                let tex = gdk::MemoryTexture::new(
                    img.width,
                    img.height,
                    gdk::MemoryFormat::R8g8b8a8,
                    &bytes,
                    (img.width * 4) as usize,
                );
                pic.set_paintable(Some(&tex));
            }
        })
    };

    {
        let slides = Rc::clone(&slides_rc);
        let idx_cell = Rc::clone(&slide_idx_rc);
        let render = Rc::clone(&render_slide);
        btn_prev_slide.connect_clicked(move |_| {
            let len = slides.borrow().len();
            if len > 1 {
                let cur = idx_cell.get();
                idx_cell.set((cur + len - 1) % len);
                render();
            }
        });
    }
    {
        let slides = Rc::clone(&slides_rc);
        let idx_cell = Rc::clone(&slide_idx_rc);
        let render = Rc::clone(&render_slide);
        btn_next_slide.connect_clicked(move |_| {
            let len = slides.borrow().len();
            if len > 1 {
                idx_cell.set((idx_cell.get() + 1) % len);
                render();
            }
        });
    }

    // Helper: Apply UI button/footer state
    let set_ui_action_state: Rc<dyn Fn(PrimaryActionMode, &str, &str, Option<f64>)> = {
        let mode_rc = Rc::clone(&action_mode_rc);
        let pulse_rc = Rc::clone(&pulse_active_rc);
        let btn = btn_primary.clone();
        let btn_content = primary_btn_content.clone();
        let btn_ver = btn_verify_quick.clone();
        let head_l = lbl_status_head.clone();
        let pct_l = lbl_status_pct.clone();
        let det_l = lbl_status_detail.clone();
        let pb = progress_bar.clone();

        Rc::new(move |mode, headline, detail, fraction_opt| {
            *mode_rc.borrow_mut() = mode.clone();
            head_l.set_text(headline);
            det_l.set_text(detail);

            match fraction_opt {
                Some(frac) => {
                    pulse_rc.set(false);
                    let clamped = frac.clamp(0.0, 1.0);
                    pb.set_fraction(clamped);
                    pct_l.set_text(&format!("%{}", (clamped * 100.0).round() as u32));
                    pct_l.set_visible(true);
                }
                None => {
                    pulse_rc.set(true);
                    pct_l.set_visible(false);
                }
            }

            btn.remove_css_class("suggested-action");
            btn.remove_css_class("destructive-action");

            match mode {
                PrimaryActionMode::Checking => {
                    btn.add_css_class("suggested-action");
                    btn.set_sensitive(false);
                    btn_ver.set_sensitive(false);
                    btn_content.set_icon_name("emblem-synchronizing-symbolic");
                    btn_content.set_label("Denetleniyor...");
                }
                PrimaryActionMode::Install => {
                    btn.add_css_class("suggested-action");
                    btn.set_sensitive(true);
                    btn_ver.set_sensitive(false);
                    btn_content.set_icon_name("folder-download-symbolic");
                    btn_content.set_label("Oyunu İndir");
                }
                PrimaryActionMode::Update => {
                    btn.add_css_class("suggested-action");
                    btn.set_sensitive(true);
                    btn_ver.set_sensitive(true);
                    btn_content.set_icon_name("software-update-available-symbolic");
                    btn_content.set_label("Güncelle");
                }
                PrimaryActionMode::Play => {
                    btn.add_css_class("suggested-action");
                    btn.set_sensitive(true);
                    btn_ver.set_sensitive(true);
                    btn_content.set_icon_name("media-playback-start-symbolic");
                    btn_content.set_label("Oyunu Başlat");
                }
                PrimaryActionMode::Busy => {
                    btn.add_css_class("suggested-action");
                    btn.set_sensitive(false);
                    btn_ver.set_sensitive(false);
                    btn_content.set_icon_name("emblem-synchronizing-symbolic");
                    btn_content.set_label("Lütfen Bekleyin...");
                }
                PrimaryActionMode::Running => {
                    btn.add_css_class("destructive-action");
                    btn.set_sensitive(true);
                    btn_ver.set_sensitive(false);
                    pulse_rc.set(false);
                    pb.set_fraction(1.0);
                    pct_l.set_visible(false);
                    btn_content.set_icon_name("media-playback-stop-symbolic");
                    btn_content.set_label("Oyunu Kapat");
                }
            }
        })
    };

    // Helper: Evaluate local install state from manifests
    let evaluate_install_state: Rc<dyn Fn()> = {
        let cfg = Rc::clone(&config_rc);
        let man_rc = Rc::clone(&manifests_rc);
        let set_state = Rc::clone(&set_ui_action_state);
        let wtitle = win_title.clone();
        let refresh_badges = Rc::clone(&refresh_local_badges);

        Rc::new(move || {
            refresh_badges();

            // First check if BinaryTrU.exe is already running!
            let (game_pid, _) = find_running_pids();
            if let Some(pid) = game_pid {
                wtitle.set_subtitle("Oyun Çalışıyor · Sunucular Çevrimiçi");
                set_state(
                    PrimaryActionMode::Running,
                    &format!("Goley Revival Çalışıyor (PID {})", pid),
                    "Oturumunuz açık · Kapatmak için sağdaki 'Oyunu Kapat' butonunu kullanabilirsiniz",
                    Some(1.0),
                );
                return;
            }

            let Some(ref manifests) = *man_rc.borrow() else {
                return;
            };
            let pfx = cfg.borrow().prefix_path.clone();
            match check_install_status_fast(&pfx, manifests) {
                LocalInstallState::NotInstalled {
                    remote_version,
                    total_bytes,
                    total_files,
                } => {
                    wtitle.set_subtitle(&format!("Sürüm {} · Kurulum Hazır", remote_version));
                    set_state(
                        PrimaryActionMode::Install,
                        "Oyun Kurulu Değil",
                        &format!(
                            "Resmi CDN üzerinden doğrudan indirilebilir ({} dosya · {}) — Windows Setup gerekmez",
                            total_files,
                            format_bytes_human(total_bytes)
                        ),
                        Some(0.0),
                    );
                }
                LocalInstallState::UpdateRequired {
                    remote_version,
                    missing_files,
                    download_bytes,
                } => {
                    wtitle.set_subtitle(&format!("Sürüm {} · Güncelleme Mevcut", remote_version));
                    set_state(
                        PrimaryActionMode::Update,
                        &format!("Güncelleme Mevcut (v{})", remote_version),
                        &format!(
                            "{} dosya güncellenecek ({})",
                            missing_files,
                            format_bytes_human(download_bytes)
                        ),
                        Some(0.0),
                    );
                }
                LocalInstallState::UpToDate { version } => {
                    let _ = install_ncrypt_shims(&pfx);
                    refresh_badges();
                    wtitle.set_subtitle(&format!("v{} · Sunucular Çevrimiçi", version));
                    set_state(
                        PrimaryActionMode::Play,
                        "Oyun Güncel ve Hazır",
                        &format!(
                            "Oyun sürümü {} · Sunucular çevrimiçi · Doğal Linux modu aktif",
                            version
                        ),
                        Some(1.0),
                    );
                }
            }
        })
    };

    // Helper: Start Runner Session (with optional LoginCredentials)
    let start_runner_flow: Rc<dyn Fn(Option<LoginCredentials>)> = {
        let cfg = Rc::clone(&config_rc);
        let set_state = Rc::clone(&set_ui_action_state);
        let eval_state = Rc::clone(&evaluate_install_state);
        let refresh_badges = Rc::clone(&refresh_local_badges);
        let logs_rc = Rc::clone(&session_logs_rc);
        let toast_ov = toast_overlay.clone();
        let win_weak = window.downgrade();

        Rc::new(move |creds_opt| {
            set_state(
                PrimaryActionMode::Busy,
                "Goley Revival Başlatılıyor...",
                "Uyumluluk katmanı başlatılıyor...",
                None,
            );

            let (tx, rx) = async_channel::unbounded::<RunnerEvent>();
            let current_cfg = cfg.borrow().clone();
            launch_headless_session(current_cfg, creds_opt, tx);

            let set_state_inner = Rc::clone(&set_state);
            let eval_inner = Rc::clone(&eval_state);
            let badges_inner = Rc::clone(&refresh_badges);
            let logs_inner = Rc::clone(&logs_rc);
            let toast_inner = toast_ov.clone();
            let win_w = win_weak.clone();
            let cfg_for_login = Rc::clone(&cfg);
            let self_flow_weak_cfg = Rc::clone(&cfg);

            glib::spawn_future_local(async move {
                while let Ok(ev) = rx.recv().await {
                    match ev {
                        RunnerEvent::LogLine(line) => {
                            let mut buf = logs_inner.borrow_mut();
                            buf.push(line);
                            if buf.len() > 500 {
                                buf.remove(0);
                            }
                        }
                        RunnerEvent::Status {
                            headline,
                            detail,
                            pulsing,
                        } => {
                            set_state_inner(
                                PrimaryActionMode::Busy,
                                &headline,
                                &detail,
                                if pulsing { None } else { Some(1.0) },
                            );
                        }
                        RunnerEvent::LoginSucceeded { username } => {
                            badges_inner();
                            toast_inner.add_toast(adw::Toast::new(&format!(
                                "Hoş geldin, {}!",
                                username
                            )));
                        }
                        RunnerEvent::NeedLogin { reason } => {
                            eval_inner();
                            if let Some(ref r) = reason {
                                toast_inner.add_toast(adw::Toast::new(r));
                            }
                            if let Some(win) = win_w.upgrade() {
                                let saved_user =
                                    read_session_info(&cfg_for_login.borrow().prefix_path).username;
                                let set_s2 = Rc::clone(&set_state_inner);
                                let eval_s2 = Rc::clone(&eval_inner);
                                let badges_s2 = Rc::clone(&badges_inner);
                                let logs_s2 = Rc::clone(&logs_inner);
                                let toast_s2 = toast_inner.clone();
                                let cfg_s2 = Rc::clone(&self_flow_weak_cfg);
                                present_login_dialog(&win, saved_user, reason, move |new_creds| {
                                    set_s2(
                                        PrimaryActionMode::Busy,
                                        "Hesaba giriş yapılıyor...",
                                        "Kimlik bilgileri doğrulanıyor",
                                        None,
                                    );
                                    let (tx2, rx2) = async_channel::unbounded::<RunnerEvent>();
                                    launch_headless_session(
                                        cfg_s2.borrow().clone(),
                                        Some(new_creds),
                                        tx2,
                                    );
                                    let set_s3 = Rc::clone(&set_s2);
                                    let eval_s3 = Rc::clone(&eval_s2);
                                    let badges_s3 = Rc::clone(&badges_s2);
                                    let logs_s3 = Rc::clone(&logs_s2);
                                    let toast_s3 = toast_s2.clone();
                                    glib::spawn_future_local(async move {
                                        while let Ok(ev2) = rx2.recv().await {
                                            match ev2 {
                                                RunnerEvent::LogLine(l) => {
                                                    logs_s3.borrow_mut().push(l);
                                                }
                                                RunnerEvent::Status {
                                                    headline, detail, ..
                                                } => {
                                                    set_s3(
                                                        PrimaryActionMode::Busy,
                                                        &headline,
                                                        &detail,
                                                        None,
                                                    );
                                                }
                                                RunnerEvent::LoginSucceeded { username } => {
                                                    badges_s3();
                                                    toast_s3.add_toast(adw::Toast::new(&format!(
                                                        "Giriş başarılı: {}",
                                                        username
                                                    )));
                                                }
                                                RunnerEvent::NeedLogin { reason } => {
                                                    eval_s3();
                                                    toast_s3.add_toast(adw::Toast::new(
                                                        reason.as_deref().unwrap_or(
                                                            "Giriş başarısız, lütfen tekrar deneyin",
                                                        ),
                                                    ));
                                                    break;
                                                }
                                                RunnerEvent::LaunchError(err) => {
                                                    eval_s3();
                                                    toast_s3.add_toast(adw::Toast::new(&err));
                                                    break;
                                                }
                                                RunnerEvent::GameRunning { pid } => {
                                                    badges_s3();
                                                    set_s3(
                                                        PrimaryActionMode::Running,
                                                        &format!(
                                                            "Goley Revival Çalışıyor (PID {})",
                                                            pid
                                                        ),
                                                        "Oturumunuz açık · İyi oyunlar!",
                                                        Some(1.0),
                                                    );
                                                }
                                                RunnerEvent::GameExited => {
                                                    eval_s3();
                                                    break;
                                                }
                                            }
                                        }
                                    });
                                });
                            }
                            break;
                        }
                        RunnerEvent::LaunchError(err) => {
                            eval_inner();
                            toast_inner.add_toast(adw::Toast::new(&err));
                            break;
                        }
                        RunnerEvent::GameRunning { pid } => {
                            badges_inner();
                            set_state_inner(
                                PrimaryActionMode::Running,
                                &format!("Goley Revival Çalışıyor (PID {})", pid),
                                "Oturumunuz açık · Arka plan başlatıcısı uyku modunda (%0 CPU)",
                                Some(1.0),
                            );
                        }
                        RunnerEvent::GameExited => {
                            eval_inner();
                            break;
                        }
                    }
                }
            });
        })
    };

    // Helper: Run Patch / Install / SHA-256 Verification
    let run_patch_job: Rc<dyn Fn(bool)> = {
        let cfg = Rc::clone(&config_rc);
        let man_rc = Rc::clone(&manifests_rc);
        let set_state = Rc::clone(&set_ui_action_state);
        let eval_state = Rc::clone(&evaluate_install_state);
        let toast_ov = toast_overlay.clone();

        Rc::new(move |full_sha256| {
            let Some(manifests) = man_rc.borrow().clone() else {
                toast_ov.add_toast(adw::Toast::new(
                    "Sunucu manifestosu henüz yüklenmedi, lütfen yenileyin.",
                ));
                return;
            };
            let prefix = cfg.borrow().prefix_path.clone();

            set_state(
                PrimaryActionMode::Busy,
                if full_sha256 {
                    "SHA-256 Dosya Doğrulaması Başlatıldı..."
                } else {
                    "Kurulum / Güncelleme Hazırlanıyor..."
                },
                "Yerel dosyalar kontrol ediliyor",
                Some(0.0),
            );

            enum JobMsg {
                Progress(PatchProgress),
                Done(Result<usize, String>),
            }

            let (tx, rx) = async_channel::unbounded::<JobMsg>();
            std::thread::spawn(move || {
                let tx_prog = tx.clone();
                let res = verify_and_patch(&prefix, &manifests, full_sha256, move |prog| {
                    let _ = tx_prog.send_blocking(JobMsg::Progress(prog));
                });
                let _ = tx.send_blocking(JobMsg::Done(res));
            });

            let set_s = Rc::clone(&set_state);
            let eval_s = Rc::clone(&eval_state);
            let toast_s = toast_ov.clone();

            glib::spawn_future_local(async move {
                while let Ok(msg) = rx.recv().await {
                    match msg {
                        JobMsg::Progress(PatchProgress::Verifying {
                            checked,
                            total,
                            current_file,
                        }) => {
                            let frac = checked as f64 / (total.max(1) as f64);
                            set_s(
                                PrimaryActionMode::Busy,
                                &format!("Dosyalar Doğrulanıyor ({} / {})", checked, total),
                                &current_file,
                                Some(frac),
                            );
                        }
                        JobMsg::Progress(PatchProgress::Downloading {
                            downloaded_bytes,
                            total_bytes,
                            speed_mbps,
                            eta_secs,
                            files_done,
                            files_total,
                        }) => {
                            let frac = if total_bytes > 0 {
                                downloaded_bytes as f64 / total_bytes as f64
                            } else {
                                0.0
                            };
                            let mm = eta_secs / 60;
                            let ss = eta_secs % 60;
                            set_s(
                                PrimaryActionMode::Busy,
                                &format!(
                                    "Yama İndiriliyor ({} / {} dosya)",
                                    files_done, files_total
                                ),
                                &format!(
                                    "{:.1} MB/s · {} / {} · kalan {:02}:{:02}",
                                    speed_mbps,
                                    format_bytes_human(downloaded_bytes),
                                    format_bytes_human(total_bytes),
                                    mm,
                                    ss
                                ),
                                Some(frac),
                            );
                        }
                        JobMsg::Done(Ok(updated_count)) => {
                            eval_s();
                            if updated_count == 0 {
                                toast_s.add_toast(adw::Toast::new(
                                    "Tüm oyun dosyaları doğrulandı ve eksiksiz!",
                                ));
                            } else {
                                toast_s.add_toast(adw::Toast::new(&format!(
                                    "{} dosya başarıyla indirildi ve doğrulandı!",
                                    updated_count
                                )));
                            }
                            break;
                        }
                        JobMsg::Done(Err(err)) => {
                            eval_s();
                            toast_s.add_toast(adw::Toast::new(&err));
                            break;
                        }
                    }
                }
            });
        })
    };

    // Helper: Fetch Portal (Slides + News) & Manifests asynchronously
    let fetch_all_remote: Rc<dyn Fn()> = {
        let slides_store = Rc::clone(&slides_rc);
        let slide_idx = Rc::clone(&slide_idx_rc);
        let render_s = Rc::clone(&render_slide);
        let man_store = Rc::clone(&manifests_rc);
        let eval_s = Rc::clone(&evaluate_install_state);
        let set_s = Rc::clone(&set_ui_action_state);
        let listbox = news_listbox.clone();
        let count_lbl = news_count_lbl.clone();
        let h_desc = hero_desc.clone();
        let win_weak = window.downgrade();
        let toast_ov = toast_overlay.clone();
        let rel_store = Rc::clone(&release_info_rc);
        let btn_upd_c = btn_self_update.clone();
        let upd_content_c = self_upd_content.clone();

        Rc::new(move || {
            let (tx_portal, rx_portal) =
                async_channel::bounded::<Result<(Vec<PreparedSlide>, Vec<NewsItem>), String>>(1);
            std::thread::spawn(move || {
                let _ = tx_portal.send_blocking(fetch_portal_content());
            });

            let (tx_man, rx_man) =
                async_channel::bounded::<Result<RemoteManifests, String>>(1);
            std::thread::spawn(move || {
                let _ = tx_man.send_blocking(fetch_remote_manifests());
            });

            let (tx_rel, rx_rel) =
                async_channel::bounded::<Result<LauncherReleaseInfo, String>>(1);
            std::thread::spawn(move || {
                let _ = tx_rel.send_blocking(check_github_launcher_update());
            });

            let rel_c = Rc::clone(&rel_store);
            let btn_u = btn_upd_c.clone();
            let content_u = upd_content_c.clone();
            glib::spawn_future_local(async move {
                if let Ok(Ok(info)) = rx_rel.recv().await {
                    if info.update_available {
                        content_u.set_label(&format!("Güncelle (v{})", info.latest_version));
                        btn_u.remove_css_class("flat");
                        btn_u.add_css_class("suggested-action");
                        btn_u.set_tooltip_text(Some(&format!(
                            "Yeni sürüm mevcut: v{} (Mevcut: v{})",
                            info.latest_version, info.current_version
                        )));
                    } else {
                        content_u.set_label("Güncelle");
                        btn_u.remove_css_class("suggested-action");
                        btn_u.add_css_class("flat");
                        btn_u.set_tooltip_text(Some(&format!(
                            "Başlatıcı güncel (v{}) — GitHub güncellemelerini denetle",
                            info.current_version
                        )));
                    }
                    *rel_c.borrow_mut() = Some(info);
                }
            });

            let slides_c = Rc::clone(&slides_store);
            let idx_c = Rc::clone(&slide_idx);
            let render_c = Rc::clone(&render_s);
            let lb_c = listbox.clone();
            let cnt_c = count_lbl.clone();
            let hd_c = h_desc.clone();
            let win_w = win_weak.clone();

            glib::spawn_future_local(async move {
                if let Ok(res) = rx_portal.recv().await {
                    match res {
                        Ok((slides, news)) => {
                            *slides_c.borrow_mut() = slides;
                            idx_c.set(0);
                            render_c();

                            // Clear listbox & populate news
                            while let Some(child) = lb_c.first_child() {
                                lb_c.remove(&child);
                            }
                            cnt_c.set_text(&format!("{} duyuru", news.len()));

                            for (i, item) in news.into_iter().enumerate() {
                                let row = adw::ActionRow::builder()
                                    .title(&item.title)
                                    .subtitle(item.summary.as_deref().unwrap_or(""))
                                    .title_lines(2)
                                    .subtitle_lines(2)
                                    .activatable(true)
                                    .build();

                                let suffix_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
                                suffix_box.set_valign(gtk::Align::Center);

                                if i == 0 {
                                    let new_badge = gtk::Label::new(Some("YENİ"));
                                    new_badge.add_css_class("badge-new");
                                    suffix_box.append(&new_badge);
                                }

                                let date_l = gtk::Label::new(Some(&format_turkish_date(
                                    item.published_at.as_deref(),
                                )));
                                date_l.add_css_class("dim-label");
                                date_l.add_css_class("caption");
                                suffix_box.append(&date_l);

                                let arrow = gtk::Image::from_icon_name("go-next-symbolic");
                                arrow.add_css_class("dim-label");
                                suffix_box.append(&arrow);

                                row.add_suffix(&suffix_box);

                                let item_clone = item.clone();
                                let win_for_row = win_w.clone();
                                row.connect_activated(move |_| {
                                    if let Some(w) = win_for_row.upgrade() {
                                        present_news_dialog(&w, &item_clone);
                                    }
                                });
                                lb_c.append(&row);
                            }
                        }
                        Err(err) => {
                            hd_c.set_text(&err);
                        }
                    }
                }
            });

            let man_c = Rc::clone(&man_store);
            let eval_c = Rc::clone(&eval_s);
            let set_c = Rc::clone(&set_s);
            let toast_c = toast_ov.clone();

            glib::spawn_future_local(async move {
                if let Ok(res) = rx_man.recv().await {
                    match res {
                        Ok(manifests) => {
                            *man_c.borrow_mut() = Some(manifests);
                            eval_c();
                        }
                        Err(err) => {
                            toast_c.add_toast(adw::Toast::new(&err));
                            set_c(
                                PrimaryActionMode::Play,
                                "Çevrimdışı / Sunucuya Ulaşılamadı",
                                &err,
                                Some(0.0),
                            );
                        }
                    }
                }
            });
        })
    };

    // If game is already running on startup, attach background exit watcher immediately!
    if let (Some(pid), _) = find_running_pids() {
        set_ui_action_state(
            PrimaryActionMode::Running,
            &format!("Goley Revival Çalışıyor (PID {})", pid),
            "Oturumunuz açık · Kapatmak için sağdaki 'Oyunu Kapat' butonunu kullanabilirsiniz",
            Some(1.0),
        );
        let pfx = config_rc.borrow().prefix_path.clone();
        let eval_after = Rc::clone(&evaluate_install_state);
        let (tx_exit, rx_exit) = async_channel::bounded::<()>(1);
        std::thread::spawn(move || {
            loop {
                std::thread::sleep(Duration::from_millis(1500));
                if find_running_pids().0.is_none() {
                    let _ = crate::runner::send_bridge_command(&pfx, "QUIT\n");
                    let _ = tx_exit.send_blocking(());
                    break;
                }
            }
        });
        glib::spawn_future_local(async move {
            let _ = rx_exit.recv().await;
            eval_after();
        });
    }

    // Kick off initial portal & manifest load
    fetch_all_remote();

    // ── Wire Up Interactive Buttons ────────────────────────────────────────

    // Primary Action Button
    {
        let mode_rc = Rc::clone(&action_mode_rc);
        let cfg = Rc::clone(&config_rc);
        let patch_fn = Rc::clone(&run_patch_job);
        let start_fn = Rc::clone(&start_runner_flow);
        let eval_fn = Rc::clone(&evaluate_install_state);
        let win_weak = window.downgrade();

        btn_primary.connect_clicked(move |_| {
            let mode = mode_rc.borrow().clone();
            match mode {
                PrimaryActionMode::Install | PrimaryActionMode::Update => {
                    patch_fn(false);
                }
                PrimaryActionMode::Play => {
                    let sess = read_session_info(&cfg.borrow().prefix_path);
                    if sess.has_session_token {
                        start_fn(None);
                    } else if let Some(win) = win_weak.upgrade() {
                        let start_cb = Rc::clone(&start_fn);
                        present_login_dialog(&win, sess.username, None, move |creds| {
                            start_cb(Some(creds));
                        });
                    }
                }
                PrimaryActionMode::Running => {
                    stop_running_game(&cfg.borrow().prefix_path);
                    eval_fn();
                }
                PrimaryActionMode::Checking | PrimaryActionMode::Busy => {}
            }
        });
    }

    // Quick Verify Button
    {
        let patch_fn = Rc::clone(&run_patch_job);
        btn_verify_quick.connect_clicked(move |_| {
            patch_fn(true);
        });
    }

    // Header links
    {
        let win_weak = window.downgrade();
        btn_register.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                open_uri_or_path(&w, "https://playrevival.co/register");
            }
        });
    }
    {
        let win_weak = window.downgrade();
        btn_support.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                open_uri_or_path(&w, "https://playrevival.co/account/support");
            }
        });
    }

    // Account Popover buttons
    {
        let pop = account_popover.clone();
        let win_weak = window.downgrade();
        let cfg = Rc::clone(&config_rc);
        let start_fn = Rc::clone(&start_runner_flow);
        btn_switch_acc.connect_clicked(move |_| {
            pop.popdown();
            if let Some(w) = win_weak.upgrade() {
                let sess = read_session_info(&cfg.borrow().prefix_path);
                let start_cb = Rc::clone(&start_fn);
                present_login_dialog(&w, sess.username, None, move |creds| {
                    start_cb(Some(creds));
                });
            }
        });
    }
    {
        let pop = account_popover.clone();
        let cfg = Rc::clone(&config_rc);
        let badges = Rc::clone(&refresh_local_badges);
        let toast_ov = toast_overlay.clone();
        btn_logout_acc.connect_clicked(move |_| {
            pop.popdown();
            if find_running_pids().0.is_some() {
                toast_ov.add_toast(adw::Toast::new(
                    "Oturumu kapatmadan önce açık olan oyunu kapatın.",
                ));
                return;
            }
            stop_running_game(&cfg.borrow().prefix_path);
            match clear_session_in_user_reg(&cfg.borrow().prefix_path) {
                Ok(_) => {
                    badges();
                    toast_ov.add_toast(adw::Toast::new("Hesap oturumu kapatıldı"));
                }
                Err(e) => {
                    toast_ov.add_toast(adw::Toast::new(&format!("Oturum silinemedi: {}", e)));
                }
            }
        });
    }

    // Self-Update button
    {
        let win_weak = window.downgrade();
        let rel_rc = Rc::clone(&release_info_rc);
        let toast_ov = toast_overlay.clone();
        btn_self_update.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                present_updater_dialog(&w, rel_rc.borrow().clone(), &toast_ov);
            }
        });
    }

    // Refresh button
    {
        let fetch_fn = Rc::clone(&fetch_all_remote);
        let toast_ov = toast_overlay.clone();
        btn_refresh.connect_clicked(move |_| {
            toast_ov.add_toast(adw::Toast::new("Sunucu verileri yenileniyor..."));
            fetch_fn();
        });
    }

    // Logs button
    {
        let win_weak = window.downgrade();
        let cfg = Rc::clone(&config_rc);
        let logs = Rc::clone(&session_logs_rc);
        let toast_ov = toast_overlay.clone();
        btn_logs.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                present_logs_dialog(&w, &cfg.borrow().prefix_path, &logs, &toast_ov);
            }
        });
    }

    // Preferences button
    {
        let win_weak = window.downgrade();
        let cfg = Rc::clone(&config_rc);
        let toast_ov = toast_overlay.clone();
        let eval_fn = Rc::clone(&evaluate_install_state);
        let badges_fn = Rc::clone(&refresh_local_badges);
        let verify_fn = Rc::clone(&run_patch_job);

        btn_prefs.connect_clicked(move |_| {
            if let Some(w) = win_weak.upgrade() {
                let eval_cb = Rc::clone(&eval_fn);
                let badges_cb = Rc::clone(&badges_fn);
                let verify_cb = Rc::clone(&verify_fn);
                present_preferences_dialog(
                    &w,
                    &cfg,
                    &toast_ov,
                    eval_cb,
                    move || badges_cb(),
                    move || verify_cb(true),
                );
            }
        });
    }

    window.present();

    if let Some(target_path) = snapshot_target {
        let win_weak = window.downgrade();
        let app_weak = app.downgrade();
        glib::timeout_add_local(Duration::from_millis(2500), move || {
            if let (Some(win), Some(app)) = (win_weak.upgrade(), app_weak.upgrade()) {
                let w = win.width() as f64;
                let h = win.height() as f64;
                let paintable = gtk::WidgetPaintable::new(Some(&win));
                let snapshot = gtk::Snapshot::new();
                paintable.snapshot(&snapshot, w, h);
                if let Some(node) = snapshot.to_node() {
                    if let Some(native) = win.native() {
                        if let Some(renderer) = native.renderer() {
                            let texture = renderer.render_texture(&node, None);
                            if let Err(e) = texture.save_to_png(&target_path) {
                                eprintln!("Failed to save snapshot: {e}");
                            } else {
                                println!("Successfully saved launcher snapshot to: {target_path}");
                            }
                        }
                    }
                }
                app.quit();
            }
            glib::ControlFlow::Break
        });
    }
}

fn create_info_tile(title: &str, initial_val: &str, icon_name: &str) -> (gtk::Box, gtk::Label) {
    let card = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    card.add_css_class("card");
    card.add_css_class("info-tile");

    let icon = gtk::Image::from_icon_name(icon_name);
    icon.set_pixel_size(20);
    icon.add_css_class("accent");
    icon.set_valign(gtk::Align::Center);

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 2);
    vbox.set_valign(gtk::Align::Center);
    vbox.set_hexpand(true);

    let lbl_t = gtk::Label::new(Some(title));
    lbl_t.add_css_class("info-tile-title");
    lbl_t.set_xalign(0.0);

    let lbl_v = gtk::Label::new(Some(initial_val));
    lbl_v.add_css_class("info-tile-value");
    lbl_v.set_xalign(0.0);
    lbl_v.set_ellipsize(gtk::pango::EllipsizeMode::End);

    vbox.append(&lbl_t);
    vbox.append(&lbl_v);

    card.append(&icon);
    card.append(&vbox);

    (card, lbl_v)
}

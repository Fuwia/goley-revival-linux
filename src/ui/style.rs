use adw::StyleManager;
use gtk::gdk;

pub const APP_CSS: &str = r#"
.hero-card {
    border-radius: 18px;
    background: linear-gradient(135deg, #162238 0%, #0f1726 55%, #102a24 100%);
    border: 1px solid alpha(currentColor, 0.10);
}

.hero-scrim {
    background: linear-gradient(
        to top,
        rgba(8, 11, 18, 0.94) 0%,
        rgba(8, 11, 18, 0.68) 42%,
        rgba(8, 11, 18, 0.12) 100%
    );
    padding: 22px;
}

.hero-badge {
    background-color: rgba(46, 194, 126, 0.22);
    color: #7ef5b8;
    border: 1px solid rgba(126, 245, 184, 0.35);
    border-radius: 999px;
    padding: 4px 12px;
    font-size: 11px;
    font-weight: 800;
    letter-spacing: 0.6px;
}

.hero-title {
    color: #ffffff;
    font-size: 22px;
    font-weight: 800;
}

.hero-subtitle {
    color: rgba(235, 240, 250, 0.86);
    font-size: 13px;
    font-weight: 500;
}

.info-tile {
    padding: 12px 14px;
    border-radius: 14px;
}

.info-tile-title {
    font-size: 11px;
    font-weight: 600;
    opacity: 0.62;
}

.info-tile-value {
    font-size: 13px;
    font-weight: 700;
}

.badge-new {
    background-color: alpha(@accent_bg_color, 0.20);
    color: @accent_color;
    border-radius: 999px;
    padding: 2px 8px;
    font-size: 10px;
    font-weight: 800;
}

.dock-footer {
    background-color: alpha(@headerbar_bg_color, 0.85);
    border-top: 1px solid alpha(currentColor, 0.09);
    padding: 14px 22px;
}

.play-pill {
    min-width: 196px;
    min-height: 48px;
    font-size: 15px;
    font-weight: 800;
}

.status-headline {
    font-size: 13px;
    font-weight: 700;
}

.status-percent {
    font-size: 13px;
    font-weight: 800;
    color: @accent_color;
}

.status-detail {
    font-size: 11.5px;
    opacity: 0.68;
}

.log-view {
    font-family: monospace;
    font-size: 12px;
}
"#;

pub fn load_css() {
    let provider = gtk::CssProvider::new();
    provider.load_from_string(APP_CSS);
    if let Some(display) = gdk::Display::default() {
        gtk::style_context_add_provider_for_display(
            &display,
            &provider,
            gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
        );
    }
}

pub fn apply_color_scheme(scheme: &str) {
    let manager = StyleManager::default();
    match scheme {
        "light" => manager.set_color_scheme(adw::ColorScheme::ForceLight),
        "system" => manager.set_color_scheme(adw::ColorScheme::Default),
        _ => manager.set_color_scheme(adw::ColorScheme::PreferDark),
    }
}

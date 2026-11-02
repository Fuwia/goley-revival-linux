mod api;
mod config;
mod prefix;
mod runner;
mod ui;

use adw::prelude::*;

const APP_ID: &str = "io.github.fuwia.GoleyRevivalLinux";

fn main() -> gtk::glib::ExitCode {
    let app = adw::Application::builder()
        .application_id(APP_ID)
        .build();

    app.connect_activate(|app| {
        ui::window::build_ui(app);
    });

    app.run()
}

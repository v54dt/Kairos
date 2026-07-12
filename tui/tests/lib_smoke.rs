//! Proves the panels/app/input tree is reachable through the library facade
//! (RF4a), not just from inside the binary: build a Config with the library arg
//! parser, capture a Snapshot-shaped value, and render a panel off-screen.

use kairos_tui::app::{self, Cli, Fetch, Snapshot, Tab};
use kairos_tui::input;
use kairos_tui::panels;
use kairos_tui::sources::feed::FeedState;
use kairos_tui::sources::halt::HaltPrompt;
use kairos_tui::sources::order_journal::ScenariosView;
use kairos_tui::sources::scenario_ctl::ScenarioUi;
use kairos_tui::sources::service::ServiceUi;
use kairos_tui::sources::supervisor::SupervisorState;
use ratatui::Terminal;
use ratatui::backend::TestBackend;

fn empty_snapshot() -> Snapshot {
    Snapshot {
        systemd: Fetch::Loading,
        journal: Fetch::Loading,
        recorder: Fetch::Loading,
        disk_free: None,
        feed: FeedState::default(),
        scenarios: ScenariosView::default(),
        fills: Vec::new(),
        fills_date: String::new(),
        supervisor: SupervisorState::default(),
        timers: Fetch::Loading,
        blacklist: Fetch::Loading,
        archive: Fetch::Loading,
        ship_verify: Fetch::Loading,
        events: Fetch::Loading,
    }
}

#[test]
fn renders_a_panel_through_the_library_path() {
    let cfg = match app::parse_cli(Vec::<String>::new()) {
        Cli::Run(c) => c,
        _ => panic!("expected Run"),
    };
    let snap = empty_snapshot();
    let halt = input::halt_ui(&None, &HaltPrompt::Idle, &None);
    let service = ServiceUi::default();
    let scenario = ScenarioUi::default();

    let mut term = Terminal::new(TestBackend::new(120, 40)).unwrap();
    term.draw(|f| {
        panels::render(f, &snap, &cfg, Tab::Overview, &halt, &service, &scenario, 0);
    })
    .unwrap();

    let buf = term.backend().buffer().clone();
    let mut text = String::new();
    for y in 0..buf.area.height {
        for x in 0..buf.area.width {
            text.push_str(buf[(x, y)].symbol());
        }
    }
    assert!(text.contains("systemd"), "overview panel did not render");
}

mod app;
mod format;
mod panels;
mod sources;
mod terminal;

use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use futures::StreamExt;

use app::{Cli, Config, Shared, Snapshot, Tab};
use sources::feed::{self, FeedState};
use sources::halt::{self, HaltAction, HaltKey, HaltPrompt, HaltUi};
use sources::scenario_ctl::{self, Focus, ScenarioAction, ScenarioPrompt, ScenarioUi};
use sources::service::{self, ConfirmPrompt, ServiceUi, Verb};

const TICK: Duration = Duration::from_millis(750);

#[tokio::main]
async fn main() -> Result<()> {
    let cfg = match app::parse_args() {
        Cli::Run(cfg) => cfg,
        Cli::Help => {
            println!("{}", app::USAGE);
            return Ok(());
        }
        Cli::Version => {
            println!("{}", app::version_line());
            return Ok(());
        }
    };

    if !std::io::stdout().is_terminal() {
        eprintln!("kairos-top: not a terminal; run in an interactive TTY (or use --help).");
        std::process::exit(1);
    }

    let socket = kairos_core::uds::path::quote_socket_path();

    let shared = Arc::new(Shared::default());
    let feed_state = Arc::new(Mutex::new(FeedState::default()));

    let blacklist_path = sources::blacklist::resolve_blacklist_path(
        cfg.blacklist_path.as_ref().and_then(|p| p.to_str()),
        std::env::var("KAIROS_BLACKLIST_CSV").ok().as_deref(),
    );
    let kqr_dir = cfg.data_dir.join("kqr");

    tokio::spawn(feed::run(socket, cfg.symbols.clone(), feed_state.clone()));
    tokio::spawn(app::refresh_systemd(shared.clone()));
    tokio::spawn(app::refresh_journal(shared.clone()));
    tokio::spawn(app::refresh_recorder(shared.clone(), cfg.data_dir.clone()));
    tokio::spawn(app::refresh_hub_status(shared.clone()));
    tokio::spawn(app::refresh_scenarios(
        shared.clone(),
        cfg.journal_dir.clone(),
    ));
    tokio::spawn(app::refresh_available(
        shared.clone(),
        cfg.scenario_dir.clone(),
    ));
    tokio::spawn(app::refresh_running(shared.clone()));
    tokio::spawn(app::refresh_timers(shared.clone()));
    tokio::spawn(app::refresh_blacklist(shared.clone(), blacklist_path));
    tokio::spawn(app::refresh_archive(shared.clone(), kqr_dir));
    tokio::spawn(app::refresh_events(shared.clone()));

    let halt_path = halt::hub_halt_path();

    let mut term = terminal::enter()?;
    let res = run(&mut term, &shared, &feed_state, &cfg, halt_path).await;
    terminal::restore()?;
    res
}

async fn run(
    term: &mut terminal::Tui,
    shared: &Shared,
    feed_state: &Mutex<FeedState>,
    cfg: &Config,
    halt_path: Option<PathBuf>,
) -> Result<()> {
    let mut events = EventStream::new();
    let mut tick = tokio::time::interval(TICK);
    let mut tab = Tab::Overview;
    let mut prompt = HaltPrompt::Idle;
    let mut halt_result: Option<String> = None;
    let mut sel: usize = 0;
    let mut confirm = ConfirmPrompt::Idle;
    let action_result: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let mut scen = ScenState::default();
    loop {
        tokio::select! {
            _ = tick.tick() => {
                let snap = Snapshot::capture(shared, feed_state);
                let ui = halt_ui(&halt_path, &prompt, &halt_result);
                let service = service_ui(sel, &confirm, &action_result);
                let scenario = scen.ui();
                term.draw(|frame| panels::render(frame, &snap, cfg, tab, &ui, &service, &scenario))?;
            }
            Some(Ok(event)) = events.next() => {
                let key = match &event {
                    Event::Key(k) if k.kind == KeyEventKind::Press => *k,
                    _ => continue,
                };
                let snap = Snapshot::capture(shared, feed_state);
                let mut redraw = false;
                if prompt.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = halt::handle_key(&prompt, hk);
                        prompt = next;
                        if let Some(a) = action {
                            halt_result = Some(apply_halt(a, halt_path.as_deref()));
                        }
                        redraw = true;
                    }
                } else if confirm.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = service::handle_key(&confirm, hk);
                        confirm = next;
                        if let Some(a) = action {
                            let slot = action_result.clone();
                            *slot.lock().unwrap() =
                                Some(format!("{} {} ...", a.verb.as_str(), a.unit));
                            tokio::spawn(async move {
                                let msg = service::run_action(a.verb, &a.unit).await;
                                *slot.lock().unwrap() = Some(msg);
                            });
                        }
                        redraw = true;
                    }
                } else if scen.confirm.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = scenario_ctl::handle_key(&scen.confirm, hk);
                        scen.confirm = next;
                        if let Some(a) = action {
                            *scen.result.lock().unwrap() = Some(apply_scenario_action(a));
                        }
                        redraw = true;
                    }
                } else if let Some(ok) = overview_key(tab, &key) {
                    apply_overview_key(ok, &mut sel, &mut confirm, &snap);
                    redraw = true;
                } else if let Some(sk) = scenarios_key(tab, &key) {
                    apply_scenarios_key(sk, &snap, &mut scen);
                    redraw = true;
                } else if let Some(rk) = risk_tab_key(tab, &key) {
                    if halt_path.is_some() {
                        prompt = match rk {
                            RiskKey::Halt => HaltPrompt::ConfirmHalt(String::new()),
                            RiskKey::Resume => HaltPrompt::ConfirmResume(String::new()),
                        };
                    } else {
                        halt_result = Some("kill switch unavailable (no runtime dir)".to_string());
                    }
                    redraw = true;
                } else if should_quit(&event) {
                    return Ok(());
                } else if let Some(next) = tab_for(&event, tab) {
                    tab = next;
                    redraw = true;
                }
                if redraw {
                    let ui = halt_ui(&halt_path, &prompt, &halt_result);
                    let service = service_ui(sel, &confirm, &action_result);
                    let scenario = scen.ui();
                    term.draw(|frame| {
                        panels::render(frame, &snap, cfg, tab, &ui, &service, &scenario)
                    })?;
                }
            }
        }
    }
}

fn service_ui(
    selected: usize,
    confirm: &ConfirmPrompt,
    action_result: &Arc<Mutex<Option<String>>>,
) -> ServiceUi {
    ServiceUi {
        selected,
        confirm: confirm.clone(),
        last_result: action_result.lock().unwrap().clone(),
    }
}

/// Scenario-tab interaction state: which sub-list is focused, the two cursors,
/// the active confirm, and the shared last-action result.
struct ScenState {
    focus: Focus,
    avail_sel: usize,
    run_sel: usize,
    confirm: ScenarioPrompt,
    result: Arc<Mutex<Option<String>>>,
}

impl Default for ScenState {
    fn default() -> Self {
        ScenState {
            focus: Focus::default(),
            avail_sel: 0,
            run_sel: 0,
            confirm: ScenarioPrompt::Idle,
            result: Arc::new(Mutex::new(None)),
        }
    }
}

impl ScenState {
    fn ui(&self) -> ScenarioUi {
        ScenarioUi {
            focus: self.focus,
            avail_sel: self.avail_sel,
            run_sel: self.run_sel,
            confirm: self.confirm.clone(),
            last_result: self.result.lock().unwrap().clone(),
        }
    }
}

/// Run a confirmed scenario action. START spawns a detached trader; STOP sends a
/// validated SIGINT. Both surface a human result line; neither panics.
fn apply_scenario_action(action: ScenarioAction) -> String {
    match action {
        ScenarioAction::Start { toml, launch } => {
            let bin = scenario_ctl::trader_bin();
            let argv = scenario_ctl::build_spawn_argv(&bin, &toml, launch);
            match scenario_ctl::spawn_detached(&argv) {
                Ok(m) | Err(m) => m,
            }
        }
        ScenarioAction::Stop { pid } => match scenario_ctl::stop_trader(pid) {
            Ok(m) | Err(m) => m,
        },
    }
}

#[derive(Debug, PartialEq, Eq)]
enum ScenariosKey {
    FocusAvailable,
    FocusRunning,
    Up,
    Down,
    Start,
    Stop,
}

// Ctrl+C carries CONTROL and must reach should_quit, not a Scenarios action;
// 'q' and digits are unbound here so they still quit / switch tabs.
fn scenarios_key(tab: Tab, key: &KeyEvent) -> Option<ScenariosKey> {
    if tab != Tab::Scenarios || key.modifiers.contains(KeyModifiers::CONTROL) {
        return None;
    }
    match key.code {
        KeyCode::Left | KeyCode::Char('h') => Some(ScenariosKey::FocusAvailable),
        KeyCode::Right | KeyCode::Char('l') => Some(ScenariosKey::FocusRunning),
        KeyCode::Up | KeyCode::Char('k') => Some(ScenariosKey::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(ScenariosKey::Down),
        KeyCode::Char('s') => Some(ScenariosKey::Start),
        KeyCode::Char('x') => Some(ScenariosKey::Stop),
        _ => None,
    }
}

fn bump(sel: usize, len: usize) -> usize {
    (sel + 1).min(len.saturating_sub(1))
}

fn apply_scenarios_key(sk: ScenariosKey, snap: &Snapshot, scen: &mut ScenState) {
    let avail = &snap.available.0;
    let running = &snap.running;
    match sk {
        ScenariosKey::FocusAvailable => scen.focus = Focus::Available,
        ScenariosKey::FocusRunning => scen.focus = Focus::Running,
        ScenariosKey::Up => match scen.focus {
            Focus::Available => scen.avail_sel = scen.avail_sel.saturating_sub(1),
            Focus::Running => scen.run_sel = scen.run_sel.saturating_sub(1),
        },
        ScenariosKey::Down => match scen.focus {
            Focus::Available => scen.avail_sel = bump(scen.avail_sel, avail.len()),
            Focus::Running => scen.run_sel = bump(scen.run_sel, running.len()),
        },
        // Start acts only on the Available list; a mis-key while focused on
        // Running is a no-op, so a start can never cross-fire onto a stop target.
        ScenariosKey::Start => {
            if scen.focus == Focus::Available && !avail.is_empty() {
                let sel = scen.avail_sel.min(avail.len() - 1);
                scen.confirm = scenario_ctl::begin_start(&avail[sel]);
            }
        }
        ScenariosKey::Stop => {
            if scen.focus == Focus::Running && !running.is_empty() {
                let sel = scen.run_sel.min(running.len() - 1);
                scen.confirm = scenario_ctl::begin_stop(&running[sel]);
            }
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
enum OverviewKey {
    Up,
    Down,
    Act(Verb),
    TargetStart,
    TargetStop,
}

// Ctrl+C carries CONTROL and must reach should_quit, not an Overview action.
fn overview_key(tab: Tab, key: &KeyEvent) -> Option<OverviewKey> {
    if tab != Tab::Overview || key.modifiers.contains(KeyModifiers::CONTROL) {
        return None;
    }
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(OverviewKey::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(OverviewKey::Down),
        KeyCode::Char('r') => Some(OverviewKey::Act(Verb::Restart)),
        KeyCode::Char('s') => Some(OverviewKey::Act(Verb::Start)),
        KeyCode::Char('x') => Some(OverviewKey::Act(Verb::Stop)),
        KeyCode::Char('f') => Some(OverviewKey::Act(Verb::ResetFailed)),
        KeyCode::Char('S') => Some(OverviewKey::TargetStart),
        KeyCode::Char('X') => Some(OverviewKey::TargetStop),
        _ => None,
    }
}

fn apply_overview_key(
    ok: OverviewKey,
    sel: &mut usize,
    confirm: &mut ConfirmPrompt,
    snap: &Snapshot,
) {
    let units = match &snap.systemd {
        app::Fetch::Ok(u) if !u.is_empty() => u,
        _ => {
            if let OverviewKey::TargetStart | OverviewKey::TargetStop = ok {
                *confirm = target_confirm(&ok);
            }
            return;
        }
    };
    let last = units.len() - 1;
    if *sel > last {
        *sel = last;
    }
    match ok {
        OverviewKey::Up => *sel = sel.saturating_sub(1),
        OverviewKey::Down => *sel = (*sel + 1).min(last),
        OverviewKey::Act(verb) => {
            *confirm = service::begin(verb, &units[*sel].unit);
        }
        OverviewKey::TargetStart | OverviewKey::TargetStop => {
            *confirm = target_confirm(&ok);
        }
    }
}

fn target_confirm(ok: &OverviewKey) -> ConfirmPrompt {
    match ok {
        OverviewKey::TargetStart => service::begin(Verb::Start, "kairos.target"),
        _ => service::begin(Verb::Stop, "kairos.target"),
    }
}

fn halt_ui(path: &Option<PathBuf>, prompt: &HaltPrompt, last_result: &Option<String>) -> HaltUi {
    HaltUi {
        path: path.clone(),
        prompt: prompt.clone(),
        last_result: last_result.clone(),
    }
}

fn to_halt_key(key: &KeyEvent) -> Option<HaltKey> {
    match key.code {
        KeyCode::Char(c) => Some(HaltKey::Char(c)),
        KeyCode::Enter => Some(HaltKey::Enter),
        KeyCode::Backspace => Some(HaltKey::Backspace),
        KeyCode::Esc => Some(HaltKey::Cancel),
        _ => None,
    }
}

fn apply_halt(action: HaltAction, path: Option<&Path>) -> String {
    let path = match path {
        Some(p) => p,
        None => return "kill switch unavailable (no runtime dir)".to_string(),
    };
    match action {
        HaltAction::Arm => match halt::arm_halt(path) {
            Ok(()) => "adminHalt armed".to_string(),
            Err(e) => format!("arm failed: {e}"),
        },
        HaltAction::Clear => match halt::clear_halt(path) {
            Ok(()) => "halt cleared".to_string(),
            Err(e) => format!("clear failed: {e}"),
        },
    }
}

#[derive(Debug, PartialEq, Eq)]
enum RiskKey {
    Halt,
    Resume,
}

// Ctrl+C carries CONTROL and must reach should_quit, not the kill-switch guard.
fn risk_tab_key(tab: Tab, key: &KeyEvent) -> Option<RiskKey> {
    if tab != Tab::Risk || key.modifiers.contains(KeyModifiers::CONTROL) {
        return None;
    }
    match key.code {
        KeyCode::Char('k') => Some(RiskKey::Halt),
        KeyCode::Char('c') => Some(RiskKey::Resume),
        _ => None,
    }
}

fn should_quit(event: &Event) -> bool {
    if let Event::Key(key) = event {
        if key.kind != KeyEventKind::Press {
            return false;
        }
        return matches!(key.code, KeyCode::Char('q') | KeyCode::Char('Q'))
            || (key.modifiers.contains(KeyModifiers::CONTROL)
                && matches!(key.code, KeyCode::Char('c')));
    }
    false
}

fn tab_for(event: &Event, current: Tab) -> Option<Tab> {
    if let Event::Key(key) = event {
        if key.kind != KeyEventKind::Press {
            return None;
        }
        return match key.code {
            KeyCode::Char(c @ ('1' | '2' | '3' | '4' | '5')) => Some(current.select(c)),
            KeyCode::Tab => Some(current.next()),
            _ => None,
        };
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossterm::event::KeyEventKind;

    fn press(code: KeyCode, mods: KeyModifiers) -> KeyEvent {
        let mut k = KeyEvent::new(code, mods);
        k.kind = KeyEventKind::Press;
        k
    }

    #[test]
    fn ctrl_c_on_risk_tab_is_not_a_kill_switch_key() {
        let ctrl_c = press(KeyCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(risk_tab_key(Tab::Risk, &ctrl_c), None);
        assert!(should_quit(&Event::Key(ctrl_c)));
    }

    #[test]
    fn bare_keys_on_risk_tab_drive_the_kill_switch() {
        let c = press(KeyCode::Char('c'), KeyModifiers::NONE);
        let k = press(KeyCode::Char('k'), KeyModifiers::NONE);
        assert_eq!(risk_tab_key(Tab::Risk, &c), Some(RiskKey::Resume));
        assert_eq!(risk_tab_key(Tab::Risk, &k), Some(RiskKey::Halt));
    }

    #[test]
    fn kill_switch_keys_ignored_off_risk_tab() {
        let c = press(KeyCode::Char('c'), KeyModifiers::NONE);
        assert_eq!(risk_tab_key(Tab::Overview, &c), None);
    }

    #[test]
    fn ctrl_c_on_overview_tab_is_not_an_action_key() {
        let ctrl_c = press(KeyCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(overview_key(Tab::Overview, &ctrl_c), None);
        assert!(should_quit(&Event::Key(ctrl_c)));
    }

    #[test]
    fn bare_keys_on_overview_tab_map_to_actions() {
        let cases = [
            (KeyCode::Up, OverviewKey::Up),
            (KeyCode::Char('k'), OverviewKey::Up),
            (KeyCode::Down, OverviewKey::Down),
            (KeyCode::Char('j'), OverviewKey::Down),
            (KeyCode::Char('r'), OverviewKey::Act(Verb::Restart)),
            (KeyCode::Char('s'), OverviewKey::Act(Verb::Start)),
            (KeyCode::Char('x'), OverviewKey::Act(Verb::Stop)),
            (KeyCode::Char('f'), OverviewKey::Act(Verb::ResetFailed)),
            (KeyCode::Char('S'), OverviewKey::TargetStart),
            (KeyCode::Char('X'), OverviewKey::TargetStop),
        ];
        for (code, want) in cases {
            let k = press(code, KeyModifiers::NONE);
            assert_eq!(overview_key(Tab::Overview, &k), Some(want));
        }
    }

    #[test]
    fn overview_keys_ignored_off_overview_tab() {
        let r = press(KeyCode::Char('r'), KeyModifiers::NONE);
        assert_eq!(overview_key(Tab::Risk, &r), None);
        assert_eq!(overview_key(Tab::Data, &r), None);
        // 'q' is not an Overview action, so it still reaches should_quit.
        let q = press(KeyCode::Char('q'), KeyModifiers::NONE);
        assert_eq!(overview_key(Tab::Overview, &q), None);
        assert!(should_quit(&Event::Key(q)));
    }

    use sources::scenario_ctl::{Launch, RunningTrader, ScenarioToml};

    fn scen_snapshot(avail: Vec<ScenarioToml>, running: Vec<RunningTrader>) -> Snapshot {
        Snapshot {
            systemd: app::Fetch::Loading,
            journal: app::Fetch::Loading,
            recorder: app::Fetch::Loading,
            disk_free: None,
            feed: FeedState::default(),
            scenarios: Default::default(),
            available: (avail, 0),
            running,
            timers: app::Fetch::Loading,
            blacklist: app::Fetch::Loading,
            archive: app::Fetch::Loading,
            ship_verify: app::Fetch::Loading,
            events: app::Fetch::Loading,
        }
    }

    fn live_toml() -> ScenarioToml {
        ScenarioToml {
            path: PathBuf::from("/e/2330.toml"),
            name: "2330-plan".to_string(),
            symbol: "2330".to_string(),
            live: true,
        }
    }

    fn paper_toml() -> ScenarioToml {
        ScenarioToml {
            path: PathBuf::from("/e/0050.toml"),
            name: "0050-plan".to_string(),
            symbol: "0050".to_string(),
            live: false,
        }
    }

    #[test]
    fn ctrl_c_on_scenarios_tab_is_not_an_action_key() {
        let ctrl_c = press(KeyCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(scenarios_key(Tab::Scenarios, &ctrl_c), None);
        assert!(should_quit(&Event::Key(ctrl_c)));
    }

    #[test]
    fn q_and_digits_are_not_scenarios_actions() {
        let q = press(KeyCode::Char('q'), KeyModifiers::NONE);
        assert_eq!(scenarios_key(Tab::Scenarios, &q), None);
        assert!(should_quit(&Event::Key(q)));
        let three = press(KeyCode::Char('3'), KeyModifiers::NONE);
        assert_eq!(scenarios_key(Tab::Scenarios, &three), None);
    }

    #[test]
    fn scenarios_keys_ignored_off_scenarios_tab() {
        let s = press(KeyCode::Char('s'), KeyModifiers::NONE);
        assert_eq!(scenarios_key(Tab::Overview, &s), None);
    }

    #[test]
    fn bare_keys_on_scenarios_tab_map_to_actions() {
        let cases = [
            (KeyCode::Left, ScenariosKey::FocusAvailable),
            (KeyCode::Right, ScenariosKey::FocusRunning),
            (KeyCode::Up, ScenariosKey::Up),
            (KeyCode::Down, ScenariosKey::Down),
            (KeyCode::Char('s'), ScenariosKey::Start),
            (KeyCode::Char('x'), ScenariosKey::Stop),
        ];
        for (code, want) in cases {
            let k = press(code, KeyModifiers::NONE);
            assert_eq!(scenarios_key(Tab::Scenarios, &k), Some(want));
        }
    }

    #[test]
    fn start_on_available_opens_confirm_bound_to_selection() {
        let snap = scen_snapshot(vec![paper_toml(), live_toml()], vec![]);
        let mut scen = ScenState {
            focus: Focus::Available,
            avail_sel: 1, // the live toml
            ..Default::default()
        };
        apply_scenarios_key(ScenariosKey::Start, &snap, &mut scen);
        match &scen.confirm {
            ScenarioPrompt::TypedStart { toml, .. } => {
                assert_eq!(toml, &PathBuf::from("/e/2330.toml"));
            }
            other => panic!("expected TypedStart, got {other:?}"),
        }
    }

    #[test]
    fn start_while_focused_on_running_is_a_no_op() {
        let running = vec![RunningTrader {
            pid: 10,
            toml: "/e/2330.toml".to_string(),
            live: true,
        }];
        let snap = scen_snapshot(vec![live_toml()], running);
        let mut scen = ScenState {
            focus: Focus::Running,
            ..Default::default()
        };
        apply_scenarios_key(ScenariosKey::Start, &snap, &mut scen);
        assert_eq!(
            scen.confirm,
            ScenarioPrompt::Idle,
            "start must not cross-fire"
        );
    }

    #[test]
    fn stop_while_focused_on_available_is_a_no_op() {
        let running = vec![RunningTrader {
            pid: 10,
            toml: "/e/2330.toml".to_string(),
            live: true,
        }];
        let snap = scen_snapshot(vec![live_toml()], running);
        let mut scen = ScenState {
            focus: Focus::Available,
            ..Default::default()
        };
        apply_scenarios_key(ScenariosKey::Stop, &snap, &mut scen);
        assert_eq!(
            scen.confirm,
            ScenarioPrompt::Idle,
            "stop must not cross-fire"
        );
    }

    #[test]
    fn paper_start_action_argv_has_no_live() {
        // The action produced by confirming a paper start builds a paper argv.
        let argv = scenario_ctl::build_spawn_argv(
            "kairos_scenario_trader",
            &PathBuf::from("/e/0050.toml"),
            Launch::Paper,
        );
        assert!(!argv.iter().any(|a| a == "--live"));
    }
}

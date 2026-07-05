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
    loop {
        tokio::select! {
            _ = tick.tick() => {
                let snap = Snapshot::capture(shared, feed_state);
                let ui = halt_ui(&halt_path, &prompt, &halt_result);
                let service = service_ui(sel, &confirm, &action_result);
                term.draw(|frame| panels::render(frame, &snap, cfg, tab, &ui, &service))?;
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
                } else if let Some(ok) = overview_key(tab, &key) {
                    apply_overview_key(ok, &mut sel, &mut confirm, &snap);
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
                    term.draw(|frame| panels::render(frame, &snap, cfg, tab, &ui, &service))?;
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
}

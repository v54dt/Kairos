mod app;
mod format;
mod panels;
mod sources;
mod terminal;

use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use futures::StreamExt;

use app::{Cli, Config, Fetch, Shared, Snapshot, Tab};
use panels::DrillView;
use sources::feed::{self, FeedState};
use sources::halt::{self, HaltAction, HaltKey, HaltPrompt, HaltUi};
use sources::journald::{self, LogLine};
use sources::scenario_ctl::{self, ScenarioAction, ScenarioPrompt, ScenarioUi};
use sources::service::{self, ConfirmPrompt, ServiceUi, Verb};
use sources::supervisor;

const TICK: Duration = Duration::from_millis(750);
const DRILL_REFRESH: Duration = Duration::from_secs(2);

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
    tokio::spawn(app::refresh_supervisor(
        shared.clone(),
        cfg.supervisor_sock.clone(),
    ));
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
    let mut fills_sel: usize = 0;
    let mut confirm = ConfirmPrompt::Idle;
    let action_result: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let mut scen = ScenState::default();
    let mut drill: Option<JournalDrill> = None;
    loop {
        tokio::select! {
            _ = tick.tick() => {
                if let Some(d) = drill.as_mut()
                    && d.last_spawn.elapsed() >= DRILL_REFRESH
                {
                    d.spawn_tail();
                }
                let snap = Snapshot::capture(shared, feed_state);
                let ui = halt_ui(&halt_path, &prompt, &halt_result);
                let service = service_ui(sel, &confirm, &action_result);
                let scenario = scen.ui();
                let drill_view = drill.as_ref().map(|d| d.view());
                let mut clamped_rev: Option<usize> = None;
                term.draw(|frame| {
                    panels::render(frame, &snap, cfg, tab, &ui, &service, &scenario, fills_sel);
                    if tab == Tab::Overview && let Some(v) = &drill_view {
                        clamped_rev = Some(panels::render_drill_overlay(frame, v));
                    }
                })?;
                if let (Some(d), Some(r)) = (drill.as_mut(), clamped_rev) {
                    d.rev = r;
                }
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
                            spawn_scenario_action(a, cfg.supervisor_sock.clone(), scen.result.clone());
                        }
                        redraw = true;
                    }
                } else if drill.is_some() {
                    if should_quit(&event) {
                        return Ok(());
                    }
                    if let Some(dk) = journal_drill_key(&key) {
                        if apply_drill_key(dk, drill.as_mut().unwrap()) {
                            drill = None;
                        }
                        redraw = true;
                    }
                } else if let Some(ok) = overview_key(tab, &key) {
                    if ok == OverviewKey::OpenJournal {
                        if let Some(unit) =
                            open_target(&prompt, &confirm, &scen.confirm, &snap, sel)
                        {
                            let mut d = JournalDrill::new(unit);
                            d.spawn_tail();
                            drill = Some(d);
                        }
                    } else {
                        apply_overview_key(ok, &mut sel, &mut confirm, &snap);
                    }
                    redraw = true;
                } else if let Some(sk) = scenarios_key(tab, &key) {
                    apply_scenarios_key(sk, &snap, &mut scen);
                    redraw = true;
                } else if let Some(fk) = fills_key(tab, &key) {
                    apply_fills_key(fk, &snap, &mut fills_sel);
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
                    let drill_view = drill.as_ref().map(|d| d.view());
                    let mut clamped_rev: Option<usize> = None;
                    term.draw(|frame| {
                        panels::render(frame, &snap, cfg, tab, &ui, &service, &scenario, fills_sel);
                        if tab == Tab::Overview && let Some(v) = &drill_view {
                            clamped_rev = Some(panels::render_drill_overlay(frame, v));
                        }
                    })?;
                    if let (Some(d), Some(r)) = (drill.as_mut(), clamped_rev) {
                        d.rev = r;
                    }
                }
            }
        }
    }
}

/// Read-only journal drill-down over the Overview tab. Holds the target unit
/// name captured at open time (so a later selection change never retargets an
/// in-flight fetch), the scroll position as lines-from-bottom, and the shared
/// fetch slot written by a background journalctl task.
struct JournalDrill {
    unit: String,
    rev: usize,
    state: Arc<Mutex<Fetch<Vec<LogLine>>>>,
    inflight: Arc<AtomicBool>,
    last_spawn: Instant,
}

impl JournalDrill {
    fn new(unit: String) -> Self {
        JournalDrill {
            unit,
            rev: 0,
            state: Arc::new(Mutex::new(Fetch::Loading)),
            inflight: Arc::new(AtomicBool::new(false)),
            last_spawn: Instant::now(),
        }
    }

    /// Spawn a one-shot journalctl tail into the shared slot. Guarded by the
    /// inflight flag so slow calls never stack and never block the render loop.
    fn spawn_tail(&mut self) {
        if self.inflight.swap(true, Ordering::SeqCst) {
            return;
        }
        self.last_spawn = Instant::now();
        let unit = self.unit.clone();
        let state = self.state.clone();
        let inflight = self.inflight.clone();
        tokio::spawn(async move {
            let r = match journald::tail_unit(&unit, app::DRILLDOWN_TAIL).await {
                Ok(v) => Fetch::Ok(v),
                Err(e) => Fetch::Err(e.to_string()),
            };
            *state.lock().unwrap() = r;
            inflight.store(false, Ordering::SeqCst);
        });
    }

    fn view(&self) -> DrillView {
        DrillView {
            unit: self.unit.clone(),
            state: self.state.lock().unwrap().clone(),
            rev: self.rev,
        }
    }
}

/// Resolve the Overview-selected unit name, mirroring `apply_overview_key`'s
/// clamp: only a non-empty `Fetch::Ok` yields a target; anything else is None.
fn selected_unit(snap: &Snapshot, sel: usize) -> Option<String> {
    match &snap.systemd {
        Fetch::Ok(units) if !units.is_empty() => {
            let idx = sel.min(units.len() - 1);
            Some(units[idx].unit.clone())
        }
        _ => None,
    }
}

/// The unit a drill-down would open on, or None if any confirm/prompt is active
/// (so a drill and the K6 service-confirm can never both be opened) or no unit
/// is selectable.
fn open_target(
    prompt: &HaltPrompt,
    confirm: &ConfirmPrompt,
    scen_confirm: &ScenarioPrompt,
    snap: &Snapshot,
    sel: usize,
) -> Option<String> {
    if prompt.is_active() || confirm.is_active() || scen_confirm.is_active() {
        return None;
    }
    selected_unit(snap, sel)
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

/// Scenario-tab interaction state: a single cursor over the merged row list, the
/// active confirm, and the shared last-action result.
struct ScenState {
    sel: usize,
    confirm: ScenarioPrompt,
    result: Arc<Mutex<Option<String>>>,
}

impl Default for ScenState {
    fn default() -> Self {
        ScenState {
            sel: 0,
            confirm: ScenarioPrompt::Idle,
            result: Arc::new(Mutex::new(None)),
        }
    }
}

impl ScenState {
    fn ui(&self) -> ScenarioUi {
        ScenarioUi {
            sel: self.sel,
            confirm: self.confirm.clone(),
            last_result: self.result.lock().unwrap().clone(),
        }
    }
}

/// The human description + exact wire line for a confirmed scenario action. Only
/// a `Start { mode: Live }` (produced solely by the typed-name confirm) stamps the
/// "live" token.
fn scenario_request(action: &ScenarioAction) -> (String, String) {
    match action {
        ScenarioAction::Start { name, mode } => (
            format!("start {name} ({})", mode.token()),
            supervisor::start_request(name, *mode),
        ),
        ScenarioAction::Stop { name } => (format!("stop {name}"), supervisor::stop_request(name)),
    }
}

/// Send a confirmed scenario action to the supervisor over UDS off the UI thread,
/// writing the daemon's ok/err reply into the shared result slot. Never panics.
fn spawn_scenario_action(
    action: ScenarioAction,
    sock: Option<PathBuf>,
    slot: Arc<Mutex<Option<String>>>,
) {
    let (desc, line) = scenario_request(&action);
    *slot.lock().unwrap() = Some(format!("{desc} ..."));
    tokio::spawn(async move {
        let msg = match sock {
            Some(p) => format!("{desc}: {}", supervisor::send_command(&p, &line).await),
            None => format!("{desc}: supervisor not connected (no runtime dir)"),
        };
        *slot.lock().unwrap() = Some(msg);
    });
}

#[derive(Debug, PartialEq, Eq)]
enum ScenariosKey {
    Up,
    Down,
    PageUp,
    PageDown,
    StartPaper,
    StartLive,
    StartTest,
    Stop,
}

const SCEN_PAGE: usize = 10;

// Ctrl+C carries CONTROL and must reach should_quit, not a Scenarios action;
// 'q' and digits are unbound here so they still quit / switch tabs.
fn scenarios_key(tab: Tab, key: &KeyEvent) -> Option<ScenariosKey> {
    if tab != Tab::Scenarios || key.modifiers.contains(KeyModifiers::CONTROL) {
        return None;
    }
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(ScenariosKey::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(ScenariosKey::Down),
        KeyCode::PageUp => Some(ScenariosKey::PageUp),
        KeyCode::PageDown => Some(ScenariosKey::PageDown),
        KeyCode::Char('s') => Some(ScenariosKey::StartPaper),
        KeyCode::Char('l') => Some(ScenariosKey::StartLive),
        KeyCode::Char('t') => Some(ScenariosKey::StartTest),
        KeyCode::Char('x') => Some(ScenariosKey::Stop),
        _ => None,
    }
}

fn bump(sel: usize, len: usize) -> usize {
    (sel + 1).min(len.saturating_sub(1))
}

fn apply_scenarios_key(sk: ScenariosKey, snap: &Snapshot, scen: &mut ScenState) {
    let rows = &snap.supervisor.rows;
    let len = rows.len();
    // A start opens a confirm only on a NON-running row; a start key on a running
    // row is a no-op with a brief note, so a start can never cross-fire onto a
    // running target. Mode is explicit per key: 's' paper, 'l' live (typed
    // confirm), 't' test.
    let begin_start = |scen: &mut ScenState, open: fn(&str) -> ScenarioPrompt| {
        if let Some(row) = rows.get(scen.sel.min(len.saturating_sub(1))) {
            if row.state.is_running() {
                *scen.result.lock().unwrap() = Some("already running".to_string());
            } else {
                scen.confirm = open(&row.name);
            }
        }
    };
    match sk {
        ScenariosKey::Up => scen.sel = scen.sel.saturating_sub(1),
        ScenariosKey::Down => scen.sel = bump(scen.sel, len),
        ScenariosKey::PageUp => scen.sel = scen.sel.saturating_sub(SCEN_PAGE),
        ScenariosKey::PageDown => scen.sel = (scen.sel + SCEN_PAGE).min(len.saturating_sub(1)),
        ScenariosKey::StartPaper => begin_start(scen, scenario_ctl::begin_start_paper),
        ScenariosKey::StartLive => begin_start(scen, scenario_ctl::begin_start_live),
        ScenariosKey::StartTest => begin_start(scen, scenario_ctl::begin_start_test),
        // Stop acts only on a running row; 'x' on a stopped row is a no-op note.
        ScenariosKey::Stop => {
            if let Some(row) = rows.get(scen.sel.min(len.saturating_sub(1))) {
                if row.state.is_running() {
                    scen.confirm = scenario_ctl::begin_stop(&row.name, row.live);
                } else {
                    *scen.result.lock().unwrap() = Some("not running".to_string());
                }
            }
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
enum FillsKey {
    Up,
    Down,
    PageUp,
    PageDown,
}

const FILLS_PAGE: usize = 10;

// Ctrl+C carries CONTROL and must reach should_quit; 'q'/digits stay unbound so
// they still quit / switch tabs.
fn fills_key(tab: Tab, key: &KeyEvent) -> Option<FillsKey> {
    if tab != Tab::Fills || key.modifiers.contains(KeyModifiers::CONTROL) {
        return None;
    }
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => Some(FillsKey::Up),
        KeyCode::Down | KeyCode::Char('j') => Some(FillsKey::Down),
        KeyCode::PageUp => Some(FillsKey::PageUp),
        KeyCode::PageDown => Some(FillsKey::PageDown),
        _ => None,
    }
}

fn apply_fills_key(fk: FillsKey, snap: &Snapshot, sel: &mut usize) {
    let len = snap.fills.len();
    match fk {
        FillsKey::Up => *sel = sel.saturating_sub(1),
        FillsKey::Down => *sel = bump(*sel, len),
        FillsKey::PageUp => *sel = sel.saturating_sub(FILLS_PAGE),
        FillsKey::PageDown => *sel = (*sel + FILLS_PAGE).min(len.saturating_sub(1)),
    }
}

#[derive(Debug, PartialEq, Eq)]
enum OverviewKey {
    Up,
    Down,
    Act(Verb),
    TargetStart,
    TargetStop,
    OpenJournal,
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
        KeyCode::Enter => Some(OverviewKey::OpenJournal),
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
        OverviewKey::OpenJournal => {}
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

const DRILL_PAGE: usize = 10;

#[derive(Debug, PartialEq, Eq)]
enum DrillKey {
    Close,
    Up,
    Down,
    PageUp,
    PageDown,
}

// Ctrl+C carries CONTROL and 'q'/'Q' are the global quit keys; both must fall
// through to should_quit rather than being swallowed by the drill-down.
fn journal_drill_key(key: &KeyEvent) -> Option<DrillKey> {
    if key.modifiers.contains(KeyModifiers::CONTROL) {
        return None;
    }
    match key.code {
        KeyCode::Char('q') | KeyCode::Char('Q') => None,
        KeyCode::Esc => Some(DrillKey::Close),
        KeyCode::Up => Some(DrillKey::Up),
        KeyCode::Down => Some(DrillKey::Down),
        KeyCode::PageUp => Some(DrillKey::PageUp),
        KeyCode::PageDown => Some(DrillKey::PageDown),
        _ => None,
    }
}

// `rev` is lines-from-bottom; Up/PageUp walk toward older lines. Returns true
// when the view should close. All-saturating so scrolling never overflows.
fn apply_drill_key(dk: DrillKey, drill: &mut JournalDrill) -> bool {
    match dk {
        DrillKey::Close => return true,
        DrillKey::Up => drill.rev = drill.rev.saturating_add(1),
        DrillKey::Down => drill.rev = drill.rev.saturating_sub(1),
        DrillKey::PageUp => drill.rev = drill.rev.saturating_add(DRILL_PAGE),
        DrillKey::PageDown => drill.rev = drill.rev.saturating_sub(DRILL_PAGE),
    }
    false
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
            KeyCode::Char(c @ ('1' | '2' | '3' | '4' | '5' | '6')) => Some(current.select(c)),
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

    use sources::supervisor::{ScenarioState, SupervisorRow, SupervisorState};

    fn scen_snapshot(rows: Vec<SupervisorRow>) -> Snapshot {
        Snapshot {
            systemd: app::Fetch::Loading,
            journal: app::Fetch::Loading,
            recorder: app::Fetch::Loading,
            disk_free: None,
            feed: FeedState::default(),
            scenarios: Default::default(),
            fills: Vec::new(),
            fills_date: String::new(),
            supervisor: SupervisorState {
                connected: true,
                last_error: None,
                rows,
            },
            timers: app::Fetch::Loading,
            blacklist: app::Fetch::Loading,
            archive: app::Fetch::Loading,
            ship_verify: app::Fetch::Loading,
            events: app::Fetch::Loading,
        }
    }

    fn row(name: &str, state: ScenarioState, pid: i64, live: bool) -> SupervisorRow {
        SupervisorRow {
            name: name.to_string(),
            state,
            pid,
            cum_fills: 0,
            cum_shares: 0,
            last_fill_ts: 0,
            last_exit_reason: String::new(),
            live,
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
            (KeyCode::Up, ScenariosKey::Up),
            (KeyCode::Down, ScenariosKey::Down),
            (KeyCode::PageUp, ScenariosKey::PageUp),
            (KeyCode::PageDown, ScenariosKey::PageDown),
            (KeyCode::Char('s'), ScenariosKey::StartPaper),
            (KeyCode::Char('l'), ScenariosKey::StartLive),
            (KeyCode::Char('t'), ScenariosKey::StartTest),
            (KeyCode::Char('x'), ScenariosKey::Stop),
        ];
        for (code, want) in cases {
            let k = press(code, KeyModifiers::NONE);
            assert_eq!(scenarios_key(Tab::Scenarios, &k), Some(want));
        }
    }

    #[test]
    fn live_start_on_stopped_row_opens_typed_confirm_bound_to_selection() {
        let snap = scen_snapshot(vec![
            row("0050", ScenarioState::Stopped, 0, false),
            row("2330", ScenarioState::Stopped, 0, false),
        ]);
        let mut scen = ScenState {
            sel: 1,
            ..Default::default()
        };
        apply_scenarios_key(ScenariosKey::StartLive, &snap, &mut scen);
        match &scen.confirm {
            ScenarioPrompt::TypedStart { name, .. } => assert_eq!(name, "2330"),
            other => panic!("expected TypedStart, got {other:?}"),
        }
    }

    #[test]
    fn paper_start_on_stopped_row_opens_simple_confirm() {
        let snap = scen_snapshot(vec![row("0050", ScenarioState::Stopped, 0, false)]);
        let mut scen = ScenState::default();
        apply_scenarios_key(ScenariosKey::StartPaper, &snap, &mut scen);
        assert!(matches!(
            scen.confirm,
            ScenarioPrompt::SimpleStart {
                mode: supervisor::Mode::Paper,
                ..
            }
        ));
    }

    #[test]
    fn start_on_a_running_row_is_a_no_op() {
        let snap = scen_snapshot(vec![row("2330", ScenarioState::InWindow, 10, true)]);
        for sk in [
            ScenariosKey::StartPaper,
            ScenariosKey::StartLive,
            ScenariosKey::StartTest,
        ] {
            let mut scen = ScenState::default();
            apply_scenarios_key(sk, &snap, &mut scen);
            assert_eq!(
                scen.confirm,
                ScenarioPrompt::Idle,
                "start on a running row must not open a confirm"
            );
        }
    }

    #[test]
    fn stop_on_a_stopped_row_is_a_no_op() {
        let snap = scen_snapshot(vec![row("2330", ScenarioState::ClosedExited, 0, false)]);
        let mut scen = ScenState::default();
        apply_scenarios_key(ScenariosKey::Stop, &snap, &mut scen);
        assert_eq!(
            scen.confirm,
            ScenarioPrompt::Idle,
            "stop on a stopped/exited row must not open a confirm"
        );
    }

    #[test]
    fn stop_on_a_running_row_opens_confirm() {
        let snap = scen_snapshot(vec![row("2330", ScenarioState::InWindow, 4242, true)]);
        let mut scen = ScenState::default();
        apply_scenarios_key(ScenariosKey::Stop, &snap, &mut scen);
        assert!(
            matches!(
                &scen.confirm,
                ScenarioPrompt::SimpleStop { name, .. } if name == "2330"
            ),
            "stop on a running row must open a SimpleStop confirm"
        );
    }

    use sources::systemd::UnitStatus;

    fn unit(name: &str) -> UnitStatus {
        UnitStatus {
            unit: name.to_string(),
            load: "loaded".to_string(),
            active: "failed".to_string(),
            sub: "failed".to_string(),
            description: "desc".to_string(),
        }
    }

    fn systemd_snapshot(systemd: Fetch<Vec<UnitStatus>>) -> Snapshot {
        let mut snap = scen_snapshot(vec![]);
        snap.systemd = systemd;
        snap
    }

    #[test]
    fn selected_unit_targets_the_selected_name() {
        let units = vec![
            unit("kairos-core.service"),
            unit("kairos-orderhub.service"),
            unit("kairos-driver.service"),
        ];
        let snap = systemd_snapshot(Fetch::Ok(units));
        assert_eq!(
            selected_unit(&snap, 1),
            Some("kairos-orderhub.service".to_string())
        );
        // A cursor past the end clamps to the last unit, never panics.
        assert_eq!(
            selected_unit(&snap, 99),
            Some("kairos-driver.service".to_string())
        );
    }

    #[test]
    fn selected_unit_none_when_not_ready() {
        assert_eq!(selected_unit(&systemd_snapshot(Fetch::Loading), 0), None);
        assert_eq!(
            selected_unit(&systemd_snapshot(Fetch::Err("boom".to_string())), 0),
            None
        );
        assert_eq!(selected_unit(&systemd_snapshot(Fetch::Ok(vec![])), 0), None);
    }

    #[test]
    fn open_snapshots_owned_unit_name() {
        let units = vec![unit("kairos-core.service"), unit("kairos-orderhub.service")];
        let snap = systemd_snapshot(Fetch::Ok(units));
        let target = selected_unit(&snap, 1).unwrap();
        let d = JournalDrill::new(target);
        assert_eq!(d.unit, "kairos-orderhub.service");
        assert_eq!(d.rev, 0);
    }

    #[test]
    fn open_refused_while_a_confirm_is_active() {
        let units = vec![unit("kairos-orderhub.service")];
        let snap = systemd_snapshot(Fetch::Ok(units));
        let active = service::begin(Verb::Restart, "kairos-orderhub.service");
        assert!(active.is_active());
        assert_eq!(
            open_target(&HaltPrompt::Idle, &active, &ScenarioPrompt::Idle, &snap, 0),
            None
        );
        // With every prompt idle the selected unit is offered.
        assert_eq!(
            open_target(
                &HaltPrompt::Idle,
                &ConfirmPrompt::Idle,
                &ScenarioPrompt::Idle,
                &snap,
                0
            ),
            Some("kairos-orderhub.service".to_string())
        );
    }

    #[test]
    fn enter_opens_journal_only_on_overview() {
        let enter = press(KeyCode::Enter, KeyModifiers::NONE);
        assert_eq!(
            overview_key(Tab::Overview, &enter),
            Some(OverviewKey::OpenJournal)
        );
        assert_eq!(overview_key(Tab::Risk, &enter), None);
        let ctrl_enter = press(KeyCode::Enter, KeyModifiers::CONTROL);
        assert_eq!(overview_key(Tab::Overview, &ctrl_enter), None);
    }

    #[test]
    fn ctrl_c_and_q_from_drilldown_still_quit() {
        let ctrl_c = press(KeyCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(journal_drill_key(&ctrl_c), None);
        assert!(should_quit(&Event::Key(ctrl_c)));
        for c in ['q', 'Q'] {
            let q = press(KeyCode::Char(c), KeyModifiers::NONE);
            assert_eq!(journal_drill_key(&q), None);
            assert!(should_quit(&Event::Key(q)));
        }
    }

    #[test]
    fn esc_closes_the_drilldown() {
        let esc = press(KeyCode::Esc, KeyModifiers::NONE);
        assert_eq!(journal_drill_key(&esc), Some(DrillKey::Close));
        let mut d = JournalDrill::new("kairos-orderhub.service".to_string());
        assert!(apply_drill_key(DrillKey::Close, &mut d));
    }

    #[test]
    fn scroll_keys_map_and_adjust_rev_saturating() {
        let cases = [
            (KeyCode::Up, DrillKey::Up),
            (KeyCode::Down, DrillKey::Down),
            (KeyCode::PageUp, DrillKey::PageUp),
            (KeyCode::PageDown, DrillKey::PageDown),
        ];
        for (code, want) in cases {
            let k = press(code, KeyModifiers::NONE);
            assert_eq!(journal_drill_key(&k), Some(want));
        }
        let mut d = JournalDrill::new("u".to_string());
        assert!(!apply_drill_key(DrillKey::Down, &mut d));
        assert_eq!(d.rev, 0, "Down at bottom stays pinned");
        apply_drill_key(DrillKey::Up, &mut d);
        assert_eq!(d.rev, 1);
        apply_drill_key(DrillKey::PageUp, &mut d);
        assert_eq!(d.rev, 1 + DRILL_PAGE);
        apply_drill_key(DrillKey::PageDown, &mut d);
        assert_eq!(d.rev, 1);
    }

    #[test]
    fn overview_action_chars_are_not_drill_keys() {
        // While a drill is open these route to the drill branch; none is a drill
        // key, so no service action can fire behind the modal.
        for c in ['r', 's', 'x', 'f', 'S', 'X'] {
            let k = press(KeyCode::Char(c), KeyModifiers::NONE);
            assert_eq!(journal_drill_key(&k), None);
        }
    }

    #[test]
    fn bare_keys_on_fills_tab_map_to_nav() {
        let cases = [
            (KeyCode::Up, FillsKey::Up),
            (KeyCode::Char('k'), FillsKey::Up),
            (KeyCode::Down, FillsKey::Down),
            (KeyCode::Char('j'), FillsKey::Down),
            (KeyCode::PageUp, FillsKey::PageUp),
            (KeyCode::PageDown, FillsKey::PageDown),
        ];
        for (code, want) in cases {
            let k = press(code, KeyModifiers::NONE);
            assert_eq!(fills_key(Tab::Fills, &k), Some(want));
        }
    }

    #[test]
    fn ctrl_c_and_q_are_not_fills_keys() {
        let ctrl_c = press(KeyCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(fills_key(Tab::Fills, &ctrl_c), None);
        let q = press(KeyCode::Char('q'), KeyModifiers::NONE);
        assert_eq!(fills_key(Tab::Fills, &q), None);
        // Off the Fills tab nav keys are ignored.
        let up = press(KeyCode::Up, KeyModifiers::NONE);
        assert_eq!(fills_key(Tab::Overview, &up), None);
    }

    #[test]
    fn fills_nav_clamps_within_bounds() {
        use sources::order_journal::Fill;
        let mut snap = scen_snapshot(vec![]);
        snap.fills = (0..25)
            .map(|i| Fill {
                t: i,
                stem: "2330-Buy-20260705".to_string(),
                buy: true,
                shares: 1000,
                price: 58500,
            })
            .collect();
        let mut sel = 0usize;
        apply_fills_key(FillsKey::Up, &snap, &mut sel);
        assert_eq!(sel, 0, "up at top stays pinned");
        apply_fills_key(FillsKey::PageDown, &snap, &mut sel);
        assert_eq!(sel, 10);
        apply_fills_key(FillsKey::PageDown, &snap, &mut sel);
        apply_fills_key(FillsKey::PageDown, &snap, &mut sel);
        assert_eq!(sel, 24, "page down clamps to the last fill");
        apply_fills_key(FillsKey::Down, &snap, &mut sel);
        assert_eq!(sel, 24, "down at bottom stays pinned");
    }

    #[test]
    fn paper_start_action_request_has_no_live_token() {
        // The action produced by confirming a paper start builds a paper request.
        let (_, line) = scenario_request(&ScenarioAction::Start {
            name: "0050".to_string(),
            mode: supervisor::Mode::Paper,
        });
        assert!(
            !line.contains("live"),
            "paper request must not carry live: {line}"
        );
        assert!(line.contains("\"mode\":\"paper\""));
    }

    #[test]
    fn live_action_request_stamps_live_token() {
        let (_, line) = scenario_request(&ScenarioAction::Stop {
            name: "2330".to_string(),
        });
        assert!(line.contains("\"cmd\":\"stop\""));
        let (_, live) = scenario_request(&ScenarioAction::Start {
            name: "2330".to_string(),
            mode: supervisor::Mode::Live,
        });
        assert!(live.contains("\"mode\":\"live\""));
    }
}

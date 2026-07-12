use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use crossterm::event::{Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};

use crate::app::{self, Fetch, Snapshot, Tab};
use crate::panels::DrillView;
#[cfg(test)]
use crate::sources;
use crate::sources::halt::{self, HaltAction, HaltKey, HaltPrompt, HaltUi};
use crate::sources::journald::{self, LogLine};
use crate::sources::scenario_ctl::{self, ScenarioAction, ScenarioPrompt, ScenarioUi};
use crate::sources::service::{self, ConfirmPrompt, ServiceUi, Verb};
use crate::sources::supervisor;

/// Read-only journal drill-down over the Overview tab. Holds the target unit
/// name captured at open time (so a later selection change never retargets an
/// in-flight fetch), the scroll position as lines-from-bottom, and the shared
/// fetch slot written by a background journalctl task.
pub struct JournalDrill {
    unit: String,
    pub rev: usize,
    state: Arc<Mutex<Fetch<Vec<LogLine>>>>,
    inflight: Arc<AtomicBool>,
    pub last_spawn: Instant,
}

impl JournalDrill {
    pub fn new(unit: String) -> Self {
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
    pub fn spawn_tail(&mut self) {
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

    pub fn view(&self) -> DrillView {
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
pub fn open_target(
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

pub fn service_ui(
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
pub struct ScenState {
    pub sel: usize,
    pub confirm: ScenarioPrompt,
    pub result: Arc<Mutex<Option<String>>>,
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
    pub fn ui(&self) -> ScenarioUi {
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
pub fn spawn_scenario_action(
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
pub enum ScenariosKey {
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
pub fn scenarios_key(tab: Tab, key: &KeyEvent) -> Option<ScenariosKey> {
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

fn page_up(sel: usize, page: usize) -> usize {
    sel.saturating_sub(page)
}

fn page_down(sel: usize, page: usize, len: usize) -> usize {
    (sel + page).min(len.saturating_sub(1))
}

pub fn apply_scenarios_key(sk: ScenariosKey, snap: &Snapshot, scen: &mut ScenState) {
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
        ScenariosKey::PageUp => scen.sel = page_up(scen.sel, SCEN_PAGE),
        ScenariosKey::PageDown => scen.sel = page_down(scen.sel, SCEN_PAGE, len),
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
pub enum FillsKey {
    Up,
    Down,
    PageUp,
    PageDown,
}

const FILLS_PAGE: usize = 10;

// Ctrl+C carries CONTROL and must reach should_quit; 'q'/digits stay unbound so
// they still quit / switch tabs.
pub fn fills_key(tab: Tab, key: &KeyEvent) -> Option<FillsKey> {
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

pub fn apply_fills_key(fk: FillsKey, snap: &Snapshot, sel: &mut usize) {
    let len = snap.fills.len();
    match fk {
        FillsKey::Up => *sel = sel.saturating_sub(1),
        FillsKey::Down => *sel = bump(*sel, len),
        FillsKey::PageUp => *sel = page_up(*sel, FILLS_PAGE),
        FillsKey::PageDown => *sel = page_down(*sel, FILLS_PAGE, len),
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum OverviewKey {
    Up,
    Down,
    Act(Verb),
    TargetStart,
    TargetStop,
    OpenJournal,
}

// Ctrl+C carries CONTROL and must reach should_quit, not an Overview action.
pub fn overview_key(tab: Tab, key: &KeyEvent) -> Option<OverviewKey> {
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

pub fn apply_overview_key(
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

pub fn halt_ui(
    path: &Option<PathBuf>,
    prompt: &HaltPrompt,
    last_result: &Option<String>,
) -> HaltUi {
    HaltUi {
        path: path.clone(),
        prompt: prompt.clone(),
        last_result: last_result.clone(),
    }
}

pub fn to_halt_key(key: &KeyEvent) -> Option<HaltKey> {
    match key.code {
        KeyCode::Char(c) => Some(HaltKey::Char(c)),
        KeyCode::Enter => Some(HaltKey::Enter),
        KeyCode::Backspace => Some(HaltKey::Backspace),
        KeyCode::Esc => Some(HaltKey::Cancel),
        _ => None,
    }
}

pub fn apply_halt(action: HaltAction, path: Option<&Path>) -> String {
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
pub enum RiskKey {
    Halt,
    Resume,
}

// Ctrl+C carries CONTROL and must reach should_quit, not the kill-switch guard.
pub fn risk_tab_key(tab: Tab, key: &KeyEvent) -> Option<RiskKey> {
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
pub enum DrillKey {
    Close,
    Up,
    Down,
    PageUp,
    PageDown,
}

// Ctrl+C carries CONTROL and 'q'/'Q' are the global quit keys; both must fall
// through to should_quit rather than being swallowed by the drill-down.
pub fn journal_drill_key(key: &KeyEvent) -> Option<DrillKey> {
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
pub fn apply_drill_key(dk: DrillKey, drill: &mut JournalDrill) -> bool {
    match dk {
        DrillKey::Close => return true,
        DrillKey::Up => drill.rev = drill.rev.saturating_add(1),
        DrillKey::Down => drill.rev = drill.rev.saturating_sub(1),
        DrillKey::PageUp => drill.rev = drill.rev.saturating_add(DRILL_PAGE),
        DrillKey::PageDown => drill.rev = drill.rev.saturating_sub(DRILL_PAGE),
    }
    false
}

pub fn should_quit(event: &Event) -> bool {
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

pub fn tab_for(event: &Event, current: Tab) -> Option<Tab> {
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

/// Which run-loop handler claims a key event. Encoding the cascade as data makes
/// the load-bearing priority testable and lifts it out of a nested if/else chain.
#[derive(Debug, PartialEq, Eq)]
pub enum Route {
    Halt,
    Service,
    Scenario,
    Drill,
    Overview(OverviewKey),
    Scenarios(ScenariosKey),
    Fills(FillsKey),
    Risk(RiskKey),
    Quit,
    Tab(Tab),
    Ignore,
}

/// Route one key event through the priority cascade, in the exact order the old
/// `run()` if/else chain used:
///   1 halt prompt, 2 service confirm, 3 scenario confirm (all claim the key on
///   the outer `is_active` predicate regardless of which key it is), 4 drill
///   (quit keys resolve first, else the drill swallows the key), then the tab
///   key layers 5 overview, 6 scenarios, 7 fills, 8 risk, 9 global quit,
///   10 tab switch. A confirm layer shadows every lower layer, so e.g. an active
///   halt prompt swallows overview keys and quit keys alike (Ctrl+C/q become a
///   buffer char, never a quit) — this preserves today's behavior exactly.
pub fn route(
    prompt_active: bool,
    confirm_active: bool,
    scen_active: bool,
    drill_open: bool,
    tab: Tab,
    event: &Event,
    key: &KeyEvent,
) -> Route {
    if prompt_active {
        return Route::Halt;
    }
    if confirm_active {
        return Route::Service;
    }
    if scen_active {
        return Route::Scenario;
    }
    if drill_open {
        if should_quit(event) {
            return Route::Quit;
        }
        return Route::Drill;
    }
    if let Some(ok) = overview_key(tab, key) {
        return Route::Overview(ok);
    }
    if let Some(sk) = scenarios_key(tab, key) {
        return Route::Scenarios(sk);
    }
    if let Some(fk) = fills_key(tab, key) {
        return Route::Fills(fk);
    }
    if let Some(rk) = risk_tab_key(tab, key) {
        return Route::Risk(rk);
    }
    if should_quit(event) {
        return Route::Quit;
    }
    if let Some(next) = tab_for(event, tab) {
        return Route::Tab(next);
    }
    Route::Ignore
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
            feed: crate::sources::feed::FeedState::default(),
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

    fn route_key(
        prompt: bool,
        confirm: bool,
        scen: bool,
        drill: bool,
        tab: Tab,
        code: KeyCode,
        mods: KeyModifiers,
    ) -> Route {
        let k = press(code, mods);
        route(prompt, confirm, scen, drill, tab, &Event::Key(k), &k)
    }

    #[test]
    fn halt_prompt_swallows_overview_keys() {
        // 'r' is an Overview action, but an active halt prompt shadows it.
        assert_eq!(
            route_key(
                true,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('r'),
                KeyModifiers::NONE,
            ),
            Route::Halt
        );
    }

    #[test]
    fn service_confirm_blocks_journal_drill_open() {
        // Enter opens the journal drill on Overview, but a service confirm shadows it.
        assert_eq!(
            route_key(
                false,
                true,
                false,
                false,
                Tab::Overview,
                KeyCode::Enter,
                KeyModifiers::NONE,
            ),
            Route::Service
        );
    }

    #[test]
    fn scenario_confirm_blocks_tab_switches() {
        for code in [KeyCode::Tab, KeyCode::Char('2')] {
            assert_eq!(
                route_key(
                    false,
                    false,
                    true,
                    false,
                    Tab::Scenarios,
                    code,
                    KeyModifiers::NONE,
                ),
                Route::Scenario,
                "{code:?} must be swallowed by the scenario confirm"
            );
        }
    }

    #[test]
    fn drill_swallows_overview_action_chars() {
        for c in ['r', 's', 'x', 'f'] {
            assert_eq!(
                route_key(
                    false,
                    false,
                    false,
                    true,
                    Tab::Overview,
                    KeyCode::Char(c),
                    KeyModifiers::NONE,
                ),
                Route::Drill,
                "{c} must route to the drill, not a service action"
            );
        }
    }

    #[test]
    fn quit_matrix_matches_todays_behavior() {
        // In a drill the quit keys resolve first.
        for (code, mods) in [
            (KeyCode::Char('q'), KeyModifiers::NONE),
            (KeyCode::Char('Q'), KeyModifiers::NONE),
            (KeyCode::Char('c'), KeyModifiers::CONTROL),
        ] {
            assert_eq!(
                route_key(false, false, false, true, Tab::Overview, code, mods),
                Route::Quit,
                "{code:?} in a drill must quit"
            );
        }
        // With no modal, quit keys reach the global quit layer.
        assert_eq!(
            route_key(
                false,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('q'),
                KeyModifiers::NONE,
            ),
            Route::Quit
        );
        assert_eq!(
            route_key(
                false,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('c'),
                KeyModifiers::CONTROL,
            ),
            Route::Quit
        );
        // Inside a confirm, quit keys are swallowed by the confirm layer (they
        // become a buffer char via to_halt_key); they never reach the quit layer.
        assert_eq!(
            route_key(
                true,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('q'),
                KeyModifiers::NONE,
            ),
            Route::Halt
        );
        assert_eq!(
            route_key(
                true,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('c'),
                KeyModifiers::CONTROL,
            ),
            Route::Halt
        );
        assert_eq!(
            route_key(
                false,
                true,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('q'),
                KeyModifiers::NONE,
            ),
            Route::Service
        );
        assert_eq!(
            route_key(
                false,
                false,
                true,
                false,
                Tab::Scenarios,
                KeyCode::Char('q'),
                KeyModifiers::NONE,
            ),
            Route::Scenario
        );
    }

    #[test]
    fn no_modal_passes_through_to_the_tab_key_layers() {
        assert_eq!(
            route_key(
                false,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('r'),
                KeyModifiers::NONE,
            ),
            Route::Overview(OverviewKey::Act(Verb::Restart))
        );
        assert_eq!(
            route_key(
                false,
                false,
                false,
                false,
                Tab::Overview,
                KeyCode::Tab,
                KeyModifiers::NONE,
            ),
            Route::Tab(Tab::FeedsBooks)
        );
    }

    // Priority-swap canary: if the halt and service layers were reordered, this
    // would return Route::Service and grow the service buffer instead of the halt
    // buffer. Halt must win when both prompts are somehow active.
    #[test]
    fn route_prefers_halt_over_service_when_both_active() {
        assert_eq!(
            route_key(
                true,
                true,
                false,
                false,
                Tab::Overview,
                KeyCode::Char('x'),
                KeyModifiers::NONE,
            ),
            Route::Halt
        );
    }
}

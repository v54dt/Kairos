use ratatui::Frame;
use ratatui::Terminal;
use ratatui::backend::TestBackend;

/// Render `draw` into an off-screen `w`x`h` TestBackend and return the buffer as
/// newline-joined rows of cell symbols, for panel render assertions.
pub fn buffer_text<F: FnOnce(&mut Frame)>(w: u16, h: u16, draw: F) -> String {
    let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
    term.draw(draw).unwrap();
    let buf = term.backend().buffer().clone();
    let mut s = String::new();
    for y in 0..buf.area.height {
        for x in 0..buf.area.width {
            s.push_str(buf[(x, y)].symbol());
        }
        s.push('\n');
    }
    s
}

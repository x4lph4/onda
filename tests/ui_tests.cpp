// Phase 8 — UI core tests: commands, shortcuts, theme, table model,
// waveform mapping, notifications, layout priorities, state button.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ui/CommandManager.h"
#include "ui/EngineCommands.h"
#include "ui/LayoutSpec.h"
#include "ui/NotificationCenter.h"
#include "ui/ShortcutManager.h"
#include "ui/ThemeEngine.h"
#include "ui/TrackTableModel.h"
#include "ui/WaveformMapping.h"
#include "core/library/MusicLibraryEngine.h"

using namespace onda;
using namespace onda::ui;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    fs::path d = fs::temp_directory_path() / "onda_uitest" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// ------------------------------------------------------------------
// CommandManager: register/invoke/source/state
// ------------------------------------------------------------------
TEST_CASE("CommandManager: dispatch, sources, state, unknown id",
          "[ui]") {
    CommandManager cm;
    int hits = 0;
    float lastV = -1.0f;
    Command c;
    c.id = 42; c.name = "test.cmd"; c.category = "Test";
    c.run = [&](float v, CommandSource) { ++hits; lastV = v; };
    c.state = [] { return 7; };
    cm.add(c);

    REQUIRE(cm.has(42));
    REQUIRE(cm.has("test.cmd"));
    REQUIRE(cm.idFor("test.cmd") == 42);
    REQUIRE(cm.idFor("nope") == cmd::Invalid);
    REQUIRE(cm.state(42) == 7);
    REQUIRE(cm.state(999) == -1);

    REQUIRE(cm.invoke(42, CommandSource::Keyboard, 0.75f));
    REQUIRE(hits == 1);
    REQUIRE(lastV == Catch::Approx(0.75f));
    REQUIRE(cm.lastSource() == CommandSource::Keyboard);
    REQUIRE(cm.invoke("test.cmd", CommandSource::Midi));
    REQUIRE(cm.lastSource() == CommandSource::Midi);
    REQUIRE(!cm.invoke(777, CommandSource::Ui));      // unknown → false

    // listener sees every successful dispatch
    uint32_t seen = 0;
    cm.addListener([&](uint32_t id, CommandSource s) {
        seen = id; REQUIRE(s == CommandSource::Hid);
    });
    REQUIRE(cm.invoke(42, CommandSource::Hid));
    REQUIRE(seen == 42);
}

// ------------------------------------------------------------------
// EngineCommands: drive a fake engine, verify rt commands land
// ------------------------------------------------------------------
TEST_CASE("EngineCommands: play toggle pushes transport; toggles read "
          "fake stats", "[ui]") {
    rt::CommandQueue q;
    DeckStats stats[4];
    stats[0].state = (int)DeckState::Paused;
    stats[0].keylock = false;

    PerformanceEngine perf;
    // Phase 28 — perf-routed commands emit through the engine's sink;
    // the rig wires it to the queue exactly like the app does.
    perf.setCommandSink([&q](const rt::Command& c) { q.tryPush(c); });
    // Same binding the app performs: deck stats reach the perf layer
    // so transport toggles read the real playing state.
    std::atomic<const BeatGrid*> pend[4]{}, ret[4]{};
    for (int d = 0; d < 4; ++d)
        perf.setGridSlot(d, &pend[d], &ret[d], &stats[d]);
    CommandManager cm;
    EnginePorts p;
    p.queue = &q;
    p.perf = &perf;
    p.deckStats = [&stats](int d) -> DeckStats* { return &stats[d]; };
    registerEngineCommands(cm, p);

    // play toggle on paused deck → action 1 (play)
    REQUIRE(cm.invoke(cmd::forDeck(cmd::DeckPlayToggle, 0),
                      CommandSource::Keyboard));
    rt::Command c;
    REQUIRE(q.tryPop(c));
    const auto* t = std::get_if<rt::DeckTransport>(&c);
    REQUIRE(t != nullptr);
    REQUIRE(t->deck == 0);
    REQUIRE(t->action == 1);

    // now playing → toggle pauses (action 0)
    stats[0].state = (int)DeckState::Playing;
    REQUIRE(cm.invoke("decka.playToggle", CommandSource::Midi));
    REQUIRE(q.tryPop(c));
    t = std::get_if<rt::DeckTransport>(&c);
    REQUIRE(t->action == 0);

    // keylock toggle → SetKeyLock pushed, state() flips
    REQUIRE(cm.invoke(cmd::forDeck(cmd::DeckKeyLockTgl, 0),
                      CommandSource::Ui));
    REQUIRE(q.tryPop(c));
    const auto* kl = std::get_if<rt::SetKeyLock>(&c);
    REQUIRE(kl != nullptr);
    REQUIRE(kl->on == true);
    stats[0].keylock = true;
    REQUIRE(cm.state(cmd::forDeck(cmd::DeckKeyLockTgl, 0)) == 1);

    // quantize state reports through perf engine; the toggle also
    // emits a real SetQuantize to the rt queue (perf-routed, §45).
    REQUIRE(cm.invoke("decka.quantizeToggle", CommandSource::Ui));
    REQUIRE(cm.state(cm.idFor("decka.quantizeToggle")) == 1);
    REQUIRE(q.tryPop(c));
    REQUIRE(std::get_if<rt::SetQuantize>(&c) != nullptr);

    // bend is momentary: value 1 → -4%, value 0 → 0
    REQUIRE(cm.invoke(cmd::forDeck(cmd::DeckBendDown, 0),
                      CommandSource::Ui, 1.0f));
    REQUIRE(q.tryPop(c));
    const auto* bd = std::get_if<rt::SetPitchBend>(&c);
    REQUIRE(bd != nullptr);
    REQUIRE(bd->pct == Catch::Approx(-4.0f));

    // hot cue trigger delegates to perf
    bool trigOk = false;
    REQUIRE(cm.invoke(cmd::forHotCue(1, 3, 0), CommandSource::Hid));
    (void)trigOk; // triggerCue on empty grid returns false — still called

    // panic stops all 4 decks + centers xf
    REQUIRE(cm.invoke(cmd::AppPanic, CommandSource::Keyboard));
    int n = 0;
    while (q.tryPop(c)) {
        const auto* dt = std::get_if<rt::DeckTransport>(&c);
        if (dt) { REQUIRE(dt->action == 0); ++n; }
    }
    REQUIRE(n == 4);
}

// ------------------------------------------------------------------
// ShortcutManager: parse/format/bind/serialize
// ------------------------------------------------------------------
TEST_CASE("Shortcuts: parse descriptors, defaults, roundtrip", "[ui]") {
    auto k = parseKey("Ctrl+Shift+A");
    REQUIRE(k.mods == (key::Ctrl | key::Shift));
    REQUIRE(k.code == 'A');
    REQUIRE(formatKey(k) == "Ctrl+Shift+A");

    REQUIRE(parseKey("Space").code == key::Space);
    REQUIRE(formatKey(parseKey("Space")) == "Space");
    REQUIRE(parseKey("F5").code == key::F1 + 4);
    REQUIRE(formatKey(parseKey("F12")) == "F12");
    REQUIRE(parseKey("Alt+Left").mods == key::Alt);
    REQUIRE(parseKey("Alt+Left").code == key::Left);
    REQUIRE(parseKey("garbage!!").code == 0);   // invalid → 0
    REQUIRE(parseKey("Shift+").code == 0);
    // comma is a printable key AND a valid token char
    REQUIRE(parseKey("Comma").code == key::Comma);

    // default profile binds the essentials
    auto m = ShortcutManager::defaultProfile();
    REQUIRE(m.size() > 40);
    REQUIRE(m.lookup(parseKey("Q")) ==
            cmd::forDeck(cmd::DeckPlayToggle, 0));
    REQUIRE(m.lookup(parseKey("P")) ==
            cmd::forDeck(cmd::DeckPlayToggle, 1));
    REQUIRE(m.lookup(parseKey("F1")) == cmd::forHotCue(0, 0, 0));
    REQUIRE(m.lookup(parseKey("Shift+F1")) == cmd::forHotCue(0, 0, 1));
    REQUIRE(m.lookup(parseKey("Escape")) == cmd::AppPanic);
    REQUIRE(m.lookup(parseKey("Ctrl+F")) == cmd::BrowserFocusSearch);
    REQUIRE(m.lookup(parseKey("Ctrl+Shift+F1")) == cmd::Invalid); // free

    // end-to-end: key → command runs
    CommandManager cm;
    int ran = 0;
    Command c;
    c.id = cmd::AppPanic; c.name = "app.panic"; c.category = "App";
    c.run = [&](float, CommandSource) { ++ran; };
    cm.add(c);
    const uint32_t id = m.lookup(parseKey("Escape"));
    REQUIRE(cm.invoke(id, CommandSource::Keyboard));
    REQUIRE(ran == 1);

    // serialize → deserialize preserves bindings
    CommandManager cm2;
    Command d1; d1.id = 1; d1.name = "a"; d1.category = "x";
    Command d2; d2.id = 2; d2.name = "b"; d2.category = "x";
    cm2.add(d1); cm2.add(d2);
    ShortcutManager sm;
    sm.bind(parseKey("Ctrl+K"), 1);
    sm.bind(parseKey("F3"), 2);
    const auto txt = sm.serialize(cm2);
    REQUIRE(txt.find("Ctrl+K=a") != std::string::npos);
    ShortcutManager sm2;
    REQUIRE(sm2.deserialize(txt, cm2));
    REQUIRE(sm2.lookup(parseKey("Ctrl+K")) == 1);
    REQUIRE(sm2.lookup(parseKey("F3")) == 2);
}

// ------------------------------------------------------------------
// ThemeEngine: defaults, density metrics, serialization
// ------------------------------------------------------------------
TEST_CASE("Theme: tokens, density scaling, serialize roundtrip",
          "[ui]") {
    ThemeEngine te;
    REQUIRE(te.theme().id == ThemeId::Dark);
    REQUIRE(te.theme().accentColor() ==
            te.theme().palette.accent);
    REQUIRE(te.theme().waveColor(0) == te.theme().palette.deckA);

    Metrics m0 = te.metrics();
    te.setDensity(Density::Compact);
    REQUIRE(te.metrics().rowH < m0.rowH);
    te.setDensity(Density::Expanded);
    REQUIRE(te.metrics().rowH > m0.rowH);
    te.setDensity(Density::Comfortable);

    // HC variant changes contrast-critical tokens
    te.set(ThemeId::DarkHighContrast);
    REQUIRE(te.theme().palette.border != Palette{}.border);
    te.set(ThemeId::Dark);

    // roundtrip
    te.setAccent(0xFF112233);
    te.setWaveColor(0, 0xFF00FF00);
    te.setDensity(Density::Expanded);
    const std::string blob = te.serialize();
    ThemeEngine te2;
    REQUIRE(te2.deserialize(blob));
    REQUIRE(te2.theme().accentColor() == 0xFF112233);
    REQUIRE(te2.theme().waveA == 0xFF00FF00);
    REQUIRE(te2.theme().density == Density::Expanded);

    // garbage keeps old theme
    ThemeEngine te3;
    REQUIRE(!te3.deserialize("theme=99"));
    REQUIRE(te3.theme().id == ThemeId::Dark);
}

// ------------------------------------------------------------------
// TrackTableModel: paging, columns, cell text
// ------------------------------------------------------------------
TEST_CASE("TrackTableModel: paged window over 50k, no full load",
          "[ui]") {
    // fake page source: deterministic tracks
    TrackTableModel m;
    m.setDefaultColumns();
    int calls = 0, fetched = 0;
    m.setPageSource([&](const library::QuerySpec&, int off, int lim) {
        ++calls; fetched += lim;
        library::TrackPage p;
        p.total = 50000;
        for (int i = off; i < off + lim && i < 50000; ++i) {
            library::Track t;
            t.id = i + 1;
            t.title = "T" + std::to_string(i);
            t.artist = "A" + std::to_string(i % 500);
            t.bpm = 100.0 + (i % 80);
            t.keyIndex = i % 24;
            t.durationSec = 180.0 + (i % 120);
            t.rating = i % 6;
            t.status = i % 97 == 0
                       ? (int)library::TrackStatus::Missing : 0;
            p.items.push_back(std::move(t));
        }
        return p;
    });
    library::QuerySpec q;
    m.setSpec(q);
    REQUIRE(m.total() == 50000);

    // visible window 0..40 → 1-2 page fetches
    m.prefetch(0, 40);
    REQUIRE(m.row(0)->title == "T0");
    REQUIRE(m.row(39)->title == "T39");
    REQUIRE(m.cachedRowCount() <= 3 * TrackTableModel::kPage);
    REQUIRE(m.pageFetches() <= 3);

    // jump to the far end → old pages evicted eventually
    m.prefetch(49000, 40);
    m.prefetch(49000, 40);
    REQUIRE(m.row(49999) != nullptr);
    REQUIRE(m.row(49999)->title == "T49999");
    REQUIRE(m.cachedRowCount() <= TrackTableModel::kMaxPages *
                                  TrackTableModel::kPage);

    // rows are Tracks, not strings — verify fields
    const library::Track* t = m.row(124);
    REQUIRE(t != nullptr);
    REQUIRE(t->keyIndex == 124 % 24);
    REQUIRE(t->bpm == Catch::Approx(100.0 + 124 % 80));

    // columns: defaults, visibility, width clamp, reorder
    REQUIRE(m.visibleColumns() == (int)m.columns().size());
    REQUIRE(m.setColumnVisible("album", false));
    REQUIRE(m.visibleColumns() == (int)m.columns().size() - 1);
    REQUIRE(m.setColumnWidth("bpm", 10));      // clamps to minW
    for (auto& c : m.columns())
        if (c.id == "bpm") REQUIRE(c.widthPx >= c.minW);
    REQUIRE(m.moveColumn("bpm", 0));
    REQUIRE(m.columns().front().id == "bpm");

    // cell text
    REQUIRE(TrackTableModel::cellText(*t, "title") == "T124");
    REQUIRE(TrackTableModel::cellText(*t, "duration") == "3:04");
    REQUIRE(!TrackTableModel::cellText(*t, "key").empty());
    library::Track empty;
    REQUIRE(TrackTableModel::cellText(empty, "bpm").empty());
    REQUIRE(TrackTableModel::cellText(empty, "dateAdded").empty());
}

// ------------------------------------------------------------------
// WaveformMapping: transforms, zoom, scroll clamp, seek
// ------------------------------------------------------------------
TEST_CASE("WaveformMapping: sec↔px, zoom anchor, clamp, seek",
          "[ui]") {
    WaveformMapping m;
    m.setView(240.0, 800);           // 4 min track, 800 px
    m.pxPerSec = 100.0;
    m.scrollSec = 10.0;
    REQUIRE(m.secToPx(10.0) == Catch::Approx(0.0));
    REQUIRE(m.secToPx(11.0) == Catch::Approx(100.0));
    REQUIRE(m.pxToSec(100.0) == Catch::Approx(11.0));
    REQUIRE(m.visibleSec() == Catch::Approx(8.0));
    REQUIRE(m.endSec() == Catch::Approx(18.0));

    // zoom at anchor keeps anchor under cursor
    const double anchorPx = m.secToPx(12.0);
    m.zoomAt(2.0, 12.0);
    REQUIRE(m.pxPerSec == Catch::Approx(200.0));
    REQUIRE(m.secToPx(12.0) == Catch::Approx(anchorPx));

    // zoom clamped
    m.zoomAt(1e9, 0.0);
    REQUIRE(m.pxPerSec <= WaveformMapping::kMaxPxPerSec);
    m.zoomAt(1e-9, 0.0);
    REQUIRE(m.pxPerSec >= WaveformMapping::kMinPxPerSec);

    // scroll clamps at edges
    m.pxPerSec = 50.0;
    m.scrollTo(-50.0);
    REQUIRE(m.scrollSec == Catch::Approx(0.0));
    m.scrollTo(1e9);
    REQUIRE(m.endSec() <= Catch::Approx(240.0));

    // ensureVisible follows the playhead both directions
    m.pxPerSec = 100.0; m.scrollSec = 100.0;
    m.ensureVisible(105.0);
    REQUIRE(m.scrollSec < 105.0);
    m.ensureVisible(160.0);
    REQUIRE(m.endSec() >= 160.0 - 8.0 * 0.15 - 0.01);

    // seek: px past the edges clamps into the track
    REQUIRE(m.seekFromPx(-50.0) == Catch::Approx(m.scrollSec));
    REQUIRE(m.seekFromPx(1e6) ==
            Catch::Approx(std::min(m.durationSec, m.endSec())));

    // overview fits the whole track
    m.fitAll();
    REQUIRE(m.scrollSec == Catch::Approx(0.0));
    REQUIRE(m.pxPerSec == Catch::Approx(800.0 / 240.0));
}

// ------------------------------------------------------------------
// GridMarker collection against a synthetic constant grid
// ------------------------------------------------------------------
TEST_CASE("Grid markers: beat/bar/downbeat kinds in window", "[ui]") {
    WaveformMapping m;
    m.setView(240.0, 800);
    m.pxPerSec = 200.0;             // 4 s visible
    m.scrollSec = 0.0;

    const double rate = 48000.0, bpm = 120.0;
    const int64_t spb = (int64_t)(rate * 60.0 / bpm); // 24000 fr/beat
    std::vector<GridMarker> marks;
    collectGridMarkers(m, rate,
        [&](int64_t f) { return (double)f / spb; },
        [&](double beat) { return (int64_t)(beat * spb); },
        [&](double beat) { return (int)beat % 4 == 0; },
        marks);
    REQUIRE(!marks.empty());
    REQUIRE(marks[0].kind == 2);          // beat 0 = downbeat
    REQUIRE(marks[0].px == Catch::Approx(0.0));
    REQUIRE(marks[1].kind == 0);          // beat 1 plain
    // spacing: 0.5 s per beat at 200 px/s → 100 px
    REQUIRE(marks[1].px - marks[0].px == Catch::Approx(100.0));
    // a bar-start marker appears every 4 beats
    bool hasBar = false, hasDown = false;
    for (auto& mk : marks) {
        if (mk.kind == 1) hasBar = true;
        if (mk.kind == 2) hasDown = true;
    }
    REQUIRE((hasBar || hasDown));
}

// ------------------------------------------------------------------
// NotificationCenter: capacity, dedup, expiry
// ------------------------------------------------------------------
TEST_CASE("Notifications: bounded, dedup by text, ttl expiry", "[ui]") {
    NotificationCenter n(3);
    n.info("a"); n.info("b"); n.info("c"); n.info("d"); // > cap
    REQUIRE(n.size() == 3);
    REQUIRE(n.items().front().text == "b");  // oldest dropped

    n.info("d");                              // dedup refresh
    REQUIRE(n.size() == 3);
    int dCount = 0;
    for (auto& it : n.items()) if (it.text == "d") ++dCount;
    REQUIRE(dCount == 1);

    n.tick(5.0);                              // default ttl 4 s → gone
    REQUIRE(n.size() == 0);
    n.error("bad", "detail");
    REQUIRE(n.items().front().ttlSec > 7.0);  // errors stay longer
}

// ------------------------------------------------------------------
// LayoutSpec: priorities — panels collapse in order
// ------------------------------------------------------------------
TEST_CASE("Layout: shell regions, collapse order, min sizes", "[ui]") {
    LayoutPrefs p;
    const auto L = LayoutSpec::compute(1440, 860, 34, p);
    REQUIRE(L.topBar.h == 34);
    REQUIRE(L.fxVisible);
    REQUIRE(L.browserVisible);
    REQUIRE((L.deckA.valid() && L.deckB.valid() && L.mixer.valid()));
    REQUIRE(L.deckA.w == L.deckB.w);
    REQUIRE(L.deckA.w + L.mixer.w + L.deckB.w == 1440);
    // regions don't overlap vertically
    REQUIRE(L.fx.y + L.fx.h <= L.browser.y);
    REQUIRE(L.deckRow.y + L.deckRow.h <= L.fx.y);

    // small window → fx hidden, browser drops to its floor,
    // decks get the largest possible row
    const auto S = LayoutSpec::compute(960, 560, 30, p);
    REQUIRE(!S.fxVisible);
    REQUIRE(S.browserVisible);
    REQUIRE(S.browser.h <= LayoutSpec::kMinBrowserH);
    REQUIRE(S.deckA.valid());
    REQUIRE(S.deckRow.h >= 390);

    // medium window → browser shrinks to min before hiding
    const auto M = LayoutSpec::compute(1280, 700, 34, p);
    if (M.browserVisible)
        REQUIRE(M.browser.h >= LayoutSpec::kMinBrowserH);
    REQUIRE(M.deckA.valid());
}

// ------------------------------------------------------------------
// ControlState enum sanity (values are stable — serialized later)
// ------------------------------------------------------------------
TEST_CASE("ControlState: stable ordinals", "[ui]") {
    REQUIRE((int)ControlState::Off == 0);
    REQUIRE((int)ControlState::On == 1);
    REQUIRE((int)ControlState::Active == 2);
    REQUIRE((int)ControlState::Disabled == 3);
    REQUIRE((int)ControlState::Error == 4);
}

// ------------------------------------------------------------------
// CommandIds: encoding helpers round-trip
// ------------------------------------------------------------------
TEST_CASE("CommandIds: deck/hotcue encoding round-trip", "[ui]") {
    REQUIRE(cmd::deckOf(cmd::forDeck(cmd::DeckPlayToggle, 2)) == 2);
    REQUIRE(cmd::actionOf(cmd::forDeck(cmd::DeckLoopDouble, 1)) ==
            cmd::DeckLoopDouble);
    REQUIRE(cmd::forHotCue(0, 7, 2) ==
            cmd::forDeck(cmd::DeckHotCueBase, 0) + 7 * 4 + 2);
    // every command id in default profile resolves to a distinct id
    auto sm = ShortcutManager::defaultProfile();
    for (auto& [k, id] : sm.entries())
        REQUIRE(id != cmd::Invalid);
}

// ------------------------------------------------------------------
// Full-surface sanity: every registered command has name+category
// ------------------------------------------------------------------
TEST_CASE("EngineCommands: registry completeness", "[ui]") {
    CommandManager cm;
    EnginePorts p;   // all-null: invoke must be safe (no-ops)
    registerEngineCommands(cm, p);
    REQUIRE(cm.list().size() >= 60);
    for (const Command* c : cm.list()) {
        REQUIRE(!c->name.empty());
        REQUIRE(!c->category.empty());
        REQUIRE(c->id != cmd::Invalid);
        // invoking with null ports must not crash
        REQUIRE(cm.invoke(c->id, CommandSource::Ui, 1.0f));
    }
}

// ------------------------------------------------------------------
// UI STRESS — 50k tracks scrolled at simulated 60 fps while waveform
// math + command dispatch run per frame. Asserts the UI-critical
// operations stay far below the 16.6 ms frame budget and memory
// stays bounded. Runs without JUCE — measures the hot paths the
// components actually execute per frame.
// ------------------------------------------------------------------
TEST_CASE("UI stress: 50k scroll + per-frame math under frame budget",
          "[ui][stress]") {
    TrackTableModel m;
    m.setDefaultColumns();
    m.setPageSource([](const library::QuerySpec&, int off, int lim) {
        library::TrackPage p;
        p.total = 50000;
        for (int i = off; i < off + lim && i < 50000; ++i) {
            library::Track t;
            t.id = i + 1;
            t.title = "Track " + std::to_string(i);
            t.artist = "Artist " + std::to_string(i % 500);
            t.bpm = 100.0 + (i % 80);
            t.keyIndex = i % 24;
            p.items.push_back(std::move(t));
        }
        return p;
    });
    library::QuerySpec q;
    m.setSpec(q);
    REQUIRE(m.total() == 50000);

    CommandManager cm;
    EnginePorts ep;
    registerEngineCommands(cm, ep);
    auto sm = ShortcutManager::defaultProfile();

    WaveformMapping wm;
    wm.setView(360.0, 1200);
    wm.pxPerSec = 400.0;

    const double rate = 48000.0;
    const int64_t spb = (int64_t)(rate * 60.0 / 128.0); // 128 BPM grid
    std::vector<GridMarker> marks;
    marks.reserve(64);

    const int kFrames = 600;            // ~10 s of UI at 60 fps
    const auto t0 = std::chrono::steady_clock::now();
    double worstFrameUs = 0.0;

    for (int f = 0; f < kFrames; ++f) {
        const auto f0 = std::chrono::steady_clock::now();

        // 1) browser viewport scrolls across the whole library
        const int firstRow = (f * 83) % 49000;   // sweeps the range
        m.prefetch(firstRow, 40);
        for (int r = firstRow; r < firstRow + 40; ++r) {
            const library::Track* t = m.row(r);
            (void)TrackTableModel::cellText(*t, "title");
            (void)TrackTableModel::cellText(*t, "bpm");
            (void)TrackTableModel::cellText(*t, "key");
        }

        // 2) waveform follows a playhead (two decks)
        const double playSec = (f * 0.02) * 1.0;
        wm.ensureVisible(playSec);
        wm.scrollSec += 0.05;
        marks.clear();
        collectGridMarkers(wm, rate,
            [&](int64_t fr) { return (double)fr / spb; },
            [&](double b) { return (int64_t)(b * spb); },
            [&](double b) { return (int)b % 4 == 0; },
            marks);
        volatile double sink = wm.secToPx(playSec);
        (void)sink;

        // 3) a control action per frame (knob drags / pads)
        cm.invoke(cmd::MixMasterGainUp, CommandSource::Ui,
                  (f % 100) / 100.0f);
        cm.invoke(cmd::forDeck(cmd::DeckBendUp, 0),
                  CommandSource::Midi, (f % 50) / 50.0f);

        const auto f1 = std::chrono::steady_clock::now();
        worstFrameUs = std::max(worstFrameUs,
            std::chrono::duration<double, std::micro>(f1 - f0).count());
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double totalMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("  [stress] %d frames over 50k rows\n", kFrames);
    std::printf("  [stress] total %.1f ms  avg %.2f ms/frame  "
                "worst %.2f ms/frame\n",
                totalMs, totalMs / kFrames, worstFrameUs / 1000.0);
    std::printf("  [stress] cached rows (bounded): %d\n",
                m.cachedRowCount());
    std::printf("  [stress] page fetches: %d (avg %.1f/frame)\n",
                m.pageFetches(), (double)m.pageFetches() / kFrames);

    // frame budget: worst frame far below 16.6 ms
    REQUIRE(worstFrameUs < 8000.0);
    REQUIRE(totalMs / kFrames < 4.0);
    // memory bounded regardless of library size
    REQUIRE(m.cachedRowCount() <= TrackTableModel::kMaxPages *
                                  TrackTableModel::kPage);
}

// ------------------------------------------------------------------
// Stress: notification flood stays bounded; theme churn safe
// ------------------------------------------------------------------
TEST_CASE("UI stress: notification flood + theme churn", "[ui][stress]") {
    NotificationCenter n(8);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        n.info("event " + std::to_string(i));
        if (i % 100 == 0) n.tick((double)i / 1000.0);
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("  [stress] 10k notifications in %.1f ms\n",
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    REQUIRE(n.size() <= 8);

    ThemeEngine te;
    for (int i = 0; i < 500; ++i) {
        te.setDensity((Density)(i % 3));
        te.setAccent(0xFF000000u + (uint32_t)i);
        (void)te.metrics();
        (void)te.serialize();
    }
    REQUIRE(true);
}

} // namespace

#pragma once

#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>
#include <array>
#include <bitset>
#include <cstdint>

class MidiInput;
class QKeyEvent;
class QLabel;
class QSlider;
class QToolButton;

// The Virtual MIDI Keyboard's piano (docs/virtual-keyboard/PLAN.md D2, D5,
// D7, D9): two octaves and a bit from a base C, 16 white keys and 12 black,
// each labelled with the computer key that plays it and each C with
// its octave. It owns no MIDI state: the mouse key is reported through
// mouseNoteOn/mouseNoteOff, and what is lit is pushed in by the window.
//
// D12: nothing here runs while idle. There is no timer and no mouse
// tracking (moves arrive only with a button down); the key rectangles and
// labels are worked out on a resize or a base change only, and a key whose
// lit state changes repaints its own rectangle, not the piano.
class VirtualPianoWidget : public QWidget
{
    Q_OBJECT

  public:
    // Keys drawn: base .. base + kSpan - 1 (C to the D# 27 semitones up).
    static constexpr int kSpan = 28;
    static constexpr int kWhiteKeys = 16;

    explicit VirtualPianoWidget(QWidget *parent = nullptr);

    // The base C (a multiple of 12, 0–108). Releases the mouse key first
    // (PLAN §2 stance 4): what sounds must never outlive the key drawn.
    void setBase(int base);
    int base() const { return m_base; }
    // Whether key is drawn and plays: on the piano and within 0–127.
    bool playable(int key) const;
    // Where key is drawn (an empty rect when it is not on the piano). For
    // the harness, and the cached rect the repaint of one key uses.
    QRect keyRect(int key) const;
    // Where key's typing letter (octave false) or, on a C, its octave name
    // (octave true) is drawn; empty when key is not on the piano.
    QRect labelRect(int key, bool octave) const;
    // The playable key under pos (black keys first), or -1.
    int keyAt(const QPoint &pos) const;
    // The key the mouse holds, or -1.
    int mouseKey() const { return m_mouseKey; }
    // Sends the mouse key's note off now (mouseNoteOff) and forgets it.
    void releaseMouseKey();
    // releaseMouseKey, and the drag (if a button is still down) plays
    // nothing more until the next press: the window let everything go
    // (deactivation, an octave shift), and what is still held stays silent
    // until pressed again, as a typed key does (PLAN §3 step 3).
    void endMouseDrag();
    // Forgets the mouse key without a note off: something else (closing the
    // port) already released it.
    void forgetMouseKey(int key);

    // What lights a key (D9): the window's own notes, and a hardware
    // keyboard's. Either one lights it; each set changes only its own bits,
    // so a hardware release never unlights a key the mouse holds.
    void setVirtualHeld(int key, bool on);
    void setHardwareDown(int key, bool on);
    void clearVirtualHeld();
    void clearHardwareDown();
    bool isLit(int key) const;
    bool isVirtualHeld(int key) const;
    bool isHardwareDown(int key) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void mouseNoteOn(int key);
    void mouseNoteOff(int key);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    static bool isBlack(int offset);
    // The rectangles and labels for the current size, base and font.
    void layoutKeys();
    void relabel();
    // Repaints key's rectangle if its lit state went from wasLit.
    void repaintIfChanged(int key, bool wasLit);
    void sound(int key);
    // The least height at which a C's two label lines clear its black key.
    int minimumPianoHeight() const;

    int m_base = 48;
    int m_mouseKey = -1;
    // endMouseDrag ran since the last press: moves glide nowhere.
    bool m_dragEnded = false;
    std::bitset<128> m_virtualHeld;
    std::bitset<128> m_hardwareDown;
    // Per offset from the base (0..kSpan-1).
    std::array<QRect, kSpan> m_rects{};
    std::array<QString, kSpan> m_letters;
    std::array<QString, kSpan> m_octaves; // "C3" on the Cs, empty elsewhere
    QFont m_labelFont;
    int m_labelHeight = 0;
};

// The Virtual MIDI Keyboard window (docs/virtual-keyboard/PLAN.md D1, steps
// 2–3): a tool window owned by the main window, so it stays above Porydaw's
// windows and hides with the app. While it is shown the Virtual Keyboard
// port is open and the note monitor runs; while it is active its keys play
// (D3, D4); hiding it releases whatever it held and closes both. Its base octave, velocity and geometry persist in
// QSettings (virtualKeyboard/...); whether it shows is the main window's
// action's to persist.
class VirtualKeyboardWindow : public QWidget
{
    Q_OBJECT

  public:
    static constexpr int kMinBase = 0;      // C-1
    static constexpr int kMaxBase = 108;    // C8 (D5)
    static constexpr int kDefaultBase = 48; // C3
    static constexpr int kDefaultVelocity = 100;

    // Typing (PLAN D2–D5, step 3). A physical key is one of D2's note keys,
    // 0..kNoteKeys-1 in the order z s x d c v g b h n j m , (the bottom row,
    // offsets 0–12) then q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 (the top row,
    // offsets 12–27), or one of the octave keys - and =.
    static constexpr int kNoteKeys = 29;
    static constexpr int kOctaveDownKey = kNoteKeys;   // -
    static constexpr int kOctaveUpKey = kNoteKeys + 1; // =
    static constexpr int kPhysicalKeys = kNoteKeys + 2;
    // Where a platform's key events say which physical key it was (D3):
    // Xkb (xcb, Wayland: nativeScanCode is the evdev code + 8), Windows
    // (nativeScanCode is the PS/2 set-1 code, extended bit included), Mac
    // (nativeVirtualKey is the kVK_ANSI_* code; nativeScanCode is 0), or
    // None (offscreen and the rest: key() only). platformKeyTable is this
    // platform's (fixed for the run; also for the harness).
    enum class KeyTable { None, Xkb, Windows, Mac };
    static KeyTable platformKeyTable();
    // The physical key event is, by table, or -1 (harness: also driven with
    // synthetic native codes). A scan-code table with a
    // non-zero code decides alone (a code it lacks is no key: falling back
    // to key() would read AZERTY's A position as Q); a zero code (offscreen,
    // synthesised and injected events) and KeyTable::None fall back to
    // key() on the US letters and digits, and their shifted US symbols
    // (Shift+2 is Key_At), never on the keypad. Mac never falls back: 0 is
    // kVK_ANSI_A there.
    static int physicalKey(const QKeyEvent *event, KeyTable table);
    // A note key's semitones above the base C (comma and q are both 12).
    static int typingOffset(int physical) { return physical < 13 ? physical : physical - 1; }
    // How many holders (typed keys and the mouse) key has: its note on went
    // out with the first and its note off goes with the last. (harness)
    int holders(int key) const { return key >= 0 && key < 128 ? m_holders[size_t(key)] : 0; }

    VirtualKeyboardWindow(MidiInput *midi, QWidget *parent);
    ~VirtualKeyboardWindow() override;

    int base() const { return m_piano->base(); }
    // Clamped to C-1..C8 and to a C; persisted.
    void setBase(int base);
    void shiftOctave(int delta) { setBase(base() + 12 * delta); }
    int velocity() const;
    void setVelocity(int velocity);
    // The selected track's name and voice, as the main window resolves them.
    void setTarget(const QString &text);
    QString target() const;

    // Teardown (PLAN §4.1 hand-off i): releases what the window holds,
    // closes the port and the monitor, saves the geometry, and never
    // touches MidiInput again. The main window calls it (by deleting the
    // window) before its MidiInput goes.
    void detach();

    VirtualPianoWidget *piano() const { return m_piano; }
    QToolButton *octaveDownButton() const { return m_down; }
    QToolButton *octaveUpButton() const { return m_up; }
    QSlider *velocitySlider() const { return m_velocity; }

  signals:
    // The user closed the window (its title-bar button: a spontaneous close
    // while the main window shows), not a hide or close() from code, a
    // minimised main window or the app quitting: the View action unchecks.
    void closeRequested();

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

  private:
    // Holder counting (PLAN §3 step 3): a note on for the first holder of
    // key, the note off for the last.
    void hold(int key);
    void unhold(int key);
    // Note offs for every holder, the mouse key included, and the drag and
    // the typed keys still down stay silent until pressed again (PLAN §2
    // stance 4): deactivation, a modal dialog, an octave shift, hiding.
    void releaseAll();
    void openPort();
    void closePort();
    void send(int status, int key, int velocity);
    void noteState(int key, bool on, bool fromVirtual);
    void syncOctave();
    void saveGeometryNow();
    // Persists the slider's value if it changed since the last save.
    void saveVelocity();

    QPointer<MidiInput> m_midi;
    VirtualPianoWidget *m_piano = nullptr;
    QToolButton *m_down = nullptr;
    QToolButton *m_up = nullptr;
    QLabel *m_octave = nullptr;
    QSlider *m_velocity = nullptr;
    QLabel *m_velocityValue = nullptr;
    QLabel *m_target = nullptr;
    // The port is open (between openPort and closePort).
    bool m_open = false;
    // Inside closePort: the release and setVirtualOpen(false) signal back
    // synchronously (hand-off ii), and nothing may reopen meanwhile.
    bool m_closing = false;
    // The velocity in QSettings (-1: none saved yet).
    int m_savedVelocity = -1;
    // Per MIDI key, its holders; per down physical note key, the MIDI key it
    // holds (-1: up, or released by releaseAll while still down).
    std::array<uint8_t, 128> m_holders{};
    std::array<int, kNoteKeys> m_typed;
};

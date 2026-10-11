#pragma once

#include <QWidget>

class QTimer;

// The transport toolbar's MIDI activity light (docs/midi-input/PLAN.md
// step 1): a painted dot that lights for a moment whenever MIDI input
// arrives. Dim while idle; the owner sets the tooltip (the open ports).
class MidiActivityLed : public QWidget
{
  public:
    static constexpr int kFlashMs = 120;

    explicit MidiActivityLed(QWidget *parent = nullptr);

    // Lights the dot for kFlashMs from now.
    void flash();
    bool isLit() const;
    // A port is open: the idle dot is drawn as a ring, not a faint dot,
    // so "listening" and "nothing connected" read differently.
    void setListening(bool listening);
    bool isListening() const { return m_listening; }

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QTimer *m_off = nullptr;
    bool m_lit = false;
    bool m_listening = false;
};

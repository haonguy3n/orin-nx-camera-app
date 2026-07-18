#include "theme.h"

#include <QFont>
#include <QWidget>

namespace Theme {

QString stylesheet()
{
    return QStringLiteral(R"(
        QMainWindow { background-color: #0b0f17; }
        QWidget {
            background-color: #0b0f17;
            color: #d7deea;
            font-size: 13px;
        }
        QWidget#transparent, QWidget#fieldBlock { background: transparent; }

        /* Product header and connection workspace */
        QFrame#appHeader {
            background: #111722;
            border: none;
            border-bottom: 1px solid #242c3a;
        }
        QLabel#brandMark {
            background: #3b82f6;
            color: white;
            border-radius: 8px;
            font-size: 14px;
            font-weight: 800;
            min-width: 42px;
            min-height: 42px;
            max-width: 42px;
            max-height: 42px;
        }
        QLabel#brandTitle { color: #f8fafc; font-size: 18px; font-weight: 700; }
        QLabel#eyebrow, QLabel#fieldLabel {
            color: #748096;
            font-size: 10px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        QLabel#connectionPill {
            background: #171d29;
            color: #7f8a9d;
            border: 1px solid #2a3343;
            border-radius: 14px;
            padding: 6px 12px;
            font-size: 10px;
            font-weight: 700;
        }
        QLabel#connectionPill[online="true"] {
            background: #10291f;
            color: #4ade80;
            border-color: #1e5a3a;
        }
        QFrame#connectionBar {
            background: #0e131d;
            border: none;
            border-bottom: 1px solid #202736;
        }
        QWidget#workspace { background: #0b0f17; }
        QLabel#pageTitle { color: #f5f7fb; font-size: 22px; font-weight: 700; }
        QLabel#panelTitle { color: #f5f7fb; font-size: 16px; font-weight: 700; }
        QLabel#subtitle { color: #7f8a9d; font-size: 12px; }

        /* Control panel and cards */
        QScrollArea#controlPanel {
            background: #101620;
            border: 1px solid #242c3a;
            border-radius: 9px;
        }
        QWidget#controlPanelBody { background: #101620; }
        QWidget#sectionCard {
            background: #151c27;
            border: 1px solid #273142;
            border-radius: 7px;
        }
        QWidget#sectionContent { background: #151c27; }
        QToolButton#sectionHeader {
            background: #151c27;
            border: none;
            text-align: left;
            padding: 10px 12px;
            color: #e8edf5;
            font-weight: 700;
            font-size: 12px;
        }
        QToolButton#sectionHeader:hover { background: #1a2330; color: #ffffff; }
        QToolButton#sectionHeader:disabled { color: #586275; }

        /* Inputs */
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: #171e2a;
            border: 1px solid #303a4b;
            border-radius: 6px;
            padding: 7px 10px;
            color: #e4e9f2;
            min-height: 18px;
            selection-background-color: #3b82f6;
        }
        QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover {
            border-color: #465268;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
            border: 1px solid #3b82f6;
        }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,
        QComboBox:disabled { background: #121722; color: #586275; }

        /* Combo dropdown */
        QComboBox::drop-down { border: none; width: 24px; }
        QComboBox QAbstractItemView {
            background: #171e2a;
            border: 1px solid #354156;
            border-radius: 5px;
            padding: 4px;
            selection-background-color: #2e6fd2;
            selection-color: white;
        }

        /* Spin buttons */
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background: #202938; border: none; width: 17px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #34445d;
        }

        /* Buttons */
        QPushButton {
            background: #1a2230;
            border: 1px solid #344055;
            border-radius: 6px;
            padding: 7px 15px;
            color: #dce3ee;
            font-weight: 600;
        }
        QPushButton:hover { background: #232e3f; border-color: #53627a; color: white; }
        QPushButton:pressed { background: #121924; }
        QPushButton:disabled { color: #566175; border-color: #252d3a; background: #141a24; }

        /* Accent button (Connect, Calibrate) */
        QPushButton#accent {
            background: #3b82f6;
            border: 1px solid #3b82f6;
            color: white;
            font-weight: 700;
        }
        QPushButton#accent:hover { background: #5594f7; border-color: #5594f7; }
        QPushButton#accent:pressed { background: #2869d2; }
        QPushButton#accent:disabled {
            background: #243553; border-color: #2b4165; color: #6980a5;
        }

        /* Checkbox */
        QCheckBox { color: #cbd3df; spacing: 8px; }
        QCheckBox::indicator {
            width: 15px; height: 15px;
            border: 1px solid #465268; border-radius: 4px; background: #171e2a;
        }
        QCheckBox::indicator:checked {
            background: #3b82f6; border-color: #3b82f6;
        }

        /* Labels */
        QLabel { background: transparent; color: #cbd3df; }
        QCheckBox { background: transparent; }
        QLabel#mono {
            font-family: 'DejaVu Sans Mono', 'Consolas', monospace;
            font-size: 11px; color: #8995a8;
        }
        QLabel#mono[role="videoStatus"] {
            background: #111722;
            border-top: 1px solid #242c3a;
            color: #9aa7ba;
            padding: 10px 14px;
            font-size: 10px;
            font-weight: 700;
        }
        QLabel#error { color: #fb7185; font-size: 11px; }
        QLabel#cameraGlyph { color: #344158; font-size: 48px; }
        QLabel#emptyTitle { color: #dce3ee; font-size: 17px; font-weight: 600; }
        QLabel#emptyHint { color: #667287; font-size: 12px; }

        QStackedWidget#videoCard {
            background: #05070b;
            border: 1px solid #242c3a;
            border-radius: 9px;
        }

        QProgressBar {
            background: #111722;
            border: 1px solid #2a3343;
            border-radius: 5px;
            color: white;
            text-align: center;
            min-height: 16px;
        }
        QProgressBar::chunk { background: #3b82f6; border-radius: 4px; }

        QMenu, QToolTip {
            background: #18202c;
            color: #e4e9f2;
            border: 1px solid #354156;
            padding: 5px;
        }
        QMenu::item { padding: 7px 18px; border-radius: 4px; }
        QMenu::item:selected { background: #2e6fd2; }

        /* Scroll area + scroll bar */
        QScrollArea { background: #101620; border: none; }
        QScrollBar:vertical { background: #101620; width: 9px; margin: 2px; }
        QScrollBar::handle:vertical {
            background: #344055; border-radius: 4px; min-height: 24px;
        }
        QScrollBar::handle:vertical:hover { background: #4c5b73; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
    )");
}

void applyFont(QWidget *widget)
{
    QFont f = widget->font();
    f.setFamilies({QStringLiteral("Inter"), QStringLiteral("Noto Sans"),
                   QStringLiteral("DejaVu Sans")});
    f.setPointSize(9);
    widget->setFont(f);
}

} // namespace Theme

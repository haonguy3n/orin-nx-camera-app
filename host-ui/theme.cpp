#include "theme.h"

#include <QFont>
#include <QWidget>

namespace Theme {

QString stylesheet()
{
    return QStringLiteral(R"(
        QMainWindow { background-color: #090b0a; }
        QWidget {
            background-color: #090b0a;
            color: #e2e7e2;
            font-size: 13px;
        }
        QWidget#transparent, QWidget#fieldBlock { background: transparent; }

        /* Product header and connection workspace */
        QFrame#appHeader {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #151a16, stop:0.48 #101311,
                                        stop:1 #0d100e);
            border: none;
            border-bottom: 1px solid #293029;
        }
        QLabel#brandMark {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #d7ff7a, stop:0.55 #b7f34a,
                                        stop:1 #7ed957);
            color: #10150d;
            border-radius: 13px;
            font-size: 14px;
            font-weight: 800;
            min-width: 42px;
            min-height: 42px;
            max-width: 42px;
            max-height: 42px;
        }
        QLabel#brandTitle { color: #f8fafc; font-size: 18px; font-weight: 700; }
        QLabel#eyebrow, QLabel#fieldLabel {
            color: #869087;
            font-size: 10px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        QLabel#connectionPill {
            background: #171b18;
            color: #929a93;
            border: 1px solid #343b34;
            border-radius: 16px;
            padding: 7px 13px;
            font-size: 10px;
            font-weight: 700;
        }
        QLabel#connectionPill[online="true"] {
            background: #172814;
            color: #b7f34a;
            border-color: #52762d;
        }
        QFrame#connectionBar {
            background: #0d100e;
            border: none;
            border-bottom: 1px solid #232923;
        }
        QWidget#workspace { background: #090b0a; }
        QLabel#pageTitle { color: #f5f8f5; font-size: 22px; font-weight: 700; }
        QLabel#panelTitle { color: #f5f8f5; font-size: 16px; font-weight: 700; }
        QLabel#subtitle { color: #8b948c; font-size: 12px; }

        /* Control panel and cards */
        QScrollArea#controlPanel {
            background: #111512;
            border: 1px solid #303831;
            border-radius: 16px;
        }
        QWidget#controlPanelViewport {
            background: #111512;
            border-radius: 15px;
        }
        QWidget#controlPanelBody {
            background: #111512;
            border-radius: 15px;
        }
        QWidget#sectionCard {
            background: #191e1a;
            border: 1px solid #333c34;
            border-radius: 12px;
        }
        QWidget#sectionContent {
            background: #191e1a;
            border-bottom-left-radius: 11px;
            border-bottom-right-radius: 11px;
        }
        QToolButton#sectionHeader {
            background: #191e1a;
            border: none;
            border-radius: 11px;
            text-align: left;
            padding: 11px 13px;
            color: #edf2ed;
            font-weight: 700;
            font-size: 12px;
        }
        QToolButton#sectionHeader:hover { background: #242c25; color: #cfff68; }
        QToolButton#sectionHeader:checked { color: #b7f34a; }
        QToolButton#sectionHeader:disabled { color: #606961; }

        /* Inputs */
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: #181c19;
            border: 1px solid #39423a;
            border-radius: 10px;
            padding: 8px 11px;
            color: #e9ede9;
            min-height: 18px;
            selection-background-color: #84b936;
        }
        QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover {
            border-color: #657365;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
            border: 1px solid #b7f34a;
        }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,
        QComboBox:disabled { background: #131613; color: #626a62; }

        /* Combo dropdown */
        QComboBox::drop-down { border: none; width: 24px; }
        QComboBox QAbstractItemView {
            background: #1c211d;
            border: 1px solid #475248;
            border-radius: 10px;
            padding: 4px;
            selection-background-color: #789f38;
            selection-color: #0d110b;
        }

        /* Spin buttons */
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background: #242a25; border: none; width: 18px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #445044;
        }

        /* Buttons */
        QPushButton {
            background: #1d221e;
            border: 1px solid #414a42;
            border-radius: 10px;
            padding: 8px 16px;
            color: #e6ebe6;
            font-weight: 600;
        }
        QPushButton:hover { background: #29312a; border-color: #6d7d6e; color: white; }
        QPushButton:pressed { background: #141814; }
        QPushButton:disabled { color: #626a63; border-color: #2b312c; background: #151815; }

        /* Accent button (Connect, Calibrate) */
        QPushButton#accent {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #a8e83f, stop:1 #d0f75e);
            border: 1px solid #b7f34a;
            color: #11160e;
            font-weight: 700;
        }
        QPushButton#accent:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #bdf55b, stop:1 #e0ff7b);
            border-color: #d8ff7a;
        }
        QPushButton#accent:pressed { background: #91c936; }
        QPushButton#accent:disabled {
            background: #263021; border-color: #36442f; color: #65755b;
        }

        /* Checkbox */
        QCheckBox { color: #d7ddd7; spacing: 9px; }
        QCheckBox::indicator {
            width: 15px; height: 15px;
            border: 1px solid #5a675b; border-radius: 5px; background: #181c19;
        }
        QCheckBox::indicator:checked {
            background: #b7f34a; border-color: #d2ff79;
        }

        /* Labels */
        QLabel { background: transparent; color: #dbe0db; }
        QCheckBox { background: transparent; }
        QLabel#mono {
            font-family: 'DejaVu Sans Mono', 'Consolas', monospace;
            font-size: 11px; color: #969f97;
        }
        QLabel#mono[role="videoStatus"] {
            background: #111512;
            border-top: 1px solid #2e362f;
            border-bottom-left-radius: 13px;
            border-bottom-right-radius: 13px;
            color: #a1aaa2;
            padding: 10px 14px;
            font-size: 10px;
            font-weight: 700;
        }
        QLabel#error { color: #fb7185; font-size: 11px; }
        QLabel#cameraGlyph { color: #6d814e; font-size: 48px; }
        QLabel#emptyTitle { color: #edf2ed; font-size: 17px; font-weight: 600; }
        QLabel#emptyHint { color: #788179; font-size: 12px; }

        QWidget#videoPane, QWidget#videoPlaceholder {
            background: #060807;
            border-radius: 14px;
        }

        QStackedWidget#videoCard {
            background: #060807;
            border: 1px solid #303a31;
            border-radius: 16px;
        }

        QProgressBar {
            background: #151916;
            border: 1px solid #39433a;
            border-radius: 8px;
            color: white;
            text-align: center;
            min-height: 16px;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #9bd63d, stop:1 #d4f75e);
            border-radius: 7px;
        }

        QMenu, QToolTip {
            background: #1c211d;
            color: #e9ede9;
            border: 1px solid #475248;
            border-radius: 10px;
            padding: 5px;
        }
        QMenu::item { padding: 8px 18px; border-radius: 7px; }
        QMenu::item:selected { background: #769d36; color: #10140e; }

        /* Scroll area + scroll bar */
        QScrollArea { background: #111512; border: none; }
        QScrollBar:vertical { background: transparent; width: 9px; margin: 3px; }
        QScrollBar::handle:vertical {
            background: #455047; border-radius: 4px; min-height: 24px;
        }
        QScrollBar::handle:vertical:hover { background: #9bd63d; }
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

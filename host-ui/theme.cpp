#include "theme.h"

#include <QFont>
#include <QWidget>

namespace Theme {

QString stylesheet()
{
    return QStringLiteral(R"(
        QMainWindow, QWidget { background-color: #1a1a1a; color: #c8c8c8; }

        /* Collapsible section headers */
        QToolButton#sectionHeader {
            background: #222;
            border: none;
            border-bottom: 1px solid #333;
            text-align: left;
            padding: 6px 10px;
            color: #e67e22;
            font-weight: bold;
            font-size: 11px;
        }
        QToolButton#sectionHeader:hover { background: #2a2a2a; }
        QToolButton#sectionHeader:disabled { color: #555; }

        /* Inputs */
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: #2a2a2a;
            border: 1px solid #383838;
            border-radius: 3px;
            padding: 3px 6px;
            color: #c8c8c8;
            selection-background-color: #e67e22;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
            border: 1px solid #e67e22;
        }

        /* Combo dropdown */
        QComboBox::drop-down { border: none; width: 18px; }
        QComboBox QAbstractItemView {
            background: #2a2a2a;
            border: 1px solid #383838;
            selection-background-color: #e67e22;
            selection-color: #1a1a1a;
        }

        /* Spin buttons */
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background: #333; border: none; width: 14px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #e67e22;
        }

        /* Buttons */
        QPushButton {
            background: #2a2a2a;
            border: 1px solid #383838;
            border-radius: 3px;
            padding: 5px 14px;
            color: #c8c8c8;
        }
        QPushButton:hover { border: 1px solid #e67e22; color: #e67e22; }
        QPushButton:pressed { background: #1a1a1a; }
        QPushButton:disabled { color: #555; border-color: #2a2a2a; }

        /* Accent button (Connect, Calibrate) */
        QPushButton#accent {
            background: #e67e22;
            border: 1px solid #e67e22;
            color: #1a1a1a;
            font-weight: bold;
        }
        QPushButton#accent:hover { background: #f39c12; border-color: #f39c12; }
        QPushButton#accent:pressed { background: #d35400; }
        QPushButton#accent:disabled {
            background: #4a3520; border-color: #4a3520; color: #666;
        }

        /* Checkbox */
        QCheckBox { color: #c8c8c8; }
        QCheckBox::indicator {
            width: 14px; height: 14px;
            border: 1px solid #383838; border-radius: 2px; background: #2a2a2a;
        }
        QCheckBox::indicator:checked {
            background: #e67e22; border-color: #e67e22;
        }

        /* Labels */
        QLabel { color: #c8c8c8; }
        QLabel#mono {
            font-family: 'DejaVu Sans Mono', 'Consolas', monospace;
            font-size: 11px; color: #999;
        }
        QLabel#error { color: #e74c3c; font-size: 11px; }

        /* Scroll area + scroll bar */
        QScrollArea { background: #1a1a1a; border: none; }
        QScrollBar:vertical { background: #1a1a1a; width: 8px; }
        QScrollBar::handle:vertical {
            background: #383838; border-radius: 4px; min-height: 20px;
        }
        QScrollBar::handle:vertical:hover { background: #e67e22; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
    )");
}

void applyFont(QWidget *widget)
{
    QFont f = widget->font();
    f.setPointSize(9);
    widget->setFont(f);
}

} // namespace Theme

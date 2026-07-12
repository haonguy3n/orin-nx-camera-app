#pragma once
#include <QString>

class QWidget;

namespace Theme {
    QString stylesheet();
    void applyFont(QWidget *widget);
}

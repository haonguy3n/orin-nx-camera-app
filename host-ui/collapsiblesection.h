#pragma once

#include <QWidget>

class QToolButton;

// A collapsible UI section: a clickable header bar (with arrow) above a
// content widget. Click the header to expand/collapse the content.
class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr);

    // The content widget — add your layout/widgets to this.
    QWidget *contentWidget() const { return m_content; }

    void setExpanded(bool expanded);
    bool isExpanded() const;

private:
    QToolButton *m_header;
    QWidget *m_content;
};

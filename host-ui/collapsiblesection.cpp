#include "collapsiblesection.h"

#include <QToolButton>
#include <QSizePolicy>
#include <QVBoxLayout>

CollapsibleSection::CollapsibleSection(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sectionCard"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_header = new QToolButton(this);
    m_header->setText(title);
    m_header->setCheckable(true);
    m_header->setChecked(true);
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setArrowType(Qt::DownArrow);
    m_header->setObjectName("sectionHeader");
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("sectionContent"));

    layout->addWidget(m_header);
    layout->addWidget(m_content);

    connect(m_header, &QToolButton::toggled, this, [this](bool checked) {
        m_content->setVisible(checked);
        m_header->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });
}

void CollapsibleSection::setExpanded(bool expanded)
{
    m_header->setChecked(expanded);
}

bool CollapsibleSection::isExpanded() const
{
    return m_header->isChecked();
}

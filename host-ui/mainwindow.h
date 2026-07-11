#pragma once

#include <QMainWindow>
#include <QUrl>

class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPushButton;
class QVideoWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    struct Pane {
        QString name;
        QMediaPlayer *player = nullptr;
        QVideoWidget *video = nullptr;
        QLabel *status = nullptr;
    };

    QWidget *createPane(Pane &pane, const QString &name);
    void connectStreams();
    void disconnectStreams();
    void setStatus(Pane &pane, const QString &text);
    QUrl streamUrl(int index) const;

    QLineEdit *m_hostEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    Pane m_panes[2];
    bool m_connected = false;
};

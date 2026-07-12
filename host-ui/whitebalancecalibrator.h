#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class ControlClient;
class QImage;
class QTimer;
class VideoPane;

// White-balance calibration: measures channel imbalance in cam0's decoded
// video and writes corrected wb trims via set-tuning. Applying set-tuning
// restarts the device's Argus daemon (~5 s outage), so the calibrator waits,
// reconnects both panes, lets AE settle, re-measures, and iterates once more
// if needed. Progress and results are reported via signals.
class WhiteBalanceCalibrator : public QObject
{
    Q_OBJECT

public:
    explicit WhiteBalanceCalibrator(ControlClient *control, QObject *parent = nullptr);

    void start(VideoPane *cam0Pane, VideoPane *cam1Pane);
    void abort();
    bool isRunning() const { return m_running; }

    // Update the latest tuning object from get-status (called by orchestrator).
    void setTuning(const QJsonObject &tuning) { m_lastTuning = tuning; }
    bool hasTuning() const { return !m_lastTuning.isEmpty(); }

signals:
    void progressMessage(const QString &message);
    void finished(const QString &message);
    void restartPanesRequested();

private:
    ControlClient *m_control;
    VideoPane *m_cam0 = nullptr;
    VideoPane *m_cam1 = nullptr;
    QJsonObject m_lastTuning;
    bool m_running = false;
    int m_pass = 0;

    void step();
    void finish(const QString &message);
    bool measureWhiteBalance(const QImage &image, double *needR,
                             double *needB, QString *why) const;
};

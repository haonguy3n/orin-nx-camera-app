#include "whitebalancecalibrator.h"

#include <QImage>
#include <QJsonObject>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

#include "controlclient.h"
#include "videopane.h"

#include "proto/Protocol.h"

namespace {
constexpr double kWbTolerance = 0.015;
constexpr int kMaxCalibrationPasses = 2;
constexpr int kRestartWaitMs = 7000;
constexpr int kSettleWaitMs = 5000;
}  // namespace

WhiteBalanceCalibrator::WhiteBalanceCalibrator(ControlClient *control,
                                               QObject *parent)
    : QObject(parent), m_control(control)
{
}

void WhiteBalanceCalibrator::start(VideoPane *cam0Pane, VideoPane *cam1Pane)
{
    if (m_running)
        return;
    m_cam0 = cam0Pane;
    m_cam1 = cam1Pane;
    m_running = true;
    m_pass = 1;
    step();
}

void WhiteBalanceCalibrator::abort()
{
    if (m_running)
        finish(QStringLiteral("calibration aborted"));
}

void WhiteBalanceCalibrator::step()
{
    if (!m_running)
        return;
    emit progressMessage(
        QStringLiteral("calibrating… pass %1/%2")
            .arg(qMin(m_pass, kMaxCalibrationPasses))
            .arg(kMaxCalibrationPasses));

    // Grab the frame cam0 is showing right now.
    QVideoSink *sink = m_cam0->videoSink();
    const QVideoFrame frame = sink ? sink->videoFrame() : QVideoFrame();
    const QImage image = frame.isValid() ? frame.toImage() : QImage();
    if (image.isNull()) {
        finish(QStringLiteral("calibration failed: no video frame"));
        return;
    }

    double needR = 1.0;
    double needB = 1.0;
    QString why;
    if (!measureWhiteBalance(image, &needR, &needB, &why)) {
        finish(QStringLiteral("calibration failed: %1").arg(why));
        return;
    }

    const bool neutral = qAbs(needR - 1.0) <= kWbTolerance
                         && qAbs(needB - 1.0) <= kWbTolerance;
    if (neutral || m_pass > kMaxCalibrationPasses) {
        // Report the measured residuals (R/G and B/G of the white region).
        finish(neutral && m_pass == 1
                   ? QStringLiteral("whites already neutral")
                   : QStringLiteral("calibrated: R/G %1, B/G %2")
                         .arg(1.0 / needR, 0, 'f', 2)
                         .arg(1.0 / needB, 0, 'f', 2));
        return;
    }

    if (!hasTuning()) {
        finish(QStringLiteral("device does not support set-tuning"));
        return;
    }

    // Compose the measured correction on top of the current trim.
    const double newR = qBound(
        0.5,
        m_lastTuning.value(QStringLiteral("wb_trim_r")).toDouble(1.0) * needR,
        2.0);
    const double newB = qBound(
        0.5,
        m_lastTuning.value(QStringLiteral("wb_trim_b")).toDouble(1.0) * needB,
        2.0);

    QJsonObject params;
    params.insert(QStringLiteral("wb_trim_r"), newR);
    params.insert(QStringLiteral("wb_trim_b"), newB);
    m_control->sendRequest(
        QLatin1String(proto::methods::kSetTuning), params,
        [this, newR, newB](const QJsonObject &, const QJsonObject &error) {
            if (!m_running)
                return;
            if (!error.isEmpty()) {
                // Devices without set-tuning answer kUnknownMethod (see
                // Protocol.h) — say so plainly instead of echoing the raw
                // "unknown method" error.
                const bool unsupported =
                    error.value(QStringLiteral("code")).toInt()
                    == proto::kUnknownMethod;
                finish(unsupported
                           ? QStringLiteral("calibration failed: device "
                                            "firmware does not support "
                                            "set-tuning")
                           : QStringLiteral("set-tuning: %1").arg(
                                 error.value(QStringLiteral("message"))
                                     .toString()));
                return;
            }
            // The poll is paused and the device is about to restart, so
            // remember locally what the next pass must compose on.
            m_lastTuning.insert(QStringLiteral("wb_trim_r"), newR);
            m_lastTuning.insert(QStringLiteral("wb_trim_b"), newB);
            emit progressMessage(
                QStringLiteral("calibrating… device restarting (pass %1/%2)")
                    .arg(m_pass)
                    .arg(kMaxCalibrationPasses));
            // set-tuning restarts the Argus daemon and every stream (~5 s
            // outage) — wait it out, reconnect both panes, then let the
            // streams and AE settle before measuring again.
            QTimer::singleShot(kRestartWaitMs, this, [this]() {
                if (!m_running)
                    return;
                emit restartPanesRequested();
                QTimer::singleShot(kSettleWaitMs, this, [this]() {
                    if (!m_running)
                        return;
                    ++m_pass;
                    step();
                });
            });
        });
}

void WhiteBalanceCalibrator::finish(const QString &message)
{
    m_running = false;
    emit finished(message);
}

bool WhiteBalanceCalibrator::measureWhiteBalance(const QImage &image,
                                                 double *needR,
                                                 double *needB,
                                                 QString *why) const
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull()) {
        *why = QStringLiteral("no video frame");
        return false;
    }

    // Mean each channel over near-neutral bright pixels: max(r,g,b) inside
    // (150, 250) — lit but not clipped — and max-min < 45 (roughly gray).
    // Every 2nd pixel in both directions is plenty of samples.
    quint64 sumR = 0;
    quint64 sumG = 0;
    quint64 sumB = 0;
    quint64 count = 0;
    quint64 sampled = 0;
    for (int y = 0; y < rgb.height(); y += 2) {
        const QRgb *row = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); x += 2) {
            ++sampled;
            const QRgb pixel = row[x];
            const int r = qRed(pixel);
            const int g = qGreen(pixel);
            const int b = qBlue(pixel);
            const int hi = qMax(r, qMax(g, b));
            const int lo = qMin(r, qMin(g, b));
            if (hi <= 150 || hi >= 250 || hi - lo >= 45)
                continue;
            sumR += r;
            sumG += g;
            sumB += b;
            ++count;
        }
    }

    if (sampled == 0 || count * 100 < sampled) {  // under 1% qualifying
        *why = QStringLiteral("not enough white/gray in view");
        return false;
    }

    // Gains that would pull the region's R and B means onto G. The filter
    // guarantees lo > 105, so the sums cannot be zero.
    *needR = static_cast<double>(sumG) / static_cast<double>(sumR);
    *needB = static_cast<double>(sumG) / static_cast<double>(sumB);
    return true;
}

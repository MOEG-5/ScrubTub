// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// HoverSession contract checks (TECH_SPEC.md section 6): keyframe-then-exact
// seeking, continuous motion, source switch/disengage hygiene, cached-only
// fallback when mpv is unavailable, warm seek latency. Every live case drives
// a real corpus file from the repository's vids/ folder, read as-is
// (AGENTS.md: read-only, never copied or derived).
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include <cmath>

#include "media/HoverSession.h"
#include "media/Probe.h"
#include "testsupport/MediaFixtures.h"

using namespace scrubtub;
using namespace scrubtub::testsupport;

// Media-dependent cases skip with a clear reason when the corpus is missing.
#define REQUIRE_REAL_MEDIA_OR_SKIP()                                        \
    do {                                                                    \
        if (m_video.isEmpty())                                              \
            QSKIP("vids/ corpus unavailable (set SCRUBTUB_TEST_VIDS_DIR)");  \
        if (!m_probe.ok || m_probe.durationMs <= 0)                         \
            QSKIP("ffprobe could not read the corpus video");                \
    } while (false)

namespace {

// A seek target inside the real clip, as a fraction of its duration.
qint64 atFraction(qint64 durationMs, double fraction)
{
    const qint64 target = static_cast<qint64>(std::llround(durationMs * fraction));
    return qBound<qint64>(0, target, qMax<qint64>(0, durationMs - 40));
}

} // namespace

class TestHoverSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void keyframeThenExactSeekUsesActualPts();
    void continuousMotionMakesProgressAndSettles();
    void initialPositionAndProcessPersistAcrossDisengage();
    void switchingVideosDropsStaleFrames();
    void disengageClearsFrameAndStopsDelivery();
    void disengageUnloadsSource();
    void cachedOnlyModeNeverEngages();
    void warmSeekLatency();
    void missingMpvUsesCachedPreviews_data();
    void missingMpvUsesCachedPreviews();

private:
    QString m_video;      // smallest corpus video (mp4 preferred), read-only
    ProbeResult m_probe;
    qint64 m_durationMs = -1;
    QString m_mpv;
    QString m_fingerprint;
};

void TestHoverSession::initTestCase()
{
    // Read-only guard: the corpus must be byte-identical afterwards.
    m_fingerprint = vidsFingerprint();

    m_video = smallestVideo(QStringLiteral("mp4"));
    if (m_video.isEmpty())
        m_video = smallestVideo();
    if (!m_video.isEmpty()) {
        m_probe = Probe::run(QStandardPaths::findExecutable(QStringLiteral("ffprobe")), m_video);
        if (m_probe.ok)
            m_durationMs = m_probe.durationMs;
    }
    // mpv is optional: cached previews carry hover feedback without it, and
    // the live cases report that gap honestly instead of faking a pass.
    m_mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"));
}

void TestHoverSession::cleanupTestCase()
{
    QCOMPARE(vidsFingerprint(), m_fingerprint);
}

void TestHoverSession::keyframeThenExactSeekUsesActualPts()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");

    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    const qint64 target = atFraction(m_durationMs, 0.5);

    QElapsedTimer latency;
    latency.start();
    session.engage(1, 1, m_video, m_durationMs, target);
    QVERIFY2(frames.wait(30000), "no keyframe preview");
    const qint64 firstPts = frames.first().at(2).toLongLong();
    const qint64 firstLatency = latency.elapsed();
    QVERIFY2(firstPts >= 0, "the first preview reported no timestamp");
    QVERIFY2(firstPts <= target - 100,
             qPrintable(QStringLiteral("first preview at %1 ms is not an earlier keyframe "
                                       "for target %2 ms")
                            .arg(firstPts).arg(target)));

    // The settle timer then refines to the requested position, still reporting
    // the delivered (not requested) timestamp.
    bool settled = false;
    qint64 finalPts = -1;
    while (!settled && latency.elapsed() < 3000) {
        if (!frames.wait(100))
            continue;
        for (const auto &row : frames) {
            const qint64 pts = row.at(2).toLongLong();
            if (qAbs(pts - target) <= 100) {
                settled = true;
                finalPts = pts;
            }
        }
    }
    QVERIFY2(settled, "exact seek did not settle near target");
    QVERIFY2(finalPts != target, "delivery reported the requested PTS");
    qDebug() << "first-preview/final latency" << firstLatency << latency.elapsed() << "ms"
             << "target" << target << "settled" << finalPts;
}

void TestHoverSession::continuousMotionMakesProgressAndSettles()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");

    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    const qint64 initial = atFraction(m_durationMs, 0.1);
    session.engage(2, 1, m_video, m_durationMs, initial);
    QVERIFY(frames.wait(30000));
    QElapsedTimer initialTimer;
    initialTimer.start();
    while (qAbs(session.lastFramePtsMs() - initial) > 100 && initialTimer.elapsed() < 3000)
        QTest::qWait(50);
    QVERIFY2(qAbs(session.lastFramePtsMs() - initial) <= 100,
             "initial position did not settle");
    frames.clear();

    // A pointer sweeping across the whole clip.
    const int motionTargets = 10;
    qint64 newest = initial;
    bool progressedDuringMotion = false;
    for (int i = 1; i <= motionTargets; ++i) {
        newest = atFraction(m_durationMs, 0.1 + 0.08 * i);
        session.scrub(newest);
        QTest::qWait(60);
        progressedDuringMotion |= !frames.isEmpty();
    }
    QVERIFY2(progressedDuringMotion, "no frame arrived during continuous motion");

    QElapsedTimer timer;
    timer.start();
    bool settled = false;
    while (!settled && timer.elapsed() < 3000) {
        if (!frames.wait(100))
            continue;
        for (const auto &row : frames)
            settled |= qAbs(row.at(2).toLongLong() - newest) <= 100;
    }
    QVERIFY2(settled, "newest target did not settle within 3 seconds");
    // Motion targets are coalesced: a sweep must not produce a delivery per
    // pointer event plus refinement.
    QVERIFY2(frames.size() <= motionTargets + 2,
             qPrintable(QStringLiteral("%1 deliveries for %2 motion targets")
                            .arg(frames.size()).arg(motionTargets)));
}

void TestHoverSession::initialPositionAndProcessPersistAcrossDisengage()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");

    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    const qint64 firstTarget = atFraction(m_durationMs, 0.3);

    session.engage(3, 1, m_video, m_durationMs, firstTarget);
    QVERIFY(frames.wait(30000));
    QVERIFY(frames.first().at(2).toLongLong() >= 0);
    // The engaged position is shown without any scrub() call.
    QElapsedTimer timer;
    timer.start();
    while (qAbs(session.lastFramePtsMs() - firstTarget) > 100 && timer.elapsed() < 3000)
        QTest::qWait(50);
    QVERIFY2(qAbs(session.lastFramePtsMs() - firstTarget) <= 100,
             "the initial position was never delivered");

    auto *first = session.findChild<QProcess *>();
    QVERIFY(first);
    const qint64 pid = first->processId();
    QVERIFY(pid > 0);
    const QStringList args = first->arguments();
    const auto sockets = args.filter(QStringLiteral("--input-ipc-server="));
    QCOMPARE(sockets.size(), 1);
    QVERIFY(sockets.first().contains(QStringLiteral("scrubtub-hover-")));
    QVERIFY(!sockets.first().contains(QStringLiteral("mpv-socket")));
    QVERIFY(args.contains(QStringLiteral("--media-controls=no")));

    // Leaving and re-entering keeps the warm process and loads the newest
    // revision at its own position.
    session.disengage();
    QTest::qWait(100);
    frames.clear();
    session.engage(3, 2, m_video, m_durationMs, atFraction(m_durationMs, 0.6));
    QVERIFY(frames.wait(30000));
    auto *second = session.findChild<QProcess *>();
    QVERIFY(second);
    QCOMPARE(second->processId(), pid);
}

void TestHoverSession::switchingVideosDropsStaleFrames()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");

    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);

    session.engage(10, 1, m_video, m_durationMs, atFraction(m_durationMs, 0.2));
    QVERIFY(frames.wait(30000));
    frames.clear();

    const qint64 secondTarget = atFraction(m_durationMs, 0.8);
    session.engage(20, 7, m_video, m_durationMs, secondTarget);
    QVERIFY(frames.wait(30000));
    // The newest source settles on its own position...
    QElapsedTimer timer;
    timer.start();
    bool settled = false;
    while (!settled && timer.elapsed() < 3000) {
        if (!frames.wait(100))
            continue;
        for (const auto &row : frames)
            settled |= qAbs(row.at(2).toLongLong() - secondTarget) <= 100;
    }
    QVERIFY2(settled, "the newly engaged position was never delivered");
    // ...and every delivery belongs to the new video/revision, never the old.
    for (const auto &row : frames) {
        QCOMPARE(row.at(0).toLongLong(), 20);
        QCOMPARE(row.at(1).toLongLong(), 7);
        const qint64 pts = row.at(2).toLongLong();
        QVERIFY(pts >= 0 && pts <= m_durationMs);
    }
}

void TestHoverSession::disengageClearsFrameAndStopsDelivery()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");

    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(30, 1, m_video, m_durationMs, atFraction(m_durationMs, 0.4));
    QVERIFY(frames.wait(30000));
    frames.clear();

    session.disengage();
    QVERIFY(session.lastFrame().isNull());
    QCOMPARE(session.lastFramePtsMs(), qint64(-1));
    QTest::qWait(1000);
    QVERIFY(frames.isEmpty());
}

void TestHoverSession::disengageUnloadsSource()
{
#ifndef Q_OS_LINUX
    QSKIP("/proc fd inspection is Linux-only");
#else
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(40, 1, m_video, m_durationMs, atFraction(m_durationMs, 0.4));
    QVERIFY(frames.wait(30000));
    auto *mpv = session.findChild<QProcess *>();
    QVERIFY(mpv);
    const qint64 pid = mpv->processId();
    const QString source = QFileInfo(m_video).canonicalFilePath();

    session.disengage();
    QTest::qWait(300);
    const QFileInfoList fds = QDir(QStringLiteral("/proc/%1/fd").arg(pid))
                                  .entryInfoList(QDir::NoDotAndDotDot | QDir::System);
    QCOMPARE(mpv->state(), QProcess::Running);
    QVERIFY(!fds.isEmpty()); // A dead child must not make this check pass.
    for (const QFileInfo &fd : fds) {
        const QString target = fd.symLinkTarget();
        QVERIFY2(target != source && target != source + QStringLiteral(" (deleted)"),
                 qPrintable(QStringLiteral("source still open: %1").arg(target)));
    }
#endif
}

void TestHoverSession::cachedOnlyModeNeverEngages()
{
    // Cached-only mode never touches the original file, so this case does not
    // need the corpus: a path that does not exist is enough to prove the
    // session stays on the cached-preview path.
    const QString path = m_video.isEmpty()
                             ? QStringLiteral("/nonexistent/scrubtub-not-a-video.mp4")
                             : m_video;
    HoverSession session;
    session.setEnabled(false);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(50, 1, path, m_durationMs > 0 ? m_durationMs : 667500);
    QTest::qWait(3000);
    QVERIFY(frames.isEmpty());
    QCOMPARE(session.status(), QStringLiteral("unavailable"));
}

void TestHoverSession::warmSeekLatency()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable; live hover previews cannot run");

    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(60, 1, m_video, m_durationMs);
    QVERIFY(frames.wait(5000));
    QTest::qWait(200); // finish the initial refinement before timing motion

    QElapsedTimer elapsed;
    qint64 target = -1, firstMs = -1, exactMs = -1;
    connect(&session, &HoverSession::frameReady, this,
            [&](qint64, qint64, qint64 pts, const QImage&) {
        if (firstMs < 0)
            firstMs = elapsed.elapsed();
        if (qAbs(pts - target) <= 100)
            exactMs = elapsed.elapsed();
    });
    for (const double fraction : {0.15, 0.45, 0.9, 0.5, 0.75}) {
        target = atFraction(m_durationMs, fraction);
        firstMs = exactMs = -1;
        elapsed.start();
        session.scrub(target);
        QTRY_VERIFY_WITH_TIMEOUT(exactMs >= 0, 2000);
        qDebug() << "target" << target << "preview_ms" << firstMs << "exact_ms" << exactMs;
        QVERIFY2(firstMs < 200, "warm motion preview missed the 200 ms budget");
    }
}

void TestHoverSession::missingMpvUsesCachedPreviews_data()
{
    QTest::addColumn<QString>("mpvPath");
    QTest::newRow("disabled") << QString();
    QTest::newRow("missing") << m_video + QStringLiteral(".not-an-executable");
}

void TestHoverSession::missingMpvUsesCachedPreviews()
{
    REQUIRE_REAL_MEDIA_OR_SKIP();
    QFETCH(QString, mpvPath);
    HoverSession session;
    session.setMpvPath(mpvPath);
    session.engage(70, 1, m_video, m_durationMs, atFraction(m_durationMs, 0.5));
    QTRY_COMPARE_WITH_TIMEOUT(session.status(), QStringLiteral("unavailable"), 5000);
    QVERIFY(session.lastFrame().isNull());
    QCOMPARE(session.lastFramePtsMs(), qint64(-1));
    session.disengage();
    QVERIFY(session.lastFrame().isNull());
}

QTEST_MAIN(TestHoverSession)
#include "tst_hoversession.moc"

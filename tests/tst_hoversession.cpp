// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include "media/HoverSession.h"

using namespace scrubtub;

class TestHoverSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
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
    QString m_fixture;
    QString m_mpv;
};

void TestHoverSession::initTestCase()
{
#ifdef SCRUBTUB_DEFAULT_SOURCE_FIXTURE
    const QByteArray env = qgetenv("SCRUBTUB_TEST_SOURCE_VIDEO");
    m_fixture = env.isEmpty() ? QStringLiteral(SCRUBTUB_DEFAULT_SOURCE_FIXTURE)
                              : QString::fromLocal8Bit(env);
    if (m_fixture.isEmpty() || !QFileInfo::exists(m_fixture))
        QSKIP("Source fixture unavailable");
#else
    QSKIP("No fixture compiled in");
#endif
    m_mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"));
}

void TestHoverSession::keyframeThenExactSeekUsesActualPts()
{
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);

    QElapsedTimer latency;
    latency.start();
    session.engage(1, 1, m_fixture, 667500, 123456);
    QVERIFY2(frames.wait(30000), "no keyframe preview");
    const qint64 firstPts = frames.first().at(2).toLongLong();
    const qint64 firstLatency = latency.elapsed();
    QVERIFY2(firstPts <= 123356, "preview did not use an earlier keyframe PTS");

    bool settled = false;
    qint64 finalPts = -1;
    while (!settled && latency.elapsed() < 2000) {
        if (!frames.wait(100))
            continue;
        for (const auto &row : frames) {
            const qint64 pts = row.at(2).toLongLong();
            if (qAbs(pts - 123456) <= 100) {
                settled = true;
                finalPts = pts;
            }
        }
    }
    QVERIFY2(settled, "exact seek did not settle near target");
    QVERIFY2(finalPts != 123456, "delivery reported the requested PTS");
    qDebug() << "first-preview/final latency" << firstLatency << latency.elapsed() << "ms";
}

void TestHoverSession::continuousMotionMakesProgressAndSettles()
{
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(2, 1, m_fixture, 667500, 60000);
    QVERIFY(frames.wait(30000));
    QElapsedTimer initialTimer;
    initialTimer.start();
    while (qAbs(session.lastFramePtsMs() - 60000) > 100 && initialTimer.elapsed() < 2000)
        QTest::qWait(50);
    QVERIFY2(qAbs(session.lastFramePtsMs() - 60000) <= 100,
             "initial position did not settle");
    frames.clear();

    const qint64 newest = 60000 + 9 * 50000;
    bool progressedDuringMotion = false;
    for (int i = 0; i < 10; ++i) {
        session.scrub(60000 + i * 50000);
        QTest::qWait(60);
        progressedDuringMotion |= !frames.isEmpty();
    }
    QVERIFY2(progressedDuringMotion, "no frame arrived during continuous motion");

    QElapsedTimer timer;
    timer.start();
    bool settled = false;
    while (!settled && timer.elapsed() < 2000) {
        if (!frames.wait(100))
            continue;
        for (const auto &row : frames)
            settled |= qAbs(row.at(2).toLongLong() - newest) <= 100;
    }
    QVERIFY2(settled, "newest target did not settle within 2 seconds");
    QVERIFY2(frames.size() <= 12,
             qPrintable(QStringLiteral("%1 deliveries for 10 motion targets").arg(frames.size())));
}

void TestHoverSession::initialPositionAndProcessPersistAcrossDisengage()
{
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);

    session.engage(3, 1, m_fixture, 667500, 123456);
    QVERIFY(frames.wait(30000));
    QVERIFY(qAbs(frames.first().at(2).toLongLong() - 123456) < 20000);
    QVERIFY(frames.first().at(2).toLongLong() > 1000);
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

    session.disengage();
    QTest::qWait(100);
    frames.clear();
    session.engage(3, 2, m_fixture, 667500, 123460);
    QVERIFY(frames.wait(30000));
    auto *second = session.findChild<QProcess *>();
    QVERIFY(second);
    QCOMPARE(second->processId(), pid);
}

void TestHoverSession::switchingVideosDropsStaleFrames()
{
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);

    session.engage(10, 1, m_fixture, 667500, 100000);
    QVERIFY(frames.wait(30000));
    frames.clear();
    session.engage(20, 7, m_fixture, 667500, 200000);
    QVERIFY(frames.wait(30000));
    for (const auto &row : frames) {
        QCOMPARE(row.at(0).toLongLong(), 20);
        QCOMPARE(row.at(1).toLongLong(), 7);
        QVERIFY(qAbs(row.at(2).toLongLong() - 200000) < 20000);
    }
}

void TestHoverSession::disengageClearsFrameAndStopsDelivery()
{
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(30, 1, m_fixture, 667500, 100000);
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
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(40, 1, m_fixture, 667500, 100000);
    QVERIFY(frames.wait(30000));
    auto *mpv = session.findChild<QProcess *>();
    QVERIFY(mpv);
    const qint64 pid = mpv->processId();
    const QString source = QFileInfo(m_fixture).canonicalFilePath();

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
    HoverSession session;
    session.setEnabled(false);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(50, 1, m_fixture, 667500);
    QTest::qWait(3000);
    QVERIFY(frames.isEmpty());
    QCOMPARE(session.status(), QStringLiteral("unavailable"));
}

void TestHoverSession::warmSeekLatency()
{
    if (m_mpv.isEmpty())
        QSKIP("mpv unavailable");
    HoverSession session;
    session.setMpvPath(m_mpv);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(60, 1, m_fixture, 667500);
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
    for (const qint64 requested : {10000, 300000, 650000, 123456, 500000}) {
        target = requested;
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
    QTest::newRow("missing") << m_fixture + QStringLiteral(".not-an-executable");
}

void TestHoverSession::missingMpvUsesCachedPreviews()
{
    QFETCH(QString, mpvPath);
    HoverSession session;
    session.setMpvPath(mpvPath);
    session.engage(70, 1, m_fixture, 667500, 123456);
    QTRY_COMPARE_WITH_TIMEOUT(session.status(), QStringLiteral("unavailable"), 5000);
    QVERIFY(session.lastFrame().isNull());
    QCOMPARE(session.lastFramePtsMs(), -1);
    session.disengage();
    QVERIFY(session.lastFrame().isNull());
}

QTEST_MAIN(TestHoverSession)
#include "tst_hoversession.moc"

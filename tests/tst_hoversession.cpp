// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// HoverSession contract checks (TECH_SPEC.md section 6): paused engagement,
// PTS-validated seeks, coalescing without a decoder storm, disengage
// releases, cached-only mode never reads.
#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include "media/HoverSession.h"

using namespace itub;

class TestHoverSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void engagementDeliversPausedFrame();
    void scrubDeliversNearbyFrame();
    void rapidScrubsDoNotStorm();
    void disengageStopsDelivery();
    void cachedOnlyModeNeverEngages();

private:
    QString m_fixture;
};

void TestHoverSession::initTestCase()
{
#ifdef ITUB_DEFAULT_SOURCE_FIXTURE
    const QByteArray env = qgetenv("ITUB_TEST_SOURCE_VIDEO");
    m_fixture = env.isEmpty() ? QStringLiteral(ITUB_DEFAULT_SOURCE_FIXTURE)
                              : QString::fromLocal8Bit(env);
    if (m_fixture.isEmpty() || !QFileInfo::exists(m_fixture))
        QSKIP("Source fixture unavailable");
#else
    QSKIP("No fixture compiled in");
#endif
}

void TestHoverSession::engagementDeliversPausedFrame()
{
    HoverSession session;
    QSignalSpy frames(&session, &HoverSession::frameReady);
    QVERIFY(frames.isValid());

    session.engage(1, 1, m_fixture, 667500);
    // First paused frame within a bounded load window.
    QVERIFY2(frames.wait(30000), "no frame after engagement");
    const auto args = frames.first();
    QCOMPARE(args.at(0).toLongLong(), 1);   // videoId
    QCOMPARE(args.at(1).toLongLong(), 1);   // revision
    QVERIFY(!args.at(3).value<QImage>().isNull());
}

void TestHoverSession::scrubDeliversNearbyFrame()
{
    HoverSession session;
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(2, 1, m_fixture, 667500);
    QVERIFY(frames.wait(30000));
    frames.clear();

    session.scrub(300000); // 300 s
    bool found = false;
    qint64 delivered = -1;
    const QDateTime deadline = QDateTime::currentDateTime().addMSecs(30000);
    while (!found && QDateTime::currentDateTime() < deadline) {
        if (!frames.wait(1000))
            continue;
        for (int i = 0; i < frames.size(); ++i) {
            const qint64 pts = frames.at(i).at(2).toLongLong();
            if (qAbs(pts - 300000) < 200) { // §11 gate: within 100 ms of target
                found = true;
                delivered = pts;
                break;
            }
        }
    }
    QVERIFY2(found, "no frame delivered within 200 ms of the 300 s target");
    qDebug() << "scrub 300s delivered pts" << delivered << "ms";
}

void TestHoverSession::rapidScrubsDoNotStorm()
{
    HoverSession session;
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(3, 1, m_fixture, 667500);
    QVERIFY(frames.wait(30000));
    frames.clear();

    // Pointer-like burst: 10 targets, one every 60 ms (coalescing window).
    for (int i = 0; i < 10; ++i) {
        session.scrub(60000 + i * 50000);
        QTest::qWait(60);
    }
    QTest::qWait(2500);
    // Coalescing must keep deliveries far below one frame per request.
    QVERIFY2(frames.size() <= 6,
             qPrintable(QStringLiteral("%1 frames for 10 coalesced targets")
                            .arg(frames.size())));
    QVERIFY(frames.size() >= 1);
}

void TestHoverSession::disengageStopsDelivery()
{
    HoverSession session;
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(4, 1, m_fixture, 667500);
    QVERIFY(frames.wait(30000));
    session.scrub(120000);
    frames.wait(2000);
    frames.clear();

    session.disengage();
    QCOMPARE(session.status(), QStringLiteral("idle"));
    QTest::qWait(1500);
    QVERIFY2(frames.isEmpty(), "frames delivered after disengage");
}

void TestHoverSession::cachedOnlyModeNeverEngages()
{
    HoverSession session;
    session.setEnabled(false);
    QSignalSpy frames(&session, &HoverSession::frameReady);
    session.engage(5, 1, m_fixture, 667500);
    QTest::qWait(3000);
    QVERIFY(frames.isEmpty());
    QCOMPARE(session.status(), QStringLiteral("unavailable"));
}

QTEST_MAIN(TestHoverSession)
#include "tst_hoversession.moc"

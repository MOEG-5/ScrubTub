// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
// PreviewProvider contract checks: async responses, cancellation, bounded
// decode size, missing-artifact behavior. Posters and atlases are produced by
// the real extractor from the repository's vids/ corpus, read as-is
// (AGENTS.md: read-only).
#include <QGuiApplication>
#include <QImage>
#include <QColor>
#include <QQuickImageResponse>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QJSValue>
#include <QQmlProperty>
#include <QWheelEvent>
#include <QPainter>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "catalogue/Catalogue.h"
#include "catalogue/CatalogueModel.h"
#include "catalogue/CatalogueProxy.h"
#include "media/Extract.h"
#include "media/HoverFrameItem.h"
#include "media/HoverSession.h"
#include "media/PreviewProvider.h"
#include "media/Probe.h"
#include "testsupport/MediaFixtures.h"

using namespace scrubtub;
using namespace scrubtub::testsupport;

class StubCatalogue : public QObject {
    Q_OBJECT
    Q_PROPERTY(int currentIndex MEMBER currentIndex CONSTANT)
    Q_PROPERTY(bool moving MEMBER moving NOTIFY movingChanged)
    Q_PROPERTY(bool cachedTimelineEnabled MEMBER cachedTimelineEnabled CONSTANT)
public:
    int currentIndex = 0;
    bool moving = false;
    bool cachedTimelineEnabled = true;
public slots:
    void requestSampleTimes(qint64 id) { emit sampleTimesReady(id, {0, 1000}); }
    void requestStoryboard(qint64) {}
    void hoverEngage(qint64 id) { emit hoverSourceReady(id, 1, {}, 1000); }

signals:
    void movingChanged();
    void sampleTimesReady(qint64, QVariantList);
    void hoverSourceReady(qint64, qint64, QString, qint64);
    void cacheEntryChanged(qint64, QString);
};

class TestPreviewProvider : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void deliversExistingArtifact();
    void missingArtifactFinishesEmpty();
    void decodesArtifactAtRequestedSize();
    void cancelledResponseStillFinishes();
    void atlasHoverSelectsOneTile();
    void mainWindowLoadsAndMapsRatingFilters();

private:
    QTemporaryDir m_cache;
    QString m_poster;      // real poster extracted from the corpus
    QSize m_posterSize;
    QString m_atlas;       // real storyboard atlas extracted from the corpus
    QString m_fingerprint;
};

void TestPreviewProvider::initTestCase()
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    // Read-only guard: the corpus must be byte-identical afterwards.
    m_fingerprint = vidsFingerprint();

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty())
        return;
    QString video = smallestVideo(QStringLiteral("mp4"));
    if (video.isEmpty())
        video = smallestVideo();
    if (video.isEmpty())
        return;
    const ProbeResult probe = Probe::run(ffprobe, video);
    if (!probe.ok)
        return;

    // The provider decodes what the extractor really wrote, not a painted
    // stand-in: poster first (always available), atlas when the clip is long
    // enough for a sample plan.
    PosterRequest poster;
    poster.ffmpegPath = ffmpeg;
    poster.sourcePath = video;
    poster.outputPath = m_cache.filePath(QStringLiteral("7-2-poster-320-v1.jpg"));
    poster.durationMs = probe.durationMs;
    poster.selectedStreamIndex = probe.selectedStreamIndex;
    const ExtractResult posterResult = Extract::poster(poster);
    if (posterResult.ok) {
        m_poster = posterResult.outputPath;
        m_posterSize = posterResult.pixelSize;
    }

    StoryboardRequest storyboard;
    storyboard.ffmpegPath = ffmpeg;
    storyboard.sourcePath = video;
    storyboard.outputDir = m_cache.filePath(QStringLiteral("atlas-tmp"));
    storyboard.outputPath = m_cache.filePath(QStringLiteral("7-2-atlas-240-5-v2.jpg"));
    storyboard.durationMs = probe.durationMs;
    storyboard.selectedStreamIndex = probe.selectedStreamIndex;
    const ExtractResult storyboardResult = Extract::storyboard(storyboard);
    if (storyboardResult.ok)
        m_atlas = storyboardResult.outputPath;
}

void TestPreviewProvider::cleanupTestCase()
{
    QCOMPARE(vidsFingerprint(), m_fingerprint);
}

void TestPreviewProvider::deliversExistingArtifact()
{
    if (m_poster.isEmpty())
        QSKIP("vids/ corpus unavailable or poster extraction failed "
              "(set SCRUBTUB_TEST_VIDS_DIR)");

    PreviewProvider provider([this](qint64, qint64, bool) { return m_poster; });
    // No requested size: the artifact is delivered at its stored size.
    QQuickImageResponse* response = provider.requestImageResponse(
        QStringLiteral("poster/7-2"), QSize());
    QVERIFY(response);
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    QVERIFY2(spy.wait(10000), "provider response did not finish");
    QQuickTextureFactory* texture = response->textureFactory();
    QVERIFY(texture);
    QVERIFY(!m_posterSize.isEmpty());
    QCOMPARE(texture->textureSize(), m_posterSize);
    delete response;
}

void TestPreviewProvider::missingArtifactFinishesEmpty()
{
    PreviewProvider provider([](qint64, qint64, bool) { return QString(); });
    QQuickImageResponse* response = provider.requestImageResponse(
        QStringLiteral("poster/9-1"), QSize());
    QVERIFY(response);
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    QVERIFY2(spy.wait(10000), "response did not finish for a missing artifact");
    QVERIFY(!response->textureFactory()
            || response->textureFactory()->textureSize().isEmpty());
    delete response;
}

void TestPreviewProvider::decodesArtifactAtRequestedSize()
{
    if (m_poster.isEmpty())
        QSKIP("vids/ corpus unavailable or poster extraction failed "
              "(set SCRUBTUB_TEST_VIDS_DIR)");

    PreviewProvider provider([this](qint64, qint64, bool) { return m_poster; });
    const QSize requested(240, 135);
    QQuickImageResponse* response = provider.requestImageResponse(
        QStringLiteral("poster/7-2"), requested);
    QVERIFY(response);
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    QVERIFY(spy.wait(10000));
    QQuickTextureFactory* texture = response->textureFactory();
    QVERIFY(texture);
    // Decoded RAM is bounded to the requested card size (§6) and the stored
    // aspect ratio survives the downscale.
    QCOMPARE(texture->textureSize().width(), requested.width());
    QVERIFY(texture->textureSize().height() > 0);
    QVERIFY(texture->textureSize().height() <= m_posterSize.height());
    const double decodedAspect = static_cast<double>(texture->textureSize().width())
                                 / texture->textureSize().height();
    const double storedAspect = static_cast<double>(m_posterSize.width())
                                / m_posterSize.height();
    QVERIFY(qAbs(decodedAspect - storedAspect) < 0.02);
    delete response;
}

void TestPreviewProvider::cancelledResponseStillFinishes()
{
    if (m_atlas.isEmpty())
        QSKIP("vids/ corpus unavailable or storyboard extraction failed "
              "(set SCRUBTUB_TEST_VIDS_DIR)");

    PreviewProvider provider([this](qint64, qint64, bool) { return m_atlas; });
    QQuickImageResponse* response = provider.requestImageResponse(
        QStringLiteral("atlas/7-2"), QSize(240, 90));
    QVERIFY(response);
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    // A response cancelled before its decode settles (the card scrolled away)
    // must still deliver finished(), or QML waits on it forever.
    response->cancel();
    QVERIFY2(spy.wait(10000), "cancelled response did not finish");
    if (QQuickTextureFactory* texture = response->textureFactory()) {
        QVERIFY(texture->textureSize().width() <= 240);
        QVERIFY(texture->textureSize().height() > 0);
    }
    delete response;
}

void TestPreviewProvider::atlasHoverSelectsOneTile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage atlas(360, 60, QImage::Format_RGB32);
    const QColor colors[] = {Qt::red, Qt::green, Qt::blue,
                             Qt::yellow, Qt::magenta, Qt::cyan};
    QPainter painter(&atlas);
    for (int tile = 0; tile < 6; ++tile)
        painter.fillRect(tile * 60, 0, 60, 60, colors[tile]);
    painter.end();
    const QString atlasPath = dir.filePath(QStringLiteral("atlas.png"));
    QVERIFY(atlas.save(atlasPath));

    const QString posterPath = dir.filePath(QStringLiteral("poster.png"));
    QImage poster(320, 180, QImage::Format_RGB32);
    poster.fill(Qt::magenta);
    QVERIFY(poster.save(posterPath));

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("previews"),
        new PreviewProvider([atlasPath, posterPath](qint64, qint64, bool isPoster) {
            return isPoster ? posterPath : atlasPath;
        }));
    qmlRegisterType<HoverFrameItem>("scrubtub.media", 1, 0, "HoverFrameItem");

    StubCatalogue catalogue;
    HoverSession hoverSession;
    hoverSession.setEnabled(false);
    engine.rootContext()->setContextProperty(QStringLiteral("catalogue"), &catalogue);
    engine.rootContext()->setContextProperty(QStringLiteral("window"), &catalogue);
    engine.rootContext()->setContextProperty(QStringLiteral("grid"), &catalogue);
    engine.rootContext()->setContextProperty(QStringLiteral("hoverSession"), &hoverSession);

    QQmlComponent component(&engine, QFINDTESTDATA("../src/ui/VideoCard.qml"));
    QVERIFY2(component.status() == QQmlComponent::Ready,
             qPrintable(component.errorString()));
    QVariantMap properties;
    properties.insert(QStringLiteral("videoId"), 1);
    properties.insert(QStringLiteral("name"), QStringLiteral("atlas"));
    properties.insert(QStringLiteral("path"), QString());
    properties.insert(QStringLiteral("sizeBytes"), 1.0);
    properties.insert(QStringLiteral("sizeText"), QStringLiteral("1 B"));
    properties.insert(QStringLiteral("durationMs"), 1000.0);
    properties.insert(QStringLiteral("durationText"), QStringLiteral("0:01"));
    properties.insert(QStringLiteral("displayWidth"), 1920);
    properties.insert(QStringLiteral("displayHeight"), 1080);
    properties.insert(QStringLiteral("codec"), QStringLiteral("test"));
    properties.insert(QStringLiteral("rating"), 0);
    properties.insert(QStringLiteral("views"), 0.0);
    properties.insert(QStringLiteral("availability"), QStringLiteral("available"));
    properties.insert(QStringLiteral("probeStatus"), QStringLiteral("ready"));
    properties.insert(QStringLiteral("probeError"), QString());
    properties.insert(QStringLiteral("revision"), 1);
    properties.insert(QStringLiteral("posterSource"), QStringLiteral("image://previews/poster/1-1"));
    properties.insert(QStringLiteral("atlasSource"), QStringLiteral("image://previews/atlas/1-1"));
    properties.insert(QStringLiteral("index"), 0);
    auto *card = qobject_cast<QQuickItem *>(component.createWithInitialProperties(properties));
    QVERIFY2(card, qPrintable(component.errorString()));

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(600, 300);
    // VideoCard's unqualified `window` resolves to the harness's `window`
    // context property (StubCatalogue), which therefore declares
    // `cachedTimelineEnabled` like Main.qml's ApplicationWindow does.
    card->setParentItem(window.contentItem());
    card->setSize(QSizeF(600, 300));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QTest::mouseMove(&window, QPoint(20, 130));
    QTRY_VERIFY_WITH_TIMEOUT(card->property("atlasReady").toBool(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window.grabWindow().pixelColor(300, 130), QColor(Qt::red), 3000);

    QTest::mouseMove(&window, QPoint(580, 130));
    QTRY_COMPARE_WITH_TIMEOUT(window.grabWindow().pixelColor(300, 130), QColor(Qt::green), 3000);
    catalogue.setProperty("moving", true);
    QTRY_VERIFY(!card->property("hoverActive").toBool());
    QTRY_COMPARE(window.grabWindow().pixelColor(300, 130), QColor(Qt::magenta));
    catalogue.setProperty("moving", false);
    QTRY_VERIFY(card->property("hoverActive").toBool());
    window.resize(700, 400);
    QTest::mouseMove(&window, QPoint(650, 350));
    QTRY_VERIFY(!card->property("hoverActive").toBool());
    QTRY_COMPARE(window.grabWindow().pixelColor(300, 130), QColor(Qt::magenta));
    card->setWidth(400);
    QTRY_COMPARE(window.grabWindow().pixelColor(200, 130), QColor(Qt::magenta));
    delete card;
}

void TestPreviewProvider::mainWindowLoadsAndMapsRatingFilters()
{
    QTemporaryDir profile;
    Catalogue catalogue;
    QString error;
    QVERIFY2(catalogue.initialize(profile.path(), QString(), QString(), &error), qPrintable(error));
    CatalogueModel model;
    CatalogueProxy proxy(&catalogue, &model);
    HoverSession hover;
    hover.setEnabled(false);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("catalogue"), &proxy);
    engine.rootContext()->setContextProperty(QStringLiteral("catalogueModel"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("hoverSession"), &hover);
    qmlRegisterType<HoverFrameItem>("scrubtub.media", 1, 0, "HoverFrameItem");
    QQmlComponent component(&engine, QFINDTESTDATA("../src/ui/Main.qml"));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QCOMPARE(window->property("title").toString(), QStringLiteral("ScrubTub"));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "selectLibrary",
        Q_ARG(QVariant, QStringLiteral("rated")), Q_ARG(QVariant, -1), Q_ARG(QVariant, QString())));
    QCOMPARE(window->property("libraryTitle").toString(), QStringLiteral("Rated videos"));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "selectLibrary",
        Q_ARG(QVariant, QStringLiteral("all")), Q_ARG(QVariant, 7), Q_ARG(QVariant, QStringLiteral("Clips"))));
    QCOMPARE(window->property("selectedRoot").toInt(), 7);
    QCOMPARE(window->property("libraryTitle").toString(), QStringLiteral("Clips"));
    window->setProperty("width", 960);
    window->setProperty("height", 720);
    window->setProperty("detailsVideoId", 1);
    auto *grid = window->findChild<QQuickItem *>(QStringLiteral("videoGrid"));
    QVERIFY(grid);
    QTRY_VERIFY(grid->width() > 420);
    QTRY_VERIFY(grid->property("cellWidth").toDouble() <= grid->width() / 2);
    // Drain navigation's debounced search before supplying two catalogue rows.
    QTest::qWait(100);
    VideoRow first;
    first.id = 1; first.fileName = QStringLiteral("First clip");
    VideoRow second;
    second.id = 2; second.fileName = QStringLiteral("Second clip");
    model.applyRows({first, second}, true);
    QVERIFY(grid->activeFocusOnTab());
    grid->forceActiveFocus();
    QTRY_COMPARE(window->property("detailsVideoId").toInt(), 1);
    QTest::keyClick(qobject_cast<QQuickWindow *>(window.get()), Qt::Key_Right);
    QTRY_COMPARE(window->property("detailsVideoId").toInt(), 2);
    QCOMPARE(window->property("detailsName").toString(), QStringLiteral("Second clip"));
    auto *panel = window->findChild<QQuickItem *>(QStringLiteral("detailsPanel"));
    QVERIFY(panel);
    QTRY_VERIFY(panel->y() >= grid->y() + grid->height());
    QTRY_VERIFY(panel->width() >= grid->width());
    const auto spec = [&]() {
        QVariant result;
        QMetaObject::invokeMethod(window.get(), "querySpec", Q_RETURN_ARG(QVariant, result));
        return result.value<QJSValue>().toVariant().toMap();
    };
    auto *duration = window->findChild<QObject *>(QStringLiteral("durationFilter"));
    auto *size = window->findChild<QObject *>(QStringLiteral("sizeFilter"));
    auto *rating = window->findChild<QObject *>(QStringLiteral("ratingFilter"));
    QVERIFY(duration && size && rating);
    duration->setProperty("minimumText", QStringLiteral("10.5"));
    duration->setProperty("maximumText", QStringLiteral("60"));
    size->setProperty("maximumText", QStringLiteral("4"));
    rating->setProperty("minimumText", QStringLiteral("3"));
    rating->setProperty("maximumText", QStringLiteral("4"));
    auto *include = window->findChild<QObject *>(QStringLiteral("includeTagsFilter"));
    QVERIFY(include);
    include->setProperty("text", QStringLiteral(" Travel , codec:h264, "));
    QCOMPARE(spec().value(QStringLiteral("durationMinMs")).toInt(), 10500);
    QCOMPARE(spec().value(QStringLiteral("durationMaxMs")).toInt(), 60000);
    QCOMPARE(spec().value(QStringLiteral("sizeMax")).toInt(), 4 * 1048576);
    QCOMPARE(spec().value(QStringLiteral("ratingMin")).toInt(), 3);
    QCOMPARE(spec().value(QStringLiteral("ratingMax")).toInt(), 4);
    QCOMPARE(spec().value(QStringLiteral("includeAllTags")).toStringList(), QStringList({QStringLiteral("Travel"), QStringLiteral("codec:h264")}));
    duration->setProperty("minimumText", QStringLiteral("70"));
    QVERIFY(!duration->property("valid").toBool());
    QVERIFY(spec().isEmpty());
    duration->setProperty("minimumText", QStringLiteral("-5"));
    QVERIFY(!duration->property("valid").toBool());
    rating->setProperty("minimumText", QStringLiteral("2.5"));
    QVERIFY(!rating->property("valid").toBool());
    window->setProperty("filtersReset", window->property("filtersReset").toInt() + 1);
    QVERIFY(duration->property("valid").toBool());
    QVERIFY(!spec().contains(QStringLiteral("durationMaxMs")));
    QVERIFY(!spec().contains(QStringLiteral("ratingMax")));
    QVERIFY(spec().value(QStringLiteral("includeAllTags")).toStringList().isEmpty());
    // Editing a precise bound also updates the native slider; keyboard moves it.
    auto *rangeSlider = duration->findChild<QQuickItem *>(QStringLiteral("rangeSlider"));
    QVERIFY(rangeSlider);
    rangeSlider->forceActiveFocus();
    QTest::keyClick(qobject_cast<QQuickWindow *>(window.get()), Qt::Key_Right);
    QTRY_COMPARE(duration->property("minimum").toInt(), 1);
    auto *filterScroll = window->findChild<QQuickItem *>(QStringLiteral("filterScroll"));
    auto *ratingMinimum = rating->findChild<QQuickItem *>(QStringLiteral("minimumField"));
    QVERIFY(filterScroll && ratingMinimum);
    ratingMinimum->forceActiveFocus();
    QTRY_VERIFY(ratingMinimum->mapToItem(filterScroll, QPointF()).y() + ratingMinimum->height()
                <= filterScroll->height());
    QTest::keyClick(qobject_cast<QQuickWindow *>(window.get()), Qt::Key_F, Qt::ControlModifier);
    auto *search = window->findChild<QQuickItem *>(QStringLiteral("catalogueSearch"));
    QVERIFY(search);
    QTRY_VERIFY(search->hasActiveFocus());
    QTRY_VERIFY(search->mapToItem(filterScroll, QPointF()).y() >= 0);

    auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebar"));
    auto *tabs = window->findChild<QQuickItem *>(QStringLiteral("detailsTabs"));
    QVERIFY(sidebar && tabs);
    window->setProperty("detailsVideoId", -1);
    QTRY_VERIFY(!tabs->isVisible());
    QTRY_VERIFY(!panel->isVisible());
    QCOMPARE(grid->y(), 0.0); // no catalogue header
    QVERIFY(QQmlProperty(sidebar, QStringLiteral("SplitView.preferredWidth"), qmlContext(sidebar)).write(330));
    QTRY_COMPARE(sidebar->width(), 330.0);
    sidebar->setVisible(false);
    QTRY_COMPARE(grid->width(), window->property("width").toDouble());
    QTest::keyClick(qobject_cast<QQuickWindow *>(window.get()), Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(sidebar->isVisible());
    QTRY_COMPARE(sidebar->width(), 330.0);
    sidebar->setVisible(false);
    // Move focus away so Ctrl+F must reopen and focus the sidebar.
    grid->forceActiveFocus();
    QTest::keyClick(qobject_cast<QQuickWindow *>(window.get()), Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(sidebar->isVisible());
    QTRY_VERIFY(search->hasActiveFocus());

    QTest::qWait(100); // finish any pending filter query before the scrolling corpus
    QList<VideoRow> rows;
    for (int i = 1; i <= 80; ++i) { VideoRow row; row.id = i; row.fileName = QStringLiteral("Clip %1").arg(i); rows.append(row); }
    model.applyRows(rows, true);
    window->setProperty("detailsVideoId", -1);
    QMetaObject::invokeMethod(grid, "positionViewAtBeginning");
    QTest::qWait(30);
    const qreal startY = grid->property("contentY").toDouble();
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    const QPointF wheelPosition = grid->mapToScene(QPointF(grid->width() / 2, 120));
    const auto wheel = [&](int delta) {
        QWheelEvent event(wheelPosition, quickWindow->mapToGlobal(wheelPosition.toPoint()), {}, QPoint(0, delta),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(quickWindow, &event);
    };
    wheel(-120);
    QTRY_VERIFY(grid->property("contentY").toDouble() > startY);
    QVERIFY(grid->property("flicking").toBool());
    wheel(-120); // repeated wheel input carries momentum forward
    QTRY_VERIFY(grid->property("contentY").toDouble() > startY + 40);
    QTRY_VERIFY_WITH_TIMEOUT(!grid->property("moving").toBool(), 5000);
    const qreal downY = grid->property("contentY").toDouble();
    wheel(120);
    QTRY_VERIFY(grid->property("contentY").toDouble() < downY);
    QMetaObject::invokeMethod(grid, "cancelFlick");

    const qreal beforeSmooth = grid->property("contentY").toDouble();
    const auto smoothWheel = [&](Qt::ScrollPhase phase, QPoint pixels, QPoint angles = {}) {
        QWheelEvent event(wheelPosition, quickWindow->mapToGlobal(wheelPosition.toPoint()), pixels, angles,
                          Qt::NoButton, Qt::NoModifier, phase, false, Qt::MouseEventSynthesizedBySystem);
        QCoreApplication::sendEvent(quickWindow, &event);
        QTest::qWait(20);
    };
    smoothWheel(Qt::ScrollBegin, {});
    smoothWheel(Qt::ScrollUpdate, QPoint(0, -60), QPoint(0, -120));
    smoothWheel(Qt::ScrollUpdate, QPoint(0, -60), QPoint(0, -120));
    smoothWheel(Qt::ScrollMomentum, QPoint(0, -30), QPoint(0, -60));
    smoothWheel(Qt::ScrollEnd, {});
    QTRY_VERIFY(grid->property("contentY").toDouble() > beforeSmooth);

    QMetaObject::invokeMethod(grid, "cancelFlick");
    const qreal beforeFineWheel = grid->property("contentY").toDouble();
    wheel(-1); // high-resolution mouse: less than one conventional wheel notch
    QTRY_VERIFY(grid->property("contentY").toDouble() > beforeFineWheel);
    QMetaObject::invokeMethod(grid, "cancelFlick");
    // Mouse wheels may report pixels as well as (or instead of) angle ticks.
    // Both forms must keep coasting after the event, rather than jump once.
    for (const QPoint angles : {QPoint(0, -120), QPoint()}) {
        QMetaObject::invokeMethod(grid, "cancelFlick");
        const qreal beforePixelWheel = grid->property("contentY").toDouble();
        QWheelEvent pixelWheel(wheelPosition, quickWindow->mapToGlobal(wheelPosition.toPoint()),
                              QPoint(0, -40), angles, Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false, Qt::MouseEventNotSynthesized);
        QCoreApplication::sendEvent(quickWindow, &pixelWheel);
        QVERIFY(grid->property("flicking").toBool());
        QTRY_VERIFY(grid->property("contentY").toDouble() > beforePixelWheel);
        const qreal afterInput = grid->property("contentY").toDouble();
        QTest::qWait(80);
        QVERIFY(grid->property("contentY").toDouble() > afterInput + 10);
    }
    QMetaObject::invokeMethod(grid, "cancelFlick");
    // The wheel surface must not intercept thumbnail selection.
    QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier, wheelPosition.toPoint());
    QTRY_VERIFY(window->property("detailsVideoId").toInt() > 0);

}

QTEST_MAIN(TestPreviewProvider)
#include "tst_preview.moc"

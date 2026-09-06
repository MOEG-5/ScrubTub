// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// PreviewProvider contract checks: async responses, bounded decode size,
// missing-artifact behavior.
#include <QGuiApplication>
#include <QImage>
#include <QQuickImageResponse>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "media/PreviewProvider.h"

using namespace itub;

class TestPreviewProvider : public QObject {
    Q_OBJECT

private slots:
    void deliversExistingArtifact();
    void missingArtifactFinishesEmpty();
    void scalesToRequestedSize();
    void qmlImageRendersPixels();

private:
    static QString writePoster(const QTemporaryDir& dir, const QString& name,
                               int width, int height);
};

QString TestPreviewProvider::writePoster(const QTemporaryDir& dir, const QString& name,
                                         int width, int height)
{
    const QString path = dir.filePath(name);
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(0x336699);
    image.save(path, "jpg", 90);
    return path;
}

void TestPreviewProvider::deliversExistingArtifact()
{
    QTemporaryDir dir;
    const QString path = writePoster(dir, QStringLiteral("7-2-poster-320-v1.jpg"), 320, 180);

    PreviewProvider provider([path](qint64, qint64, bool) { return path; });
    // No requested size: the artifact is delivered at its stored size.
    QQuickImageResponse* response = provider.requestImageResponse(
        QStringLiteral("poster/7-2"), QSize());
    QVERIFY(response);
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    QVERIFY2(spy.wait(10000), "provider response did not finish");
    QQuickTextureFactory* texture = response->textureFactory();
    QVERIFY(texture);
    QCOMPARE(texture->textureSize().width(), 320);
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

void TestPreviewProvider::scalesToRequestedSize()
{
    QTemporaryDir dir;
    const QString path = writePoster(dir, QStringLiteral("8-1-poster-320-v1.jpg"), 960, 540);

    PreviewProvider provider([path](qint64, qint64, bool) { return path; });
    QQuickImageResponse* response = provider.requestImageResponse(
        QStringLiteral("poster/8-1"), QSize(240, 135));
    QSignalSpy spy(response, &QQuickImageResponse::finished);
    QVERIFY(spy.wait(10000));
    QQuickTextureFactory* texture = response->textureFactory();
    QVERIFY(texture);
    // Decoded RAM is bounded to the requested card size (§6).
    QCOMPARE(texture->textureSize().width(), 240);
    delete response;
}

void TestPreviewProvider::qmlImageRendersPixels()
{
    QTemporaryDir dir;
    const QString path = writePoster(dir, QStringLiteral("11-1-poster-320-v1.jpg"), 320, 180);
    PreviewProvider provider([path](qint64, qint64, bool) { return path; });

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("previews"), &provider);
    QQmlComponent component(&engine);
    // file:// first: validates that the grab/render path shows ANY image.
    const bool controlCase = qEnvironmentVariableIsSet("ITUB_CONTROL_FILE");
    const QString source = controlCase
        ? QStringLiteral("file://%1").arg(path)
        : QStringLiteral("image://previews/poster/11-1");
    component.setData(QByteArray("import QtQuick\nImage { source: \""
                                 + source.toUtf8() + "\"; sourceSize.width: 224 }"),
                      QUrl());
    QVERIFY2(component.isReady(), qUtf8Printable(component.errorString()));
    auto* item = qobject_cast<QQuickItem*>(component.create());
    QVERIFY(item);

    QQuickWindow window;
    item->setParentItem(window.contentItem());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000)
            || QTest::qWaitForWindowActive(&window, 5000));

    // Wait until the image is ready, then grab actual pixels.
    QTRY_COMPARE(item->property("status").toInt(), 1); // Ready
    const QImage grabbed = window.grabWindow();

    // The painted image must contain non-uniform pixels (poster content).
    quint64 sum = 0, sumSq = 0;
    int samples = 0;
    for (int y = 0; y < grabbed.height(); y += 7) {
        for (int x = 0; x < grabbed.width(); x += 7) {
            const QRgb px = grabbed.pixel(x, y);
            const int gray = qGray(px);
            sum += gray;
            sumSq += static_cast<quint64>(gray) * gray;
            ++samples;
        }
    }
    const double mean = static_cast<double>(sum) / samples;
    const double variance = static_cast<double>(sumSq) / samples - mean * mean;
    QVERIFY2(variance > 4.0,
             qPrintable(QStringLiteral("rendered image looks uniform (variance %1)").arg(variance)));
    delete item;
}

QTEST_MAIN(TestPreviewProvider)
#include "tst_preview.moc"

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// PreviewProvider contract checks: async responses, bounded decode size,
// missing-artifact behavior.
#include <QGuiApplication>
#include <QImage>
#include <QQuickImageResponse>
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

QTEST_MAIN(TestPreviewProvider)
#include "tst_preview.moc"

// SPDX-License-Identifier: GPL-3.0-only
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "app/MediaTools.h"

class TestMediaTools : public QObject {
    Q_OBJECT
private slots:
    void overrideWinsEvenWhenMissing()
    {
        const char* variable = "SCRUBTUB_TEST_TOOL_PATH";
        const QByteArray old = qgetenv(variable);
        const auto restore = qScopeGuard([&] {
            if (old.isNull()) qunsetenv(variable); else qputenv(variable, old);
        });
        qputenv(variable, "/deliberately/missing/tool");
        QCOMPARE(scrubtub::findMediaTool(QStringLiteral("ffmpeg"), variable),
                 QStringLiteral("/deliberately/missing/tool"));
    }
    void bundledToolWinsOverPath()
    {
        const QString directory = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("media"));
        // Own only this uniquely named test executable, never an existing tool.
        QVERIFY(QDir().mkpath(directory));
        QTemporaryDir pathDir;
        QVERIFY(pathDir.isValid());
        QString name = QStringLiteral("scrubtub-resolution-test");
#ifdef Q_OS_WIN
        name += QStringLiteral(".exe");
#endif
        const QString bundledPath = QDir(directory).filePath(name);
        QVERIFY(!QFile::exists(bundledPath));
        const auto cleanup = qScopeGuard([&] { QFile::remove(bundledPath); QDir().rmdir(directory); });
        for (const auto& path : {bundledPath, pathDir.filePath(name)}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("test");
            file.close();
            QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        const QByteArray previous = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", previous); });
        qputenv("PATH", pathDir.path().toLocal8Bit());
        QCOMPARE(scrubtub::findMediaTool(name, "SCRUBTUB_TEST_UNSET_OVERRIDE"), bundledPath);
        QVERIFY(QFile::remove(bundledPath));
        QCOMPARE(scrubtub::findMediaTool(name, "SCRUBTUB_TEST_UNSET_OVERRIDE"), pathDir.filePath(name));
    }
};
QTEST_GUILESS_MAIN(TestMediaTools)
#include "tst_mediatools.moc"

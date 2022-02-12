#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "Decoder.h"
#include "AudioOutput.h"

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QGuiApplication app(argc, argv);

    qmlRegisterType<Decoder>("Decoder", 1, 0, "Decoder");
    qmlRegisterType<AudioOutput>("AudioOutput", 1, 0, "AudioOutput");
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}

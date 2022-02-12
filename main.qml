import QtQuick 2.9
import QtQuick.Window 2.2
import QtMultimedia 5.8
import Decoder 1.0
import AudioOutput 1.0

Window {
    visible: true
    width: 1280
    height: 720
    title: qsTr("PowPlayer")

    Rectangle {
        id: background
        color: "black"
        anchors.fill: parent
    }

    Decoder {
        id: decoder
        videoUrl:qsTr("video.mp4")
    }

    VideoOutput {
        anchors.fill: parent
        source: decoder
    }

    AudioOutput {
        source: decoder
    }

    Connections {
        target: decoder
        onPlayFinished: {
            Qt.quit();
        }
    }
}

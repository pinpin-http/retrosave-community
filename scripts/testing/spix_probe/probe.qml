import QtQuick
import QtQuick.Controls

Window {
    id: root
    objectName: "mainWindow"
    width: 400; height: 520; visible: true
    property string touched: "aucun"

    Column {
        anchors.fill: parent
        spacing: 4
        Item {
            objectName: "cible_taphandler"
            width: 400; height: 80
            Rectangle { anchors.fill: parent; color: "#eeeeee" }
            TapHandler { onTapped: root.touched = "taphandler" }
        }
        Item {
            objectName: "cible_tap_release"
            width: 400; height: 80
            Rectangle { anchors.fill: parent; color: "#e4e4e4" }
            TapHandler {
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: root.touched = "tap_release"
            }
        }
        Item {
            objectName: "cible_tap_within"
            width: 400; height: 80
            Rectangle { anchors.fill: parent; color: "#dcdcdc" }
            TapHandler {
                gesturePolicy: TapHandler.WithinBounds
                onTapped: root.touched = "tap_within"
            }
        }
        Item {
            objectName: "cible_mousearea"
            width: 400; height: 80
            Rectangle { anchors.fill: parent; color: "#d4d4d4" }
            MouseArea { anchors.fill: parent; onClicked: root.touched = "mousearea" }
        }
        Button {
            objectName: "cible_button"
            width: 400; height: 80
            text: "bouton"
            onClicked: root.touched = "button"
        }
        Text { objectName: "resultat"; text: root.touched }
    }
}

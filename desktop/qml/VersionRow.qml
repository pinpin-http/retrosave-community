// ─── Une version dans l'historique ────────────────────────────────────────
// `kind` porte l'essentiel : une branche de conflit n'est pas une publication
// ordinaire, et une restauration non plus. Le montrer évite la question « d'où
// sort cette version que je n'ai jamais faite ? ».
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: entry
    property int number: 0
    property string kind: "normal"
    property string device: ""
    property string moment: ""
    property int bytes: 0
    property bool head: false
    property bool acting: false
    signal restoreAsked()

    implicitHeight: 64
    Layout.fillWidth: true

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusControl
        color: entry.head ? Theme.secondaryContainer
             : hover.hovered ? Theme.surfaceLow : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
    }
    HoverHandler { id: hover }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.cardPadSm
        anchors.rightMargin: Theme.cardPadSm
        spacing: Theme.gutterMd

        Rectangle {
            implicitWidth: 38
            implicitHeight: 38
            radius: Theme.radiusControl
            color: entry.head ? Theme.primary : Theme.surfaceHigh
            Label {
                anchors.centerIn: parent
                text: entry.number
                color: entry.head ? Theme.inkInverse : Theme.inkMuted
                font.pixelSize: Theme.sizeBodySm
                font.weight: Theme.weightSemi
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                spacing: Theme.gutterSm
                Label {
                    text: entry.device.length > 0 ? entry.device : qsTr("Unknown device")
                    color: Theme.ink
                    font.pixelSize: Theme.sizeBodySm
                    font.weight: Theme.weightSemi
                }
                Chip {
                    visible: entry.head
                    label: qsTr("current")
                    tone: Theme.surfaceLowest
                    ink: Theme.inkAccent
                }
                Chip {
                    visible: entry.kind === "conflict_branch"
                    label: qsTr("conflict branch")
                    tone: Theme.errorContainer
                    ink: Theme.inkAlert
                }
                Chip {
                    visible: entry.kind === "restore"
                    label: qsTr("restore")
                    tone: Theme.surfaceHigh
                }
            }
            Label {
                text: entry.bytes >= 1024
                    ? qsTr("%1 — %2 kB").arg(entry.moment).arg(Math.round(entry.bytes / 1024))
                    : qsTr("%1 — %2 B").arg(entry.moment).arg(entry.bytes)
                color: Theme.outline
                font.pixelSize: Theme.sizeCaption
            }
        }

        Pill {
            objectName: "bouton_restaurer"
            text: qsTr("Restore")
            visible: !entry.head
            enabled: !entry.acting
            onClicked: entry.restoreAsked()
        }
    }
}

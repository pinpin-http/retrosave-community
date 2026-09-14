// ─── Un connecteur d'émulateur ────────────────────────────────────────────
// Ce que RetroSave sait lire, et où il regarde. La racine est TOUJOURS
// désignée par l'utilisateur : le manifeste ne fait que proposer, et rien
// n'est adopté sans son geste (INS-04).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: entry
    property string emulator: "folder"
    property string name: ""
    property string root: ""
    property bool present: false
    property string unitType: "file"
    signal chooseAsked()
    signal forgetAsked()

    implicitHeight: body.implicitHeight + Theme.cardPadSm * 2
    Layout.fillWidth: true

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusControl
        color: hover.hovered ? Theme.surfaceLow : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
    }
    HoverHandler { id: hover }

    RowLayout {
        id: body
        anchors.fill: parent
        anchors.leftMargin: Theme.cardPadSm
        anchors.rightMargin: Theme.cardPadSm
        spacing: Theme.gutterMd

        Rectangle {
            implicitWidth: 44
            implicitHeight: 44
            radius: Theme.radiusControl
            color: entry.root.length > 0 ? Theme.secondaryContainer : Theme.surfaceHigh
            Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
            EmulatorMark {
                anchors.centerIn: parent
                width: 20
                height: 20
                emulator: entry.emulator
                color: entry.root.length > 0 ? Theme.secondary : Theme.outline
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            RowLayout {
                spacing: Theme.gutterSm
                Label {
                    text: entry.name
                    color: Theme.ink
                    font.pixelSize: Theme.sizeTitleMd
                    font.weight: Theme.weightSemi
                }
                Chip {
                    label: entry.unitType === "dir" ? qsTr("folders") : qsTr("files")
                    tone: Theme.surfaceHigh
                }
                // Une racine configurée mais absente du disque doit se voir :
                // un disque externe débranché ne doit pas passer pour une
                // synchronisation qui marche.
                Chip {
                    visible: entry.root.length > 0 && !entry.present
                    label: qsTr("folder not found")
                    tone: Theme.errorContainer
                    ink: Theme.inkAlert
                }
                Chip {
                    visible: entry.root.length > 0 && entry.present
                    label: qsTr("watched")
                    tone: Theme.secondaryContainer
                    ink: Theme.inkAccent
                    dot: true
                }
            }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: entry.root.length > 0 ? entry.root : qsTr("No folder set")
                color: entry.root.length > 0 ? Theme.inkMuted : Theme.outline
                font.family: "monospace"
                font.pixelSize: Theme.sizeCaption
            }
        }

        Pill {
            text: entry.root.length > 0 ? qsTr("Change") : qsTr("Set")
            primary: entry.root.length === 0
            onClicked: entry.chooseAsked()
        }
        Pill {
            visible: entry.root.length > 0
            text: qsTr("Forget")
            onClicked: entry.forgetAsked()
        }
    }
}

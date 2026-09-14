// ─── Une puce d'état ──────────────────────────────────────────────────────
// Une gélule qui dit un état. **La couleur ne porte jamais seule
// l'information** : le mot est toujours là (EXP-06). C'est aussi ce qui rend
// l'interface lisible en plein soleil sur une console portable.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: chip
    property string label: ""
    property color tone: Theme.surfaceHigh
    property color ink: Theme.inkMuted
    property bool dot: false
    property color dotTone: Theme.tertiaryDim
    implicitHeight: 24
    implicitWidth: chipRow.implicitWidth + Theme.gutterMd
    radius: Theme.radiusChip
    color: chip.tone
    RowLayout {
        id: chipRow
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            visible: chip.dot
            implicitWidth: 7
            implicitHeight: 7
            radius: 3.5
            color: chip.dotTone
        }
        Label {
            text: chip.label
            color: chip.ink
            font.pixelSize: Theme.sizeCaption
            font.weight: Theme.weightSemi
        }
    }
}

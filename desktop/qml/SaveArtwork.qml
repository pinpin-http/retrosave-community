// ─── La vignette d'une sauvegarde ─────────────────────────────────────────
// Deux cas, et seulement deux.
//
// 1. **L'icône réelle**, quand la sauvegarde en porte une : `ICON0.PNG` vit
//    dans le dossier PSP que l'utilisateur nous a confié. Elle n'est ni
//    téléchargée, ni envoyée au serveur, et aucune ROM n'est ouverte (I1).
// 2. **Le repli dessiné** : une teinte et des initiales calculées par le NOYAU
//    partagé, pas ici. La même sauvegarde doit avoir la même couleur sur le
//    desktop et sur Android, sinon on croit voir deux jeux différents.
import QtQuick
import QtQuick.Controls

Rectangle {
    id: art
    property string source: ""
    property int hue: 0
    property string initials: "?"
    property string emulator: "folder"
    property bool showEmulator: true

    implicitWidth: 52
    implicitHeight: 52
    radius: width * 0.28
    // Saturation et clarté fixes : seule la teinte varie, ce qui garde toutes
    // les vignettes à la même densité visuelle et lisibles avec du blanc.
    color: art.source.length > 0 ? Theme.surfaceHigh
                                 : Qt.hsla(art.hue / 360, 0.42, 0.58, 1)

    Label {
        anchors.centerIn: parent
        visible: art.source.length === 0
        text: art.initials
        color: Theme.inkInverse
        font.pixelSize: parent.height * 0.34
        font.weight: Theme.weightSemi
    }

    Image {
        anchors.fill: parent
        visible: art.source.length > 0
        source: art.source.length > 0 ? "file://" + art.source : ""
        fillMode: Image.PreserveAspectCrop
        // Une icône illisible ne doit pas laisser un trou : le repli reprend
        // la main tout seul.
        onStatusChanged: {
            if (status === Image.Error)
                art.source = "";
        }
        layer.enabled: false
    }

    // Le badge d'émulateur, posé dans le coin comme sur les maquettes.
    Rectangle {
        visible: art.showEmulator
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: -2
        width: parent.width * 0.42
        height: width
        radius: width * 0.3
        color: Theme.surfaceLowest
        EmulatorMark {
            anchors.centerIn: parent
            width: parent.width * 0.62
            height: parent.height * 0.62
            emulator: art.emulator
            color: Theme.primary
        }
    }
}

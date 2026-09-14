// ─── Une sauvegarde dans la liste ─────────────────────────────────────────
// Composant à propriétés explicites plutôt que délégué anonyme : dans un
// délégué, le contexte extérieur n'est pas visible des outils d'analyse, et
// chaque référence à la fenêtre y devient un avertissement.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: row
    property string label: ""
    property string unitKey: ""
    property string emulator: "folder"
    property string gameIcon: ""
    property int hue: 0
    property string initials: "?"
    property int version: 0
    property string unitState: "active"
    // SYN-06 / SYN-07 : ce que l'utilisateur a demandé pour CET appareil —
    // « sync », « paused » ou « excluded ». À ne pas confondre avec
    // `unitState`, qui dit ce que la synchronisation constate.
    property string localMode: "sync"
    property bool conflict: false
    property bool current: false
    // Vrai pendant qu'une opération de l'agent est en cours : le réarmement
    // n'est pas un geste à répéter nerveusement.
    property bool acting: false
    signal opened()
    signal retryAsked()
    // Le mode demandé est passé explicitement : la ligne ne décide pas seule
    // d'un basculement, elle dit ce que l'utilisateur a cliqué.
    signal modeAsked(string mode)

    // Les états viennent du carnet, en anglais. Les montrer tels quels
    // afficherait « error » à un joueur français — et surtout « error » ne dit
    // pas ce qui s'est passé ni quoi faire.
    readonly property string stateLabel: {
        switch (row.unitState) {
        case "missing": return qsTr("missing");
        case "paused": return qsTr("paused");
        case "duplicate": return qsTr("duplicate");
        case "error": return qsTr("quarantined");
        default: return row.unitState;
        }
    }

    // Pourquoi cette sauvegarde ne bouge pas, dit avec les mots de celui qui
    // l'a demandé. Une pause générale n'est pas une pause de ce jeu-là : le
    // dire évite d'aller chercher un réglage qu'on n'a pas posé.
    readonly property string modeLabel: {
        switch (row.localMode) {
        case "paused": return qsTr("paused");
        case "excluded": return qsTr("not on this device");
        default: return "";
        }
    }

    implicitHeight: 78
    Layout.fillWidth: true

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusControl
        color: row.current ? Theme.surfaceHigh
             : hover.hovered ? Theme.surfaceLow : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
    }

    HoverHandler { id: hover }
    // ─── Pourquoi MouseArea et non TapHandler ─────────────────────────────
    // Les deux font la même chose ici : un clic, sans geste. Mais un
    // `TapHandler` est INPILOTABLE par notre banc d'interface — mesuré, pas
    // supposé : avec Spix 0.14 sur Qt 6.11, un TapHandler reste muet aux
    // événements souris synthétisés, quelle que soit sa `gesturePolicy`, alors
    // qu'un `MouseArea` et un `Button` de Controls répondent. La reproduction
    // est dans `scripts/testing/spix_probe/`.
    //
    // Un contrôle qu'aucun test ne peut actionner n'est pas couvert. On garde
    // donc MouseArea pour ce qui doit être piloté, et `HoverHandler` — qui ne
    // porte aucune action — reste tel quel.
    MouseArea { anchors.fill: parent; onClicked: row.opened() }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.cardPadSm
        anchors.rightMargin: Theme.cardPadSm
        spacing: Theme.gutterMd

        SaveArtwork {
            source: row.gameIcon
            hue: row.hue
            initials: row.initials
            emulator: row.emulator
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: row.label
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: Theme.ink
                font.pixelSize: Theme.sizeTitleMd
                font.weight: Theme.weightSemi
            }
            Label {
                text: row.unitKey
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                color: Theme.outline
                font.family: "monospace"
                font.pixelSize: Theme.sizeCaption
            }
        }

        Chip {
            visible: row.conflict
            label: qsTr("to resolve")
            tone: Theme.errorContainer
            ink: Theme.inkAlert
        }
        Chip {
            visible: !row.conflict && row.unitState !== "active"
            label: row.stateLabel
            tone: Theme.surfaceHigh
        }
        // SYN-06 / SYN-07 : le choix de l'utilisateur se voit avant tout le
        // reste. Sans cette pastille, une sauvegarde en pause ressemblerait à
        // une sauvegarde en panne.
        Chip {
            objectName: "mode_" + row.unitKey
            visible: row.localMode !== "sync"
            label: row.modeLabel
            tone: Theme.surfaceHigh
            ink: Theme.inkMuted
        }
        // Mettre en pause ne supprime rien : ni le fichier du joueur, ni la
        // moindre version sur le serveur. Les autres appareils continuent.
        Pill {
            objectName: "pause_" + row.unitKey
            visible: row.localMode !== "excluded"
            enabled: !row.acting
            text: row.localMode === "paused" ? qsTr("Resume") : qsTr("Pause")
            implicitHeight: 32
            leftPadding: Theme.gutterMd
            rightPadding: Theme.gutterMd
            onClicked: row.modeAsked(row.localMode === "paused" ? "sync" : "paused")
        }
        // Retirer de cet appareil : la sauvegarde reste sur le serveur et sur
        // les autres appareils, et le fichier local n'est pas touché.
        Pill {
            objectName: "retirer_" + row.unitKey
            enabled: !row.acting
            text: row.localMode === "excluded" ? qsTr("Resume") : qsTr("Remove here")
            implicitHeight: 32
            leftPadding: Theme.gutterMd
            rightPadding: Theme.gutterMd
            onClicked: row.modeAsked(row.localMode === "excluded" ? "sync" : "excluded")
        }
        // AD-30 : une unité écartée après des échecs répétés doit pouvoir
        // repartir depuis l'écran où on la voit. Sans ce bouton, la garde qui
        // protège la synchronisation devient une impasse.
        Pill {
            visible: row.unitState === "error"
            enabled: !row.acting
            text: qsTr("Retry")
            implicitHeight: 32
            leftPadding: Theme.gutterMd
            rightPadding: Theme.gutterMd
            onClicked: row.retryAsked()
        }
        Chip {
            label: row.version > 0 ? qsTr("version %1").arg(row.version) : qsTr("never pushed")
            tone: row.version > 0 ? Theme.secondaryContainer : Theme.surfaceHigh
            ink: row.version > 0 ? Theme.inkAccent : Theme.inkMuted
        }
    }
}

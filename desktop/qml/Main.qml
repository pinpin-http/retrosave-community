// ─── Repères QML ──────────────────────────────────────────────────────────
// QML est DÉCLARATIF : on décrit l'arbre voulu, pas les étapes pour le
// construire. `propriete: expression` est une LIAISON, réévaluée toute seule ;
// `propriete = valeur` est une affectation qui DÉTRUIT la liaison.
//
// ─── La direction artistique appliquée ici ────────────────────────────────
// Relevée sur les maquettes livrées (« Botanical Wellness & Glass ») :
//   - fond porcelaine, cartes BLANCHES à grands rayons ;
//   - barre latérale et barre haute en VERRE dépoli, flottantes ;
//   - action principale : gélule vert-noir (`primary`), jamais un aplat vif ;
//   - états et métadonnées : chips en gélule, vertes / grises / rouges ;
//   - titres à graisse 600 maximum — la DA interdit le noir gras.
//
// ─── Le mouvement ─────────────────────────────────────────────────────────
// Il explique, il ne décore pas. Trois registres, et rien d'autre :
//   - le SURVOL est imperceptible (140 ms) : il confirme qu'un élément répond ;
//   - l'APPARITION est décalée (320 ms, 60 ms d'écart) : l'œil suit la lecture
//     de haut en bas au lieu de recevoir la page d'un bloc ;
//   - un état VIVANT respire lentement (1,8 s) : une connexion établie, un
//     conflit qui attend. Ce qui ne bouge pas est ce qui ne demande rien.
//
// Aucune couleur, aucun rayon, aucune durée n'est écrit en dur : tout vient du
// singleton `Theme`, miroir de `design/tokens.json` que l'Android suit aussi.
// `pragma ComponentBehavior: Bound` lie les délégués au contexte de CE fichier.
// Sans lui, un délégué est un composant à part : `window` y devient invisible
// des outils d'analyse, et chaque action déclenchée depuis une liste produit un
// avertissement. C'est la réponse officielle de Qt depuis 6.5, et elle exige en
// retour ce qu'on fait déjà — déclarer `required property var modelData`.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import RetroSave.Desktop

ApplicationWindow {
    id: window
    // `objectName` est la RACINE des chemins de pilotage des tests d'interface
    // (`mainWindow/nav_conflits`). C'est une propriété Qt ordinaire, présente
    // dans toutes les compilations : elle ne coûte rien et ne pilote rien
    // toute seule — le canal, lui, n'existe que dans la compilation de test.
    objectName: "mainWindow"
    required property AgentConnection agent
    required property DesktopTray tray
    required property DesktopPreferences preferences
    required property string appVersion

    width: 1280
    height: 860
    minimumWidth: 900
    minimumHeight: 640
    visible: true
    title: qsTr("RetroSave")
    color: Theme.background

    readonly property bool agentOnline: agent.state === "online"

    // L'unité dont l'historique est ouvert porte-t-elle une image choisie ? La
    // réponse vient de l'agent, seul à connaître le disque : l'interface ne va
    // jamais vérifier un fichier elle-même.
    readonly property bool currentUnitHasArt: {
        for (let index = 0; index < window.agent.units.length; ++index) {
            const unit = window.agent.units[index];
            if (unit.key === window.agent.historyUnit)
                return unit.custom_art === true;
        }
        return false;
    }
    property int section: 0
    // Quel connecteur attend un dossier : le dialogue de fichiers ne peut pas
    // porter cette information lui-même.
    property string pendingEmulator: ""

    // ─── BIB-02 : chercher et filtrer ─────────────────────────────────────
    // Le filtrage se fait ICI, sur la liste que l'agent a déjà envoyée : une
    // recherche qui demanderait au disque à chaque lettre serait lente et
    // rendrait l'agent occupé pour rien. L'agent borne la liste, l'interface
    // la trie — c'est la même séparation que partout ailleurs.
    property string librarySearch: ""
    // « tous », « probleme » ou « pause ». Un seul filtre d'état à la fois :
    // combiner « en panne » et « en pause » ne répond à aucune question réelle.
    property string libraryFilter: "tous"
    property string libraryEmulator: "tous"

    // Quel appareil un dialogue est en train de viser. Comme `pendingEmulator`
    // : un Dialog ne peut pas porter cette information lui-même.
    property string renamingDevice: ""
    property string revokingDevice: ""
    property string revokingName: ""

    readonly property var filteredUnits: {
        const needle = window.librarySearch.trim().toLowerCase();
        const out = [];
        for (let index = 0; index < window.agent.units.length; ++index) {
            const unit = window.agent.units[index];
            if (window.libraryEmulator !== "tous" && unit.emulator !== window.libraryEmulator)
                continue;
            // « Problème » réunit ce qui demande une action : un conflit, une
            // unité mise de côté, une disparition. Les séparer obligerait à
            // connaître notre vocabulaire pour retrouver sa partie.
            if (window.libraryFilter === "probleme"
                && !(unit.conflict === true || unit.state === "error"
                     || unit.state === "missing" || unit.state === "duplicate"))
                continue;
            if (window.libraryFilter === "pause" && unit.local_mode === "sync")
                continue;
            if (needle.length > 0) {
                const label = (unit.label || "").toLowerCase();
                const key = (unit.key || "").toLowerCase();
                if (label.indexOf(needle) < 0 && key.indexOf(needle) < 0)
                    continue;
            }
            out.push(unit);
        }
        return out;
    }

    // Les émulateurs réellement présents dans la liste. Proposer un filtre
    // « Dolphin » à quelqu'un qui n'a pas de sauvegarde Dolphin ne ferait
    // qu'allonger la barre.
    readonly property var libraryEmulators: {
        const seen = [];
        for (let index = 0; index < window.agent.units.length; ++index) {
            const id = window.agent.units[index].emulator;
            if (id && seen.indexOf(id) < 0)
                seen.push(id);
        }
        return seen.sort();
    }

    // ─── BIB-04 : la fiche du jeu ouvert ──────────────────────────────────
    // Un jeu peut porter PLUSIEURS sauvegardes (deux cartes mémoire, deux
    // emplacements). Les montrer ensemble évite de restaurer la mauvaise.
    readonly property var currentUnit: {
        for (let index = 0; index < window.agent.units.length; ++index) {
            if (window.agent.units[index].key === window.agent.historyUnit)
                return window.agent.units[index];
        }
        return null;
    }
    readonly property var siblingUnits: {
        const out = [];
        if (!window.currentUnit)
            return out;
        for (let index = 0; index < window.agent.units.length; ++index) {
            const unit = window.agent.units[index];
            if (unit.game_key === window.currentUnit.game_key)
                out.push(unit);
        }
        return out;
    }
    // Les appareils qui ont publié une version de cette sauvegarde. Lu dans
    // l'historique, donc constaté — jamais déduit d'un nom d'appareil connu.
    readonly property var originDevices: {
        const seen = [];
        for (let index = 0; index < window.agent.versions.length; ++index) {
            const device = window.agent.versions[index].device;
            if (device && seen.indexOf(device) < 0)
                seen.push(device);
        }
        return seen;
    }

    // Un conflit à la fois, comme la maquette. Trancher se fait en regardant
    // deux parties, pas en parcourant une liste : les suivants viennent ensuite.
    readonly property int conflictCount: agent.conflicts.length
    readonly property var conflict: conflictCount > 0 ? agent.conflicts[0] : null

    // ─── Briques ──────────────────────────────────────────────────────────

    // Une carte : blanche, pleine, grand rayon. Elle entre en scène avec un
    // décalage qui suit l'ordre de lecture — d'où `rank`.
    component Pod: Rectangle {
        id: pod
        property int rank: 0
        default property alias content: podBox.data
        Layout.fillWidth: true
        implicitHeight: podBox.implicitHeight + Theme.cardPadLg * 2
        radius: Theme.radiusCard
        color: Theme.surfaceLowest

        // L'ombre ambiante de la DA : « élévation sans poids ». Trois halos
        // empilés DERRIÈRE la carte, décalés vers le bas et de plus en plus
        // pâles — la lumière du jour à travers un feuillage.
        //
        // **Dessinée, pas calculée.** La première version passait par un
        // `MultiEffect`, qui rend la carte purement et simplement INVISIBLE
        // sous le rendu logiciel — celui des machines sans GPU et des bureaux
        // distants. Une ombre décorative ne vaut pas de faire disparaître le
        // contenu chez une partie des utilisateurs.
        // Trois halos écrits en clair plutôt qu'un modèle : dans un délégué,
        // `parent` désigne le Repeater et non la carte, et trois lignes se
        // lisent mieux qu'une boucle.
        Rectangle {
            z: -1
            x: -10
            y: 4
            width: pod.width + 20
            height: pod.height + 20
            radius: pod.radius + 10
            color: Theme.shadowWide
        }
        Rectangle {
            z: -1
            x: -5
            y: 3
            width: pod.width + 10
            height: pod.height + 10
            radius: pod.radius + 5
            color: Theme.shadowMid
        }
        Rectangle {
            z: -1
            x: -2
            y: 1
            width: pod.width + 4
            height: pod.height + 4
            radius: pod.radius + 2
            color: Theme.shadowTight
        }

        // L'entrée passe par une TRANSFORMATION, jamais par `y` : dans un
        // layout, la position appartient au layout. L'animer le met en
        // désaccord avec lui et les cartes se chevauchent — constaté, corrigé.
        opacity: 0
        transform: Translate { id: lift; y: 12 }
        Component.onCompleted: entrance.start()
        SequentialAnimation {
            id: entrance
            PauseAnimation { duration: pod.rank * Theme.staggerMs }
            ParallelAnimation {
                NumberAnimation {
                    target: pod
                    property: "opacity"
                    to: 1
                    duration: Theme.enterMs
                    easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: lift
                    property: "y"
                    to: 0
                    duration: Theme.enterMs
                    easing.type: Easing.OutCubic
                }
            }
        }

        ColumnLayout {
            id: podBox
            anchors.fill: parent
            anchors.margins: Theme.cardPadLg
            spacing: Theme.gutterMd
        }
    }

    // Chip : une gélule qui dit un état. La couleur ne porte jamais seule
    // l'information — le mot est toujours là (EXP-06).
    // Gélule d'action. `tone` la fait passer de principale (vert-noir) à calme
    // (porcelaine) sans changer sa forme : la hiérarchie tient à la matière.
    // Bouton rond des barres : un puits de porcelaine, glyphe fin au centre.
    component RoundButton: Button {
        id: round
        property string glyph: ""
        implicitWidth: 36
        implicitHeight: 36
        background: Rectangle {
            radius: width / 2
            color: round.down ? Theme.surfaceHigh
                 : round.hovered ? Theme.surfaceContainer : Theme.surfaceLow
            Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        }
        contentItem: Label {
            text: round.glyph
            color: round.enabled ? Theme.inkMuted : Theme.outlineVariant
            font.pixelSize: 15
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Un réglage qui s'allume ou s'éteint : son nom, sa conséquence, son
    // interrupteur. Sorti en composant parce que les réglages de notification
    // en demandent trois — et trois copies du même balisage finissent toujours
    // par diverger sur un détail.
    component ToggleRow: Rectangle {
        id: toggleRow
        property string label: ""
        property string detail: ""
        property bool active: false
        property bool ready: true
        signal asked(bool wanted)
        Layout.fillWidth: true
        implicitHeight: 64
        radius: Theme.radiusControl
        color: Theme.surfaceLow
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.cardPadSm
            anchors.rightMargin: Theme.cardPadSm
            spacing: Theme.gutterMd
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: toggleRow.label
                    color: Theme.ink
                    font.pixelSize: Theme.sizeBodySm
                    font.weight: Theme.weightSemi
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: toggleRow.detail
                    color: Theme.inkMuted
                    font.pixelSize: Theme.sizeCaption
                }
            }
            Switch {
                id: toggle
                enabled: toggleRow.ready
                checked: toggleRow.active
                indicator: Rectangle {
                    implicitWidth: 48
                    implicitHeight: 28
                    radius: 14
                    color: toggle.checked ? Theme.primary : Theme.surfaceHighest
                    Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
                    Rectangle {
                        y: 3
                        x: toggle.checked ? parent.width - width - 3 : 3
                        width: 22
                        height: 22
                        radius: 11
                        color: Theme.surfaceLowest
                        Behavior on x {
                            NumberAnimation { duration: Theme.hoverMs; easing.type: Easing.OutCubic }
                        }
                    }
                }
                onToggled: {
                    const wanted = checked
                    // Un clic casserait la liaison avec l'état réel : on la
                    // rétablit aussitôt, c'est l'agent qui décide de la
                    // position finale.
                    checked = Qt.binding(() => toggleRow.active)
                    toggleRow.asked(wanted)
                }
            }
        }
    }

    component Field: TextField {
        id: field
        Layout.fillWidth: true
        implicitHeight: 44
        leftPadding: Theme.gutterLg
        rightPadding: Theme.gutterLg
        font.pixelSize: Theme.sizeBodySm
        color: Theme.ink
        placeholderTextColor: Theme.outline
        selectionColor: Theme.secondaryContainer
        selectedTextColor: Theme.inkAccent
        background: Rectangle {
            radius: Theme.radiusChip
            color: field.activeFocus ? Theme.surfaceLowest : Theme.surfaceLow
            border.color: field.activeFocus ? Theme.outlineVariant : "transparent"
            border.width: 1
            Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        }
    }

    // Une mesure : sa valeur en grand, son nom en petit. Quatre tuiles se
    // lisent d'un regard là où une phrase demande d'être décodée.
    component Metric: Rectangle {
        id: metric
        property string value: "0"
        property string caption: ""
        property color tone: Theme.surfaceLow
        Layout.fillWidth: true
        implicitHeight: 76
        radius: Theme.radiusControl
        color: metric.tone
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.cardPadSm
            spacing: 2
            Label {
                text: metric.caption
                color: Theme.inkMuted
                font.pixelSize: Theme.sizeCaption
            }
            Label {
                text: metric.value
                color: Theme.ink
                font.pixelSize: Theme.sizeHeadlineMd
                font.weight: Theme.weightSemi
            }
        }
    }

    // Pastille vivante : l'anneau qui s'échappe dit « en ce moment ». Un état
    // figé ne bouge pas — c'est ainsi qu'on distingue les deux sans les lire.
    component LiveDot: Item {
        id: live
        property color tone: Theme.tertiaryDim
        property bool beating: true
        implicitWidth: 8
        implicitHeight: 8
        Rectangle {
            anchors.centerIn: parent
            width: 8
            height: 8
            radius: 4
            color: live.tone
        }
        Rectangle {
            anchors.centerIn: parent
            width: 8
            height: 8
            radius: 4
            color: live.tone
            visible: live.beating
            SequentialAnimation on scale {
                running: live.beating
                loops: Animation.Infinite
                NumberAnimation { from: 1; to: 2.6; duration: Theme.pulseMs; easing.type: Easing.OutCubic }
                PauseAnimation { duration: 120 }
            }
            SequentialAnimation on opacity {
                running: live.beating
                loops: Animation.Infinite
                NumberAnimation { from: 0.55; to: 0; duration: Theme.pulseMs; easing.type: Easing.OutCubic }
                PauseAnimation { duration: 120 }
            }
        }
    }

    // Un côté du conflit. Tout ce qui permet de reconnaître SA partie — d'où
    // elle vient, quand elle est arrivée, ce qu'elle pèse — et rien de plus.
    component ChoiceCard: Rectangle {
        id: choice
        property string heading: ""
        property string device: ""
        property string moment: ""
        property int number: 0
        property int bytes: 0
        property bool latest: false
        property bool acting: false
        signal chosen()

        Layout.fillWidth: true
        implicitHeight: choiceBox.implicitHeight + Theme.cardPadMd * 2
        radius: Theme.radiusCard
        color: Theme.surfaceLow
        border.width: 1
        border.color: choice.latest ? Theme.secondaryContainer : "transparent"

        ColumnLayout {
            id: choiceBox
            anchors.fill: parent
            anchors.margins: Theme.cardPadMd
            spacing: Theme.gutterSm

            RowLayout {
                Layout.fillWidth: true
                Chip {
                    label: choice.heading
                    tone: choice.latest ? Theme.secondaryContainer : Theme.surfaceHigh
                    ink: choice.latest ? Theme.inkAccent : Theme.inkMuted
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("version %1").arg(choice.number)
                    color: Theme.outline
                    font.pixelSize: Theme.sizeCaption
                }
            }
            Label {
                text: choice.device.length > 0 ? choice.device : qsTr("Unknown device")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.ink
                font.pixelSize: Theme.sizeHeadlineMd
                font.weight: Theme.weightSemi
            }
            Label {
                text: choice.moment
                color: Theme.inkMuted
                font.pixelSize: Theme.sizeBodySm
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gutterSm
                Metric {
                    value: choice.bytes >= 1024 ? qsTr("%1 kB").arg(Math.round(choice.bytes / 1024))
                                                : qsTr("%1 B").arg(choice.bytes)
                    caption: qsTr("size")
                    tone: Theme.surfaceLowest
                }
            }
            Pill {
                objectName: "bouton_garder"
                text: qsTr("Keep this version")
                primary: choice.latest
                enabled: !choice.acting
                onClicked: choice.chosen()
            }
        }
    }

    component SectionTitle: Label {
        color: Theme.ink
        font.pixelSize: Theme.sizeHeadlineMd
        font.weight: Theme.weightSemi
    }

    component Body: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.inkMuted
        font.pixelSize: Theme.sizeBodyLg
    }

    onClosing: (close) => {
        if (window.tray.available) {
            close.accepted = false
            window.hide()
            window.tray.announceHidden()
        }
    }

    // ─── L'écran ──────────────────────────────────────────────────────────
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // — Barre latérale : verre dépoli, flottante ————————————————
        Rectangle {
            Layout.preferredWidth: 288
            Layout.fillHeight: true
            color: Theme.surfaceLow

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.cardPadMd
                spacing: Theme.gutterMd

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.gutterSm
                    Rectangle {
                        implicitWidth: 40
                        implicitHeight: 40
                        radius: Theme.radiusControl
                        color: Theme.primary
                        StarMark {
                            anchors.centerIn: parent
                            implicitWidth: 20
                            implicitHeight: 20
                            color: Theme.inkInverse
                        }
                    }
                    ColumnLayout {
                        spacing: 0
                        RowLayout {
                            spacing: Theme.gutterXs
                            Label {
                                text: "RetroSave"
                                color: Theme.primary
                                font.pixelSize: Theme.sizeTitleMd
                                font.weight: Theme.weightSemi
                            }
                            Chip {
                                label: window.appVersion
                                tone: Theme.secondaryContainer
                                ink: Theme.inkAccent
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.gutterSm
                    spacing: 2
                    // Six entrées fixes, écrites en clair : plus lisible qu'un
                    // modèle, et chaque référence à la fenêtre reste vérifiable.
                    NavItem {
                        objectName: "nav_sync"
                        glyph: "▣"
                        label: qsTr("Sync")
                        current: window.section === 0
                        onActivated: window.section = 0
                    }
                    NavItem {
                        objectName: "nav_conflits"
                        glyph: "⚠"
                        label: qsTr("Conflicts")
                        current: window.section === 1
                        alert: window.conflictCount > 0
                        onActivated: {
                            window.section = 1
                            // On va chercher l'état réel plutôt que d'afficher
                            // celui de la dernière passe : ouvrir cet écran, c'est
                            // vouloir décider maintenant.
                            window.agent.refreshConflicts()
                        }
                    }
                    NavItem {
                        objectName: "nav_historique"
                        glyph: "↺"
                        label: qsTr("Library")
                        current: window.section === 2
                        onActivated: {
                            window.section = 2
                            window.agent.refreshUnits()
                        }
                    }
                    NavItem {
                        objectName: "nav_dossiers"
                        glyph: "▤"
                        label: qsTr("Folders")
                        current: window.section === 3
                        onActivated: {
                            window.section = 3
                            window.agent.refreshUnits()
                        }
                    }
                    NavItem {
                        objectName: "nav_parametres"
                        glyph: "⚙"
                        label: qsTr("Settings")
                        current: window.section === 4
                        onActivated: {
                            window.section = 4
                            // Le journal se relit à l'ouverture : il est là
                            // pour être consulté, pas pour être demandé.
                            window.agent.refreshActivity()
                        }
                    }
                    NavItem {
                        objectName: "nav_appareils"
                        glyph: "❑"
                        label: qsTr("Devices")
                        current: window.section === 5
                        onActivated: {
                            window.section = 5
                            // La liste vient du serveur : ouvrir cet écran,
                            // c'est vouloir son état réel, pas le précédent.
                            window.agent.refreshDevices()
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Pied de barre : ce qui rassure sans qu'on ait à le demander.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: vault.implicitHeight + Theme.cardPadSm * 2
                    radius: Theme.radiusControl
                    color: Theme.surfaceLowest
                    ColumnLayout {
                        id: vault
                        anchors.fill: parent
                        anchors.margins: Theme.cardPadSm
                        spacing: Theme.gutterSm
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("Watched folder")
                                color: Theme.inkMuted
                                font.pixelSize: Theme.sizeCaption
                                Layout.fillWidth: true
                            }
                            Label {
                                text: window.preferences.folder.length > 0 ? qsTr("set") : qsTr("none")
                                color: Theme.ink
                                font.pixelSize: Theme.sizeCaption
                                font.weight: Theme.weightSemi
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 6
                            radius: 3
                            color: Theme.surfaceHighest
                            Rectangle {
                                height: parent.height
                                radius: 3
                                color: Theme.secondaryContainer
                                width: parent.width * (window.agent.ready ? 1.0 : window.agent.linked ? 0.5 : 0.12)
                                Behavior on width { NumberAnimation { duration: Theme.enterMs; easing.type: Easing.OutCubic } }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gutterXs
                            LiveDot { tone: Theme.tertiaryDim; beating: false }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // — Barre supérieure ————————————————————————————————
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 64
                color: Theme.background
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.gutterLg
                    anchors.rightMargin: Theme.gutterLg
                    spacing: Theme.gutterMd

                    Rectangle {
                        implicitHeight: 26
                        implicitWidth: hostRow.implicitWidth + Theme.gutterLg
                        radius: Theme.radiusChip
                        color: Theme.surfaceLow
                        RowLayout {
                            id: hostRow
                            anchors.centerIn: parent
                            spacing: Theme.gutterSm
                            LiveDot {
                                tone: window.agent.linked ? Theme.tertiaryDim : Theme.outlineVariant
                                beating: window.agent.linked
                            }
                            Label {
                                text: window.agent.linked ? qsTr("server linked") : qsTr("no server")
                                color: Theme.inkMuted
                                font.pixelSize: Theme.sizeCaption
                                font.family: "monospace"
                            }
                        }
                    }
                    Chip {
                        label: window.agentOnline
                            ? qsTr("Agent running · PID %1").arg(window.agent.agentPid)
                            : qsTr("Agent stopped")
                        tone: Theme.surfaceLow
                        dot: true
                        dotTone: window.agentOnline ? Theme.tertiaryDim : Theme.outlineVariant
                    }
                    Item { Layout.fillWidth: true }
                    BusyIndicator {
                        running: window.agent.busy
                        visible: running
                        implicitWidth: 22
                        implicitHeight: 22
                    }
                    RoundButton {
                        glyph: "↻"
                        enabled: !window.agent.busy
                        onClicked: window.agent.refresh()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Check agent")
                    }
                    Rectangle {
                        implicitWidth: 32
                        implicitHeight: 32
                        radius: 16
                        color: Theme.primary
                        StarMark {
                            anchors.centerIn: parent
                            implicitWidth: 15
                            implicitHeight: 15
                            color: Theme.inkInverse
                        }
                    }
                }
            }

            // — Contenu ————————————————————————————————————————
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 0

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 1120
                        Layout.alignment: Qt.AlignHCenter
                        Layout.margins: Theme.gutterXl
                        spacing: Theme.gutterLg

                        // Le titre de la section, et rien d'autre.
                        Label {
                            Layout.fillWidth: true
                            text: window.sectionName
                            color: Theme.ink
                            font.pixelSize: window.width < 1080 ? Theme.sizeHeadlineMd : Theme.sizeHeadlineLg
                            font.weight: Theme.weightSemi
                        }

                        StackLayout {
                            Layout.fillWidth: true
                            currentIndex: window.section

                            // ── 0. Synchronisation ────────────────────────
                            ColumnLayout {
                                objectName: "page_sync"
                                Layout.fillWidth: true
                                spacing: Theme.gutterLg

                                Pod {
                                    rank: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.gutterMd
                                        Rectangle {
                                            implicitWidth: 52
                                            implicitHeight: 52
                                            radius: Theme.radiusControl
                                            color: Theme.secondaryContainer
                                            StarMark {
                                                anchors.centerIn: parent
                                                implicitWidth: 22
                                                implicitHeight: 22
                                                color: Theme.secondary
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            SectionTitle {
                                                text: window.agent.syncing ? qsTr("Syncing")
                                                    : window.agent.ready ? qsTr("Watching")
                                                    : qsTr("Not watching")
                                            }
                                        }
                                        Chip {
                                            label: window.agent.ready ? qsTr("Ready") : qsTr("Incomplete")
                                            tone: window.agent.ready ? Theme.secondaryContainer : Theme.surfaceHigh
                                            ink: window.agent.ready ? Theme.inkAccent : Theme.inkMuted
                                            dot: true
                                            dotTone: window.agent.ready ? Theme.tertiaryDim : Theme.outline
                                        }
                                    }
                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: window.width < 1080 ? 2 : 4
                                        columnSpacing: Theme.gutterSm
                                        rowSpacing: Theme.gutterSm
                                        Metric { value: window.agent.statPushed; caption: qsTr("pushed") }
                                        Metric { value: window.agent.statPulled; caption: qsTr("pulled") }
                                        Metric {
                                            value: window.agent.statConflicts
                                            caption: qsTr("to resolve")
                                            tone: window.agent.statConflicts === "0" ? Theme.surfaceLow : Theme.errorContainer
                                        }
                                        Metric {
                                            value: window.agent.statErrors
                                            caption: qsTr("error")
                                            tone: window.agent.statErrors === "0" ? Theme.surfaceLow : Theme.errorContainer
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                        wrapMode: Text.WordWrap
                                        text: window.agent.lastSummary
                                        color: Theme.inkMuted
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                    RowLayout {
                                        spacing: Theme.gutterSm
                                        Pill {
                                            objectName: "bouton_synchroniser"
                                            text: qsTr("Sync now")
                                            primary: true
                                            enabled: window.agentOnline && window.agent.ready
                                                && !window.agent.busy && !window.agent.syncing
                                            onClicked: window.agent.synchronize()
                                        }
                                        Label {
                                            visible: window.agent.lastChecked.length > 0
                                            text: qsTr("Last contact %1").arg(window.agent.lastChecked)
                                            color: Theme.outline
                                            font.pixelSize: Theme.sizeCaption
                                        }
                                    }
                                }

                                Pod {
                                    rank: 1
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Local agent")
                                            Layout.fillWidth: true
                                        }
                                        Chip {
                                            label: window.agentOnline ? qsTr("Running") : qsTr("Stopped")
                                            tone: window.agentOnline ? Theme.secondaryContainer : Theme.surfaceHigh
                                            ink: window.agentOnline ? Theme.inkAccent : Theme.inkMuted
                                        }
                                    }
                                    Body { text: window.agent.message }
                                    Label {
                                        text: window.agent.syncProblem
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAlert
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                    RowLayout {
                                        spacing: Theme.gutterSm
                                        Pill {
                                            text: qsTr("Start agent")
                                            primary: true
                                            visible: !window.agentOnline
                                            enabled: !window.agent.busy
                                            onClicked: window.agent.startAgent()
                                        }
                                        Pill {
                                            text: qsTr("Stop agent")
                                            visible: window.agentOnline
                                            enabled: !window.agent.busy
                                            onClicked: window.agent.stopAgent()
                                        }
                                    }
                                }
                            }

                            // ── 1. Conflits ───────────────────────────────
                            ColumnLayout {
                                objectName: "page_conflits"
                                Layout.fillWidth: true
                                spacing: Theme.gutterLg

                                Pod {
                                    rank: 0
                                    visible: window.conflict === null
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("No open conflict")
                                            Layout.fillWidth: true
                                        }
                                        Chip {
                                            label: qsTr("Nothing to resolve")
                                            tone: Theme.secondaryContainer
                                            ink: Theme.inkAccent
                                            dot: true
                                        }
                                    }
                                    Pill {
                                        objectName: "bouton_verifier_conflits"
                                        text: qsTr("Check now")
                                        enabled: window.agentOnline && window.agent.linked && !window.agent.busy
                                        onClicked: window.agent.refreshConflicts()
                                    }
                                }

                                Pod {
                                    rank: 0
                                    visible: window.conflict !== null
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.gutterSm
                                        Rectangle {
                                            implicitWidth: 52
                                            implicitHeight: 52
                                            radius: Theme.radiusControl
                                            color: Theme.errorContainer
                                            Label {
                                                anchors.centerIn: parent
                                                text: "⚠"
                                                color: Theme.inkAlert
                                                font.pixelSize: 22
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            SectionTitle {
                                                text: window.conflict ? window.conflict.unit : ""
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                        }
                                        Chip {
                                            label: window.conflictCount > 1
                                                ? qsTr("%1 incidents").arg(window.conflictCount)
                                                : qsTr("1 incident")
                                            tone: Theme.errorContainer
                                            ink: Theme.inkAlert
                                        }
                                    }

                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: window.width < 1080 ? 1 : 2
                                        columnSpacing: Theme.gutterMd
                                        rowSpacing: Theme.gutterMd
                                        ChoiceCard {
                                            heading: qsTr("Server version")
                                            device: window.conflict ? window.conflict.a_device : ""
                                            moment: window.conflict ? qsTr("Published %1").arg(window.conflict.a_at) : ""
                                            objectName: "branche_a"
                                            number: window.conflict ? window.conflict.a_number : 0
                                            bytes: window.conflict ? window.conflict.a_size : 0
                                            acting: window.agent.busy
                                            onChosen: {
                                                if (window.conflict)
                                                    window.agent.resolveConflict(window.conflict.id, window.conflict.a_number)
                                            }
                                        }
                                        ChoiceCard {
                                            heading: qsTr("Latest")
                                            latest: true
                                            device: window.conflict ? window.conflict.b_device : ""
                                            moment: window.conflict ? qsTr("Published %1").arg(window.conflict.b_at) : ""
                                            objectName: "branche_b"
                                            number: window.conflict ? window.conflict.b_number : 0
                                            bytes: window.conflict ? window.conflict.b_size : 0
                                            acting: window.agent.busy
                                            onChosen: {
                                                if (window.conflict)
                                                    window.agent.resolveConflict(window.conflict.id, window.conflict.b_number)
                                            }
                                        }
                                    }

                                    // La promesse qui rend la décision supportable.
                                    // Elle est vraie : le serveur ne supprime rien,
                                    // et la branche perdante reste une version de
                                    // l'historique (invariant I3).
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: reassure.implicitHeight + Theme.cardPadSm * 2
                                        radius: Theme.radiusControl
                                        color: Theme.surfaceLow
                                        RowLayout {
                                            id: reassure
                                            anchors.fill: parent
                                            anchors.margins: Theme.cardPadSm
                                            spacing: Theme.gutterSm
                                            StarMark {
                                                Layout.alignment: Qt.AlignTop
                                                implicitWidth: 16
                                                implicitHeight: 16
                                                color: Theme.secondary
                                            }
                                        }
                                    }
                                    Label {
                                        text: window.agent.syncProblem
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAlert
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                }
                            }

                            // ── 2. Historique ─────────────────────────────
                            ColumnLayout {
                                objectName: "page_historique"
                                Layout.fillWidth: true
                                spacing: Theme.gutterLg

                                Pod {
                                    rank: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Tracked saves")
                                            Layout.fillWidth: true
                                        }
                                        // SYN-06 : la pause générale. Elle
                                        // n'arrête pas l'agent et ne supprime
                                        // rien — elle suspend les captures et
                                        // les réceptions, ici seulement.
                                        Pill {
                                            objectName: "bouton_pause_generale"
                                            text: window.agent.globalPause ? qsTr("Resume all")
                                                                           : qsTr("Pause all")
                                            enabled: window.agentOnline
                                            onClicked: window.agent.setGlobalPause(!window.agent.globalPause)
                                        }
                                        Pill {
                                            text: qsTr("Refresh")
                                            enabled: window.agentOnline && !window.agent.busy
                                            onClicked: window.agent.refreshUnits()
                                        }
                                    }
                                    Body {
                                        visible: window.agent.globalPause
                                        text: qsTr("Everything is paused on this device.")
                                    }
                                    Body {
                                        visible: window.agent.units.length === 0
                                        text: qsTr("No saves tracked yet.")
                                    }

                                    // ── BIB-02 : chercher, puis filtrer ───
                                    Field {
                                        objectName: "champ_recherche"
                                        visible: window.agent.units.length > 0
                                        placeholderText: qsTr("Search…")
                                        text: window.librarySearch
                                        onTextChanged: window.librarySearch = text
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        visible: window.agent.units.length > 0
                                        spacing: Theme.gutterSm
                                        Repeater {
                                            // Les trois filtres d'état, puis un
                                            // par émulateur présent. Un seul
                                            // actif à la fois : c'est une
                                            // question, pas une combinaison.
                                            model: [
                                                { id: "tous", label: qsTr("All") },
                                                { id: "probleme", label: qsTr("Needs attention") },
                                                { id: "pause", label: qsTr("Paused or removed") }
                                            ]
                                            delegate: Pill {
                                                required property var modelData
                                                objectName: "filtre_" + modelData.id
                                                text: modelData.label
                                                primary: window.libraryFilter === modelData.id
                                                implicitHeight: 34
                                                leftPadding: Theme.gutterMd
                                                rightPadding: Theme.gutterMd
                                                onClicked: window.libraryFilter = modelData.id
                                            }
                                        }
                                        Repeater {
                                            model: window.libraryEmulators
                                            delegate: Pill {
                                                required property var modelData
                                                objectName: "filtre_emu_" + modelData
                                                text: modelData
                                                primary: window.libraryEmulator === modelData
                                                implicitHeight: 34
                                                leftPadding: Theme.gutterMd
                                                rightPadding: Theme.gutterMd
                                                // Recliquer le filtre actif le
                                                // retire : sinon il faut deviner
                                                // où se trouve « tous ».
                                                onClicked: window.libraryEmulator =
                                                    (window.libraryEmulator === modelData ? "tous" : modelData)
                                            }
                                        }
                                    }
                                    Body {
                                        objectName: "recherche_vide"
                                        visible: window.agent.units.length > 0
                                                 && window.filteredUnits.length === 0
                                        text: qsTr("No match.")
                                    }
                                    Label {
                                        visible: window.filteredUnits.length > 0
                                                 && window.filteredUnits.length !== window.agent.units.length
                                        text: qsTr("%1 of %2 saves shown.")
                                                  .arg(window.filteredUnits.length)
                                                  .arg(window.agent.units.length)
                                        color: Theme.inkMuted
                                        font.pixelSize: Theme.sizeCaption
                                    }
                                    Repeater {
                                        model: window.filteredUnits
                                        delegate: UnitRow {
                                            required property var modelData
                                            // Chemin de pilotage stable : nommé par sa CLÉ, pas par sa position —
                                            // un tri différent ne doit pas changer ce qu'un test désigne.
                                            objectName: "unite_" + modelData.key
                                            label: modelData.label
                                            unitKey: modelData.key
                                            emulator: modelData.emulator
                                            gameIcon: modelData.icon
                                            hue: modelData.hue
                                            initials: modelData.initials
                                            version: modelData.version
                                            unitState: modelData.state
                                            localMode: modelData.local_mode
                                            conflict: modelData.conflict
                                            current: modelData.key === window.agent.historyUnit
                                            acting: window.agent.busy
                                            onOpened: window.agent.openHistory(modelData.key)
                                            onRetryAsked: window.agent.retryUnit(modelData.key)
                                            onModeAsked: mode => window.agent.setLocalMode(modelData.key, mode)
                                        }
                                    }
                                }

                                Pod {
                                    rank: 1
                                    visible: window.agent.versions.length > 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("History — %1").arg(window.agent.historyUnit)
                                            Layout.fillWidth: true
                                            elide: Text.ElideMiddle
                                        }
                                        Chip {
                                            label: qsTr("head: version %1").arg(window.agent.historyHead)
                                            tone: Theme.secondaryContainer
                                            ink: Theme.inkAccent
                                        }
                                    }
                                    // ── BIB-04 : la fiche du jeu ──────────
                                    // Dans la MÊME carte que l'historique, et
                                    // au-dessus de lui : c'est le contexte de
                                    // ce qu'on s'apprête à restaurer. En carte
                                    // séparée, elle repoussait les versions
                                    // hors de l'écran — donc hors d'atteinte,
                                    // y compris du banc qui les clique.
                                    ColumnLayout {
                                        objectName: "fiche_jeu"
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            visible: window.siblingUnits.length > 1
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            text: qsTr("%1 saves for this game: %2.")
                                                      .arg(window.siblingUnits.length)
                                                      .arg(window.siblingUnits.map(u => u.key).join(", "))
                                            color: Theme.inkMuted
                                            font.pixelSize: Theme.sizeCaption
                                        }
                                        Label {
                                            visible: window.originDevices.length > 0
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            text: qsTr("Published from %1").arg(window.originDevices.join(", "))
                                            color: Theme.inkMuted
                                            font.pixelSize: Theme.sizeCaption
                                        }
                                        // BIB-06 : la fraîcheur est celle du
                                        // dernier contrôle RÉUSSI. Ne jamais
                                        // prétendre connaître l'état d'un autre
                                        // appareil maintenant.
                                    }
                                    // ── L'image de la sauvegarde ──────────────
                                    // Purement décoratif : choisir une image ne
                                    // renomme aucun fichier, ne change ni la clé
                                    // de l'unité ni celle du jeu, et n'influence
                                    // aucune décision de synchronisation.
                                    // Le texte AU-DESSUS des boutons, pas à côté :
                                    // dans une ligne, une explication longue
                                    // comprime les gélules jusqu'à les rendre
                                    // inatteignables — au doigt comme au clic.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.gutterSm
                                        Pill {
                                            objectName: "bouton_choisir_image"
                                            text: window.currentUnitHasArt ? qsTr("Replace image")
                                                                           : qsTr("Choose an image")
                                            enabled: window.agentOnline && window.agent.historyUnit.length > 0
                                            onClicked: artworkDialog.open()
                                        }
                                        Pill {
                                            objectName: "bouton_retirer_image"
                                            text: qsTr("Remove")
                                            visible: window.currentUnitHasArt
                                            enabled: window.agentOnline
                                            onClicked: window.agent.chooseArtwork(window.agent.historyUnit, "")
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    Repeater {
                                        model: window.agent.versions
                                        delegate: VersionRow {
                                            required property var modelData
                                            // Chemin de pilotage stable : nommé par sa CLÉ, pas par sa position —
                                            // un tri différent ne doit pas changer ce qu'un test désigne.
                                            objectName: "version_" + modelData.number
                                            number: modelData.number
                                            kind: modelData.kind
                                            device: modelData.device
                                            moment: modelData.at
                                            bytes: modelData.size
                                            head: modelData.number === window.agent.historyHead
                                            acting: window.agent.busy
                                            onRestoreAsked: window.agent.restoreVersion(window.agent.historyUnit, modelData.number)
                                        }
                                    }
                                    Label {
                                        text: window.agent.syncProblem
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAlert
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                }
                            }

                            // ── 3. Dossiers ───────────────────────────────
                            ColumnLayout {
                                objectName: "page_dossiers"
                                Layout.fillWidth: true
                                spacing: Theme.gutterLg

                                Pod {
                                    rank: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Adapters")
                                            Layout.fillWidth: true
                                        }
                                        Chip {
                                            label: qsTr("%1 adapters").arg(window.agent.adapters.length)
                                            tone: Theme.surfaceHigh
                                        }
                                    }
                                    Body {
                                        visible: window.agent.adapters.length === 0
                                        text: qsTr("No adapters loaded.")
                                    }
                                    Repeater {
                                        model: window.agent.adapters
                                        delegate: AdapterRow {
                                            required property var modelData
                                            // Chemin de pilotage stable : nommé par sa CLÉ, pas par sa position —
                                            // un tri différent ne doit pas changer ce qu'un test désigne.
                                            objectName: "connecteur_" + modelData.id
                                            emulator: modelData.id
                                            name: modelData.name
                                            root: modelData.root
                                            present: modelData.present
                                            unitType: modelData.unit_type
                                            onChooseAsked: {
                                                window.pendingEmulator = modelData.id
                                                adapterDialog.open()
                                            }
                                            onForgetAsked: window.preferences.forgetRoot(modelData.id)
                                        }
                                    }
                                    Pill {
                                        objectName: "bouton_relire_connecteurs"
                                        text: qsTr("Refresh")
                                        enabled: window.agentOnline
                                        onClicked: window.agent.refreshUnits()
                                    }
                                }

                                Pod {
                                    rank: 1
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Generic folder")
                                            Layout.fillWidth: true
                                        }
                                        Chip {
                                            label: qsTr("no emulator")
                                            tone: Theme.surfaceHigh
                                        }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: 44
                                        radius: Theme.radiusChip
                                        color: Theme.surfaceLow
                                        Label {
                                            anchors.fill: parent
                                            anchors.leftMargin: Theme.gutterLg
                                            anchors.rightMargin: Theme.gutterLg
                                            verticalAlignment: Text.AlignVCenter
                                            elide: Text.ElideMiddle
                                            text: window.preferences.folder.length > 0
                                                ? window.preferences.folder
                                                : qsTr("No folder set")
                                            color: window.preferences.folder.length > 0 ? Theme.ink : Theme.outline
                                            font.family: "monospace"
                                            font.pixelSize: Theme.sizeBodySm
                                        }
                                    }
                                    Label {
                                        text: window.preferences.folderProblem
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAlert
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                    RowLayout {
                                        spacing: Theme.gutterSm
                                        Pill {
                                            text: qsTr("Choose a folder")
                                            onClicked: folderDialog.open()
                                        }
                                        Pill {
                                            text: qsTr("Forget")
                                            enabled: window.preferences.folder.length > 0
                                            onClicked: window.preferences.forgetFolder()
                                        }
                                    }
                                }
                            }

                            // ── 4. Paramètres ─────────────────────────────
                            ColumnLayout {
                                objectName: "page_parametres"
                                Layout.fillWidth: true
                                spacing: Theme.gutterLg

                                Pod {
                                    rank: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: window.agent.linked ? qsTr("Server linked") : qsTr("Connect a server")
                                            Layout.fillWidth: true
                                        }
                                        Chip {
                                            visible: window.agent.linked
                                            label: qsTr("Device registered")
                                            tone: Theme.secondaryContainer
                                            ink: Theme.inkAccent
                                        }
                                    }
                                    Field {
                                        objectName: "champ_serveur"
                                        id: serverField
                                        enabled: window.agentOnline && !window.agent.busy
                                        placeholderText: qsTr("https://retrosave.example.com")
                                        inputMethodHints: Qt.ImhUrlCharactersOnly
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.gutterSm
                                        Field {
                                            objectName: "champ_jeton"
                                            id: tokenField
                                            enabled: window.agentOnline && !window.agent.busy
                                            placeholderText: qsTr("Access token")
                                            echoMode: TextInput.Password
                                        }
                                        Field {
                                            objectName: "champ_appareil"
                                            id: deviceField
                                            enabled: window.agentOnline && !window.agent.busy
                                            placeholderText: qsTr("Device name")
                                        }
                                    }
                                    Pill {
                                        objectName: "bouton_relier"
                                        text: window.agent.linked ? qsTr("Reconnect") : qsTr("Connect")
                                        primary: true
                                        enabled: window.agentOnline && !window.agent.busy
                                            && serverField.text.length > 0 && tokenField.text.length > 0
                                            && deviceField.text.length > 0
                                        onClicked: {
                                            window.agent.connectAccount(serverField.text, tokenField.text, deviceField.text)
                                            tokenField.clear()
                                        }
                                    }
                                }

                                Pod {
                                    rank: 1
                                    SectionTitle { text: qsTr("Desktop behaviour") }
                                    Body {
                                        text: window.tray.available
                                            ? qsTr("Closing the window hides it in the tray.")
                                            : qsTr("No system tray on this desktop. Closing the window quits the interface; the agent keeps running.")
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: 64
                                        radius: Theme.radiusControl
                                        color: Theme.surfaceLow
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: Theme.cardPadSm
                                            anchors.rightMargin: Theme.cardPadSm
                                            spacing: Theme.gutterMd
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                Label {
                                                    text: qsTr("Start the agent at login")
                                                    color: Theme.ink
                                                    font.pixelSize: Theme.sizeBodySm
                                                    font.weight: Theme.weightSemi
                                                }
                                            }
                                            Switch {
                                                id: autostart
                                                enabled: !window.preferences.autostartBusy
                                                    && window.preferences.autostart !== "unsupported"
                                                checked: window.preferences.autostart === "on"
                                                indicator: Rectangle {
                                                    implicitWidth: 48
                                                    implicitHeight: 28
                                                    radius: 14
                                                    color: autostart.checked ? Theme.primary : Theme.surfaceHighest
                                                    Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
                                                    Rectangle {
                                                        y: 3
                                                        x: autostart.checked ? parent.width - width - 3 : 3
                                                        width: 22
                                                        height: 22
                                                        radius: 11
                                                        color: Theme.surfaceLowest
                                                        Behavior on x {
                                                            NumberAnimation { duration: Theme.hoverMs; easing.type: Easing.OutCubic }
                                                        }
                                                    }
                                                }
                                                onToggled: {
                                                    const wanted = checked
                                                    // Un clic casserait la liaison avec l’état réel :
                                                    // on la rétablit aussitôt, c’est le système qui
                                                    // décide de la position finale.
                                                    checked = Qt.binding(() => window.preferences.autostart === "on")
                                                    window.preferences.setStartWithSession(wanted)
                                                }
                                            }
                                        }
                                    }
                                    Label {
                                        text: window.preferences.problem
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAlert
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                }

                                // ── EXP-02 : ce dont on veut être prévenu ──
                                Pod {
                                    objectName: "pod_notifications"
                                    rank: 2
                                    SectionTitle { text: qsTr("Notifications") }
                                    Body {
                                        visible: !window.tray.available
                                        text: qsTr("No system tray on this desktop.")
                                    }
                                    ToggleRow {
                                        objectName: "notif_conflits"
                                        label: qsTr("A conflict to resolve")
                                        active: window.agent.notifyConflicts
                                        ready: window.agentOnline
                                        onAsked: wanted => window.agent.setNotifications(
                                            wanted, window.agent.notifyErrors, window.agent.notifyRestores)
                                    }
                                    ToggleRow {
                                        objectName: "notif_erreurs"
                                        label: qsTr("A failed sync")
                                        active: window.agent.notifyErrors
                                        ready: window.agentOnline
                                        onAsked: wanted => window.agent.setNotifications(
                                            window.agent.notifyConflicts, wanted, window.agent.notifyRestores)
                                    }
                                    ToggleRow {
                                        objectName: "notif_restaurations"
                                        label: qsTr("A finished restore")
                                        detail: qsTr("A previous version is the head again.")
                                        active: window.agent.notifyRestores
                                        ready: window.agentOnline
                                        onAsked: wanted => window.agent.setNotifications(
                                            window.agent.notifyConflicts, window.agent.notifyErrors, wanted)
                                    }
                                }

                                // ── EXP-05 : versions et compatibilité ─────
                                Pod {
                                    objectName: "pod_versions"
                                    rank: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Versions")
                                            Layout.fillWidth: true
                                        }
                                        Pill {
                                            objectName: "bouton_verifier_serveur"
                                            text: qsTr("Check server")
                                            enabled: window.agentOnline && window.agent.linked
                                                     && !window.agent.busy
                                            onClicked: window.agent.checkServer()
                                        }
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: Theme.gutterSm
                                        Chip {
                                            label: qsTr("interface %1").arg(window.appVersion)
                                            tone: Theme.surfaceHigh
                                        }
                                        Chip {
                                            label: qsTr("agent %1").arg(
                                                window.agent.agentVersion.length > 0
                                                    ? window.agent.agentVersion : qsTr("unknown"))
                                            tone: Theme.surfaceHigh
                                        }
                                        Chip {
                                            objectName: "chip_version_serveur"
                                            label: qsTr("server %1").arg(
                                                window.agent.serverVersion.length > 0
                                                    ? window.agent.serverVersion : qsTr("unchecked"))
                                            tone: window.agent.serverCompatibility === "ecart"
                                                      ? Theme.errorContainer : Theme.surfaceHigh
                                            ink: window.agent.serverCompatibility === "ecart"
                                                     ? Theme.inkAlert : Theme.inkMuted
                                        }
                                    }
                                    // Un avertissement qui s'affiche en
                                    // permanence n'avertit plus de rien : la
                                    // comparaison ignore le suffixe de build
                                    // (AD-31), et ce bandeau ne paraît que sur
                                    // un vrai écart de version.
                                    Body {
                                        objectName: "avertissement_version"
                                        visible: window.agent.serverCompatibility === "ecart"
                                        text: qsTr("Server and client versions differ.")
                                    }
                                }

                                // ── EXP-03 / EXP-04 / PRO-08 ───────────────
                                Pod {
                                    objectName: "pod_maintenance"
                                    rank: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Diagnostics and export")
                                            Layout.fillWidth: true
                                        }
                                        Pill {
                                            text: qsTr("Refresh")
                                            enabled: window.agentOnline
                                            onClicked: window.agent.refreshActivity()
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.gutterSm
                                        Pill {
                                            objectName: "bouton_diagnostic"
                                            text: qsTr("Write diagnostics")
                                            enabled: window.agentOnline
                                            onClicked: diagnosticDialog.open()
                                        }
                                        Pill {
                                            objectName: "bouton_export"
                                            text: qsTr("Export saves")
                                            enabled: window.agentOnline && !window.agent.busy
                                            onClicked: exportDialog.open()
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    Label {
                                        objectName: "resultat_operation"
                                        text: window.agent.outcome
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAccent
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                    // EXP-03 : le journal, lisible sans passer
                                    // par un fichier. Borné par l'agent.
                                    Body {
                                        visible: window.agent.activity.length === 0
                                        text: qsTr("No activity yet.")
                                    }
                                    Repeater {
                                        model: window.agent.activity
                                        delegate: Label {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            text: modelData
                                            color: Theme.inkMuted
                                            font.family: "monospace"
                                            font.pixelSize: Theme.sizeCaption
                                        }
                                    }
                                }
                            }

                            // ── 5. Appareils (EXP-01) ─────────────────────
                            ColumnLayout {
                                objectName: "page_appareils"
                                Layout.fillWidth: true
                                spacing: Theme.gutterLg

                                Pod {
                                    rank: 0
                                    RowLayout {
                                        Layout.fillWidth: true
                                        SectionTitle {
                                            text: qsTr("Devices")
                                            Layout.fillWidth: true
                                        }
                                        Pill {
                                            objectName: "bouton_relire_appareils"
                                            text: qsTr("Refresh")
                                            enabled: window.agentOnline && window.agent.linked
                                                     && !window.agent.busy
                                            onClicked: window.agent.refreshDevices()
                                        }
                                    }
                                    Body {
                                        visible: !window.agent.linked
                                        text: qsTr("Connect to a server first.")
                                    }
                                    Body {
                                        visible: window.agent.linked && window.agent.devices.length === 0
                                        text: qsTr("No devices listed.")
                                    }
                                    // La limite est dite ici, pas découverte
                                    // après coup : révoquer refuse CET
                                    // enregistrement, pas le jeton du compte.
                                    Body {
                                        visible: window.agent.devices.length > 0
                                        text: qsTr("The account token stays valid. To cut access for good, regenerate it on your server.")
                                    }
                                    Repeater {
                                        model: window.agent.devices
                                        delegate: Rectangle {
                                            // Un id explicite : les enfants du
                                            // délégué qualifient leur accès à
                                            // `modelData`, sinon qmllint le
                                            // signale — et un accès non
                                            // qualifié casse au moindre
                                            // renommage de propriété.
                                            id: deviceRow
                                            required property var modelData
                                            objectName: "appareil_" + deviceRow.modelData.id
                                            Layout.fillWidth: true
                                            implicitHeight: 72
                                            radius: Theme.radiusControl
                                            color: Theme.surfaceLow
                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: Theme.cardPadSm
                                                anchors.rightMargin: Theme.cardPadSm
                                                spacing: Theme.gutterMd
                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2
                                                    RowLayout {
                                                        spacing: Theme.gutterSm
                                                        Label {
                                                            text: deviceRow.modelData.name
                                                            color: Theme.ink
                                                            font.pixelSize: Theme.sizeTitleMd
                                                            font.weight: Theme.weightSemi
                                                        }
                                                        Chip {
                                                            visible: deviceRow.modelData.current === true
                                                            label: qsTr("this device")
                                                            tone: Theme.secondaryContainer
                                                            ink: Theme.inkAccent
                                                        }
                                                        Chip {
                                                            visible: deviceRow.modelData.revoked === true
                                                            label: qsTr("revoked")
                                                            tone: Theme.errorContainer
                                                            ink: Theme.inkAlert
                                                        }
                                                    }
                                                    Label {
                                                        // BIB-06 : le dernier
                                                        // contact, jamais « en
                                                        // ligne » — nous ne le
                                                        // savons pas.
                                                        text: deviceRow.modelData.last_seen.length > 0
                                                            ? qsTr("%1 — last seen %2").arg(deviceRow.modelData.os).arg(deviceRow.modelData.last_seen)
                                                            : qsTr("%1 — never seen").arg(deviceRow.modelData.os)
                                                        color: Theme.inkMuted
                                                        font.pixelSize: Theme.sizeCaption
                                                    }
                                                }
                                                Pill {
                                                    objectName: "renommer_" + deviceRow.modelData.id
                                                    text: qsTr("Rename")
                                                    implicitHeight: 34
                                                    leftPadding: Theme.gutterMd
                                                    rightPadding: Theme.gutterMd
                                                    enabled: window.agentOnline && !window.agent.busy
                                                    onClicked: {
                                                        window.renamingDevice = deviceRow.modelData.id
                                                        renameField.text = deviceRow.modelData.name
                                                        renameDialog.open()
                                                    }
                                                }
                                                Pill {
                                                    objectName: "revoquer_" + deviceRow.modelData.id
                                                    text: qsTr("Revoke")
                                                    danger: true
                                                    implicitHeight: 34
                                                    leftPadding: Theme.gutterMd
                                                    rightPadding: Theme.gutterMd
                                                    // Ni l'appareil courant, ni
                                                    // un appareil déjà révoqué.
                                                    visible: deviceRow.modelData.current !== true
                                                             && deviceRow.modelData.revoked !== true
                                                    enabled: window.agentOnline && !window.agent.busy
                                                    onClicked: {
                                                        window.revokingDevice = deviceRow.modelData.id
                                                        window.revokingName = deviceRow.modelData.name
                                                        revokeDialog.open()
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    Label {
                                        text: window.agent.syncProblem
                                        visible: text.length > 0
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: Theme.inkAlert
                                        font.pixelSize: Theme.sizeBodySm
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Les intitulés de chaque section, tenus à un seul endroit.
    readonly property var sectionNames: [qsTr("Sync"), qsTr("Conflicts"),
                                         qsTr("Library"), qsTr("Folders"),
                                         qsTr("Settings"), qsTr("Devices")]
    readonly property string sectionName: sectionNames[section]

    FileDialog {
        id: artworkDialog
        title: qsTr("Choose an image")
        // PNG seulement : l'agent valide l'en-tête sans jamais DÉCODER l'image,
        // et un décodeur est une surface d'attaque qu'un service de fond n'a
        // aucune raison d'ouvrir. Le JPEG reste donc refusé.
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: window.agent.chooseArtwork(window.agent.historyUnit, selectedFile)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose a saves folder")
        onAccepted: window.preferences.chooseFolder(selectedFolder)
    }

    // EXP-04 : l'utilisateur choisit OÙ écrire son diagnostic. On ne le dépose
    // jamais d'office quelque part : il doit savoir ce qu'il s'apprête à
    // partager, et pouvoir le relire avant.
    FileDialog {
        id: diagnosticDialog
        title: qsTr("Save diagnostics")
        fileMode: FileDialog.SaveFile
        // Pas de nom présélectionné : désigner un fichier qui n'existe pas
        // encore fait avertir FileDialog, et un avertissement au démarrage est
        // exactement ce que la recette de fumée doit continuer d'attraper.
        // L'extension est ajoutée toute seule.
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text files (*.txt)")]
        onAccepted: window.agent.exportDiagnostic(selectedFile)
    }

    // PRO-08 : une copie, vers un dossier vide choisi par l'utilisateur.
    FolderDialog {
        id: exportDialog
        title: qsTr("Choose an empty folder")
        onAccepted: window.agent.exportData(selectedFolder)
    }

    // EXP-01 : renommer un appareil. Le champ est prérempli avec son nom
    // actuel — on renomme rarement en repartant de rien.
    Dialog {
        id: renameDialog
        objectName: "dialogue_renommer"
        title: qsTr("Rename this device")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            spacing: Theme.gutterSm
            Field {
                id: renameField
                objectName: "champ_renommer"
                Layout.preferredWidth: 360
                placeholderText: qsTr("Device name")
            }
        }
        onAccepted: {
            window.agent.renameDevice(window.renamingDevice, renameField.text)
            window.renamingDevice = ""
        }
        onRejected: window.renamingDevice = ""
    }

    // La révocation se confirme : c'est le seul geste de cet écran qu'on ne
    // peut pas défaire depuis l'application.
    Dialog {
        id: revokeDialog
        objectName: "dialogue_revoquer"
        title: qsTr("Revoke this device?")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            Layout.preferredWidth: 380
            width: 380
            wrapMode: Text.WordWrap
            color: Theme.ink
            font.pixelSize: Theme.sizeBodySm
            text: qsTr("“%1” will no longer sync with this server. Published versions stay in history; nothing is deleted. This cannot be undone from the app.")
                      .arg(window.revokingName)
        }
        onAccepted: {
            window.agent.revokeDevice(window.revokingDevice)
            window.revokingDevice = ""
            window.revokingName = ""
        }
        onRejected: {
            window.revokingDevice = ""
            window.revokingName = ""
        }
    }

    FolderDialog {
        id: adapterDialog
        title: qsTr("Choose this emulator’s folder")
        onAccepted: {
            window.preferences.chooseRoot(window.pendingEmulator, selectedFolder)
            window.pendingEmulator = ""
        }
        onRejected: window.pendingEmulator = ""
    }
}

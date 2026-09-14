// ─── Une entrée de la barre latérale ──────────────────────────────────────
// Sortie en composant plutôt que laissée en délégué d'un Repeater : dans un
// délégué, le contexte extérieur n'est pas visible des outils d'analyse, et
// chaque référence à la fenêtre y devient un avertissement. Un composant à
// propriétés explicites se vérifie, se réutilise et se lit mieux.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: item
    property string glyph: ""
    property string label: ""
    property bool current: false
    property string badge: ""
    property bool alert: false
    signal activated()

    implicitHeight: 44
    Layout.fillWidth: true

    // ─── EXP-06 : atteignable sans souris ─────────────────────────────────
    // Un `Item` n'est pas focusable par défaut ; sans ces trois lignes, la
    // barre latérale était un mur pour qui navigue au clavier. Entrée et
    // Espace activent, comme partout ailleurs dans le système.
    activeFocusOnTab: true
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
            || event.key === Qt.Key_Space) {
            item.activated()
            event.accepted = true
        }
    }
    Accessible.role: Accessible.Button
    Accessible.name: item.label
    Accessible.onPressAction: item.activated()

    Rectangle {
        id: plate
        anchors.fill: parent
        radius: Theme.radiusControl
        // Le survol ne fait qu'accuser réception : 140 ms, sans déplacement.
        color: item.current ? Theme.primary : hover.hovered ? Theme.surfaceHigh : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        // Le focus clavier se voit à un LISERÉ, pas à un changement de fond :
        // il doit rester lisible sur l'entrée déjà sélectionnée, qui est
        // pleine. La couleur ne porte jamais seule l'information.
        border.color: item.activeFocus ? Theme.inkAccent : "transparent"
        border.width: 2
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
    MouseArea { anchors.fill: parent; onClicked: item.activated() }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.cardPadSm
        anchors.rightMargin: Theme.cardPadSm
        spacing: Theme.gutterSm
        Label {
            text: item.glyph
            color: item.current ? Theme.inkInverse : Theme.inkMuted
            font.pixelSize: 15
            Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        }
        Label {
            text: item.label
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: item.current ? Theme.inkInverse : Theme.inkMuted
            font.pixelSize: Theme.sizeBodySm
            font.weight: item.current ? Theme.weightSemi : Font.Normal
            Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        }
        // Ce qui attend une décision bat ; le reste est immobile. C'est ce qui
        // permet de repérer un conflit sans lire la barre entière.
        Item {
            visible: item.alert
            implicitWidth: 8
            implicitHeight: 8
            Rectangle {
                anchors.centerIn: parent
                width: 8
                height: 8
                radius: 4
                color: Theme.error
            }
            Rectangle {
                anchors.centerIn: parent
                width: 8
                height: 8
                radius: 4
                color: Theme.error
                SequentialAnimation on scale {
                    running: item.alert
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: 2.6; duration: Theme.pulseMs; easing.type: Easing.OutCubic }
                    PauseAnimation { duration: 120 }
                }
                SequentialAnimation on opacity {
                    running: item.alert
                    loops: Animation.Infinite
                    NumberAnimation { from: 0.55; to: 0; duration: Theme.pulseMs; easing.type: Easing.OutCubic }
                    PauseAnimation { duration: 120 }
                }
            }
        }
        Rectangle {
            visible: item.badge.length > 0
            implicitHeight: 20
            implicitWidth: badgeLabel.implicitWidth + Theme.gutterMd
            radius: Theme.radiusChip
            color: item.current ? Theme.primaryContainer : Theme.surfaceHighest
            Label {
                id: badgeLabel
                anchors.centerIn: parent
                text: item.badge
                color: item.current ? Theme.inkOnContainer : Theme.outline
                font.pixelSize: Theme.sizeCaption
            }
        }
    }
}

// ─── Une gélule d'action ──────────────────────────────────────────────────
// `primary` la fait passer d'action principale (vert-noir) à action calme
// (porcelaine) sans changer sa forme : la hiérarchie tient à la matière, pas
// à la géométrie. Une seule action principale par carte.
import QtQuick
import QtQuick.Controls

Button {
    id: pill
    property bool primary: false
    property bool danger: false
    // EXP-06 : la gélule se prend au clavier, et Entrée/Espace l'actionnent —
    // `Button` s'en charge dès qu'il a le focus. Sans cette ligne, le style
    // Basic laisse les boutons hors du parcours de tabulation.
    activeFocusOnTab: true
    Accessible.name: pill.text
    implicitHeight: 44
    leftPadding: Theme.gutterLg
    rightPadding: Theme.gutterLg
    font.pixelSize: Theme.sizeBodySm
    font.weight: Theme.weightSemi
    background: Rectangle {
        radius: Theme.radiusChip
        color: !pill.enabled ? Theme.surfaceHigh
             : pill.danger ? (pill.down ? Qt.darker(Theme.error, 1.2) : Theme.error)
             : pill.primary ? (pill.down ? Qt.lighter(Theme.primary, 1.4) : Theme.primary)
             : pill.down ? Theme.surfaceHighest : Theme.surfaceLow
        Behavior on color { ColorAnimation { duration: Theme.hoverMs } }
        // Le focus clavier se voit à un liseré, sur la forme existante : un
        // bouton qui change de couleur au focus se confond avec un bouton actif.
        border.color: pill.activeFocus ? Theme.inkAccent : "transparent"
        border.width: 2
        // Le survol soulève à peine : assez pour répondre, pas assez pour
        // qu'on le remarque consciemment.
        scale: pill.hovered && pill.enabled ? 1.02 : 1.0
        Behavior on scale { NumberAnimation { duration: Theme.hoverMs; easing.type: Easing.OutCubic } }
    }
    contentItem: Label {
        text: pill.text
        font: pill.font
        color: !pill.enabled ? Theme.outline
             : (pill.primary || pill.danger) ? Theme.inkInverse : Theme.ink
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}

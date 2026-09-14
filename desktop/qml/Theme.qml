// ─── L'identité visuelle, en un seul endroit ──────────────────────────────
// Miroir Qt de `design/tokens.json`, lui-même relevé sur les maquettes validées
// et archivées hors dépôt. Les valeurs sont
// recopiées à la main plutôt que chargées : une application ne doit pas
// dépendre d'un fichier de conception pour démarrer.
//
// Elles ne peuvent pas dériver en silence : `tests/test_design_tokens.py` relit
// ce fichier et son équivalent Android et échoue si l'un s'écarte. C'est la
// discipline des vecteurs partagés du noyau, appliquée à l'apparence.
//
// Ce sont des rôles Material 3, ce qui rend la transposition Compose directe.
pragma Singleton

import QtQuick

QtObject {
    // ── Surfaces ──────────────────────────────────────────────────────────
    readonly property color background: "#f6fbf5"
    readonly property color surfaceLowest: "#ffffff"   // les cartes
    readonly property color surfaceLow: "#f0f5ef"      // champs, boutons calmes
    readonly property color surfaceContainer: "#eaefea"
    readonly property color surfaceHigh: "#e5e9e4"
    readonly property color surfaceHighest: "#dfe4de"

    // ── Encres ────────────────────────────────────────────────────────────
    // **Renommage obligatoire.** Les rôles Material 3 s'appellent `onSurface`,
    // `onPrimary`, `onErrorContainer`… et QML refuse tout identifiant `on` +
    // majuscule : il y voit un gestionnaire de signal, et le fichier ne charge
    // plus. La correspondance, pour que l'Android reste traçable :
    //   ink            = on-surface            inkMuted   = on-surface-variant
    //   inkInverse     = on-primary            inkAccent  = on-secondary-fixed
    //   inkOnContainer = on-primary-container  inkAlert   = on-error-container
    readonly property color ink: "#181d1a"
    readonly property color inkMuted: "#424844"
    readonly property color outline: "#727974"
    readonly property color outlineVariant: "#c1c8c3"

    // ── Vert de marque ────────────────────────────────────────────────────
    // `primary` est un vert si sombre qu'il se lit comme un noir : c'est la
    // gélule d'action principale des maquettes. Le vert vif `tertiaryDim` ne
    // sert qu'aux pastilles d'état vivantes.
    readonly property color primary: "#07241a"
    readonly property color inkInverse: "#ffffff"
    readonly property color primaryContainer: "#1e3a2f"
    readonly property color inkOnContainer: "#86a496"
    readonly property color secondary: "#43664d"
    readonly property color secondaryContainer: "#c2e9c9"
    readonly property color inkAccent: "#00210e"
    readonly property color tertiaryDim: "#4de082"

    // ── Ce qui demande une décision ───────────────────────────────────────
    readonly property color error: "#ba1a1a"
    readonly property color errorContainer: "#ffdad6"
    readonly property color inkAlert: "#93000a"
    readonly property color inverseSurface: "#2c322e"
    readonly property color inverseOnSurface: "#edf2ec"

    // ── Verre ─────────────────────────────────────────────────────────────
    // La DA pose du verre sur les barres FLOTTANTES (latérale, supérieure),
    // jamais en fond de carte : une carte est blanche et pleine.
    readonly property color glass: Qt.rgba(1, 1, 1, 0.72)
    readonly property color glassEdge: Qt.rgba(1, 1, 1, 0.65)

    // ── Ombre portée ──────────────────────────────────────────────────────
    // Trois couches, de la plus large à la plus serrée, empilées sous une même
    // carte : c'est ce dégradé d'opacités qui donne la profondeur, pas une
    // ombre unique. Le vert sombre plutôt qu'un noir pur — une ombre noire sur
    // un fond verdâtre paraît sale.
    //
    // Ces trois valeurs vivent ICI, comme le verre, et non dans
    // `design/tokens.json` : ce sont des couleurs DÉRIVÉES du fond, pas des
    // jetons relevés sur les maquettes.
    readonly property color shadowWide: Qt.rgba(0.11, 0.19, 0.15, 0.035)
    readonly property color shadowMid: Qt.rgba(0.11, 0.19, 0.15, 0.045)
    readonly property color shadowTight: Qt.rgba(0.11, 0.19, 0.15, 0.05)

    // ── Formes ────────────────────────────────────────────────────────────
    readonly property int radiusChip: 999
    readonly property int radiusControl: 16
    readonly property int radiusCard: 24
    readonly property int radiusPod: 32

    // ── Rythme ────────────────────────────────────────────────────────────
    readonly property int gutterXs: 4
    readonly property int gutterSm: 8
    readonly property int gutterMd: 16
    readonly property int gutterLg: 24
    readonly property int gutterXl: 32
    readonly property int cardPadSm: 14
    readonly property int cardPadMd: 20
    readonly property int cardPadLg: 28

    // ── Échelle typographique ─────────────────────────────────────────────
    // La pile de polices est déclarée une fois dans `src/app/main.cpp` : le
    // type `font` de QML n'accepte qu'une famille, `QFont::setFamilies` prend
    // la liste d'essai, et la police de l'application se transmet ensuite.
    // Graisse plafonnée à 600 : la DA interdit le noir gras.
    readonly property int sizeHero: 40
    readonly property int sizeHeadlineLg: 28
    readonly property int sizeHeadlineMd: 22
    readonly property int sizeTitleMd: 17
    readonly property int sizeBodyLg: 15
    readonly property int sizeBodySm: 13
    readonly property int sizeCaption: 11
    readonly property int weightSemi: Font.DemiBold

    // ── Mouvement ─────────────────────────────────────────────────────────
    // Le mouvement explique, il ne décore pas. Un survol doit être
    // imperceptible, une apparition se remarquer sans faire attendre, un état
    // vivant respirer lentement.
    readonly property int hoverMs: 140
    readonly property int enterMs: 320
    readonly property int staggerMs: 60
    readonly property int pulseMs: 1800
}

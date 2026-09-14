// ─── La marque d'un émulateur ─────────────────────────────────────────────
// Dessinée, pas importée. Trois raisons : aucune police d'icônes n'est
// embarquée, une image par émulateur serait un binaire de plus à mettre à
// l'échelle, et surtout ces silhouettes doivent rester les mêmes sur Android,
// qui les dessinera avec la même géométrie.
//
// Ce sont des SILHOUETTES, pas des logos : reproduire la marque d'un émulateur
// dans notre interface suggérerait un partenariat qui n'existe pas.
import QtQuick

Canvas {
    id: mark
    // « ppsspp », « melonds », « azahar », « retroarch », « folder ».
    property string emulator: "folder"
    property color color: Theme.ink

    implicitWidth: 16
    implicitHeight: 16
    onColorChanged: requestPaint()
    onEmulatorChanged: requestPaint()

    onPaint: {
        const context = getContext("2d");
        context.reset();
        context.fillStyle = mark.color;
        const w = width;
        const h = height;
        const round = (x, y, rw, rh, r) => {
            context.beginPath();
            context.moveTo(x + r, y);
            context.arcTo(x + rw, y, x + rw, y + rh, r);
            context.arcTo(x + rw, y + rh, x, y + rh, r);
            context.arcTo(x, y + rh, x, y, r);
            context.arcTo(x, y, x + rw, y, r);
            context.closePath();
            context.fill();
        };

        if (mark.emulator === "melonds") {
            // Deux écrans empilés : la silhouette de la DS.
            round(w * 0.18, h * 0.12, w * 0.64, h * 0.34, w * 0.08);
            round(w * 0.18, h * 0.54, w * 0.64, h * 0.34, w * 0.08);
        } else if (mark.emulator === "azahar") {
            // Le clapet de la 3DS : l'écran haut plus large que le bas.
            round(w * 0.12, h * 0.12, w * 0.76, h * 0.34, w * 0.08);
            round(w * 0.24, h * 0.54, w * 0.52, h * 0.34, w * 0.08);
        } else if (mark.emulator === "ppsspp") {
            // La barre horizontale d'une console à écran large.
            round(w * 0.06, h * 0.28, w * 0.88, h * 0.44, h * 0.2);
        } else if (mark.emulator === "dolphin") {
            // Un disque optique : le GameCube et la Wii lisent des galettes.
            context.beginPath();
            context.arc(w / 2, h / 2, w * 0.42, 0, Math.PI * 2);
            context.fill();
            context.globalCompositeOperation = "destination-out";
            context.beginPath();
            context.arc(w / 2, h / 2, w * 0.13, 0, Math.PI * 2);
            context.fill();
            context.globalCompositeOperation = "source-over";
        } else if (mark.emulator === "duckstation" || mark.emulator === "pcsx2") {
            // Une carte mémoire : un corps et son ergot de détrompage. PCSX2
            // reprend la même silhouette, en plus large — ce sont deux
            // générations du même objet.
            const wide = mark.emulator === "pcsx2";
            round(w * (wide ? 0.08 : 0.16), h * 0.2, w * (wide ? 0.84 : 0.68), h * 0.6, w * 0.09);
            context.globalCompositeOperation = "destination-out";
            round(w * (wide ? 0.3 : 0.34), h * 0.2, w * 0.32, h * 0.14, w * 0.03);
            context.globalCompositeOperation = "source-over";
        } else if (mark.emulator === "mgba") {
            // Une console à écran unique, tenue à deux mains : le corps large,
            // l'écran au centre.
            round(w * 0.06, h * 0.24, w * 0.88, h * 0.52, h * 0.18);
            context.globalCompositeOperation = "destination-out";
            round(w * 0.32, h * 0.36, w * 0.36, h * 0.28, w * 0.04);
            context.globalCompositeOperation = "source-over";
        } else if (mark.emulator === "snes9x") {
            // Une cartouche : le corps, et l'encoche du connecteur en bas.
            // C'est l'objet que le joueur reconnaît, pas le logo de l'émulateur.
            round(w * 0.18, h * 0.1, w * 0.64, h * 0.66, w * 0.07);
            context.globalCompositeOperation = "destination-out";
            round(w * 0.34, h * 0.1, w * 0.32, h * 0.16, w * 0.03);
            context.globalCompositeOperation = "source-over";
            round(w * 0.3, h * 0.78, w * 0.4, h * 0.12, w * 0.03);
        } else if (mark.emulator === "retroarch") {
            // Un noyau et ses satellites : le multi-système.
            context.beginPath();
            context.arc(w / 2, h / 2, w * 0.2, 0, Math.PI * 2);
            context.fill();
            for (let step = 0; step < 3; ++step) {
                const angle = (Math.PI * 2 * step) / 3 - Math.PI / 2;
                context.beginPath();
                context.arc(w / 2 + Math.cos(angle) * w * 0.35,
                            h / 2 + Math.sin(angle) * h * 0.35, w * 0.1, 0, Math.PI * 2);
                context.fill();
            }
        } else {
            // Le dossier générique : une languette et un corps.
            round(w * 0.1, h * 0.2, w * 0.34, h * 0.12, w * 0.04);
            round(w * 0.1, h * 0.28, w * 0.8, h * 0.5, w * 0.07);
        }
    }
}

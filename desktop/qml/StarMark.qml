// ─── La marque ────────────────────────────────────────────────────────────
// Une étoile à sept branches, dessinée plutôt qu'importée : une image serait un
// binaire de plus à embarquer, à mettre à l'échelle et à garder synchronisé
// avec l'application Android, qui dessine exactement la même géométrie.
import QtQuick

Canvas {
    id: mark
    property color color: Theme.primary
    property int points: 7
    property real innerRatio: 0.42

    implicitWidth: 24
    implicitHeight: 24
    onColorChanged: requestPaint()

    onPaint: {
        const context = getContext("2d");
        context.reset();
        const middleX = width / 2;
        const middleY = height / 2;
        const outer = Math.min(width, height) / 2;
        const inner = outer * mark.innerRatio;
        context.beginPath();
        // Un sommet, un creux, et ainsi de suite : 2n points au total. Le
        // décalage de -90° met une branche vers le haut plutôt que vers la
        // droite — sans lui l'étoile paraît penchée.
        for (let step = 0; step < mark.points * 2; ++step) {
            const radius = step % 2 === 0 ? outer : inner;
            const angle = (Math.PI * step) / mark.points - Math.PI / 2;
            const x = middleX + radius * Math.cos(angle);
            const y = middleY + radius * Math.sin(angle);
            if (step === 0)
                context.moveTo(x, y);
            else
                context.lineTo(x, y);
        }
        context.closePath();
        context.fillStyle = mark.color;
        context.fill();
    }
}

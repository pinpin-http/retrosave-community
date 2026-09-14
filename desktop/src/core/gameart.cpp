#include "core/gameart.h"

#include <QCryptographicHash>
#include <QStringList>

namespace retrosave::core
{
namespace
{
const QByteArray PngSignature = QByteArray::fromRawData("\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8);

// Les mots vides, communs aux trois implémentations. Toute divergence ici
// donnerait des initiales différentes selon l'appareil.
const QStringList &stopWords()
{
    static const QStringList words{"the", "a",  "an",  "of", "le", "la",
                                   "les", "un", "une", "de", "du", "des"};
    return words;
}

quint32 readInt(const QByteArray &data, qsizetype offset)
{
    quint32 value = 0;
    for (int index = 0; index < 4; ++index)
        value = (value << 8) | quint8(data.at(offset + index));
    return value;
}

QStringList tokenize(const QString &label)
{
    QStringList words;
    QString current;
    for (const auto character : label) {
        if (character.isLetterOrNumber()) {
            current.append(character);
        } else if (!current.isEmpty()) {
            words << current;
            current.clear();
        }
    }
    if (!current.isEmpty())
        words << current;
    return words;
}
} // namespace

std::optional<IconInfo> readPngHeader(const QByteArray &data, qsizetype maxBytes)
{
    if (data.isEmpty() || data.size() > maxBytes)
        return std::nullopt;
    if (data.size() < 33)
        return std::nullopt;
    if (!data.startsWith(PngSignature))
        return std::nullopt;
    // Longueur du premier bloc (13) puis son type : IHDR est obligatoirement le
    // premier ; un fichier qui commence autrement n'est pas un PNG valide.
    if (readInt(data, 8) != 13)
        return std::nullopt;
    if (data.mid(12, 4) != QByteArray("IHDR"))
        return std::nullopt;

    const auto width = int(readInt(data, 16));
    const auto height = int(readInt(data, 20));
    if (width <= 0 || height <= 0)
        return std::nullopt;
    if (width > MaxIconDimension || height > MaxIconDimension)
        return std::nullopt;
    return IconInfo{width, height};
}

bool isValidIcon(const QByteArray &data, qsizetype maxBytes)
{
    return readPngHeader(data, maxBytes).has_value();
}

int placeholderHue(const QString &gameKey)
{
    const auto digest = QCryptographicHash::hash(gameKey.toUtf8(), QCryptographicHash::Sha256);
    qint64 value = 0;
    for (int index = 0; index < 4; ++index)
        value = (value << 8) | quint8(digest.at(index));
    return int(value % 360);
}

QString placeholderInitials(const QString &label)
{
    const auto words = tokenize(label);
    if (words.isEmpty())
        return QStringLiteral("?");
    QStringList meaningful;
    for (const auto &word : words) {
        if (!stopWords().contains(word.toLower()))
            meaningful << word;
    }
    if (meaningful.isEmpty())
        meaningful = words;
    if (meaningful.size() == 1)
        return meaningful.first().left(2).toUpper();
    return (meaningful.at(0).left(1) + meaningful.at(1).left(1)).toUpper();
}

} // namespace retrosave::core

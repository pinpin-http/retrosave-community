#pragma once

// Helpers privés aux codecs HTTP. Ne pas les utiliser comme règles du noyau.
#include <QDateTime>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>
#include <optional>

namespace retrosave::network::json
{
inline bool uuid(const QString &value)
{
    static const QRegularExpression pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z");
    return pattern.match(value).hasMatch();
}
inline bool hash(const QString &value)
{
    static const QRegularExpression pattern("^[0-9a-f]{64}\\z");
    return pattern.match(value).hasMatch();
}
inline bool header(const QByteArray &value)
{
    if (value.isEmpty() || value.size() > 4096)
        return false;
    for (unsigned char byte : value)
        if (byte < 0x21 || byte > 0x7e)
            return false;
    return true;
}
inline bool safeInteger(qint64 value)
{
    return value >= 0 && value <= 9007199254740991LL;
}
inline std::optional<qint64> integer(const QJsonValue &value, qint64 minimum = 0)
{
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < minimum ||
        number > 9007199254740991.0 || std::floor(number) != number)
        return {};
    return static_cast<qint64>(number);
}
inline bool timestamp(const QString &value)
{
    return QDateTime::fromString(value, Qt::ISODateWithMs).isValid();
}
inline bool httpUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           !url.hasFragment() && (url.scheme() == "https" || url.scheme() == "http");
}

inline bool os(const QString &value)
{
    return value == "linux" || value == "windows" || value == "android";
}
// **Ce qui est vérifié ici est la FORME, pas la politique.** La liste des
// émulateurs que le service accepte appartient au serveur
// (`server/app/core/emulators.py`) : lui seul peut trancher, et il doit le
// faire de toute façon puisqu'il ne croit aucun client sur parole.
//
// Le client en gardait une copie, et c'est ce qui a produit le défaut du
// 13/09/2026 : cinq adaptateurs livrés — mGBA, Snes9x, Dolphin, DuckStation,
// PCSX2 — étaient absents de cette copie. Leurs sauvegardes étaient découvertes,
// nommées, affichées dans la bibliothèque… et refusées AVANT envoi. Le serveur
// ne voyait jamais passer la requête, donc rien ne pouvait le signaler : ni un
// code d'erreur, ni un journal, ni un test de découverte.
//
// La copie a donc été retirée plutôt que corrigée. Ce qui reste est un contrôle
// syntaxique — l'identifiant a la forme d'un identifiant — et c'est tout ce que
// cette couche peut honnêtement affirmer. Un identifiant bien formé mais inconnu
// du serveur part sur le réseau et revient en 422 : l'unité passe en erreur avec
// une raison lisible, au lieu de disparaître en silence. C'est exactement le
// comportement voulu face à un serveur plus ancien que le client.
//
// Ce que le client sait vraiment, ce sont les manifestes de `adapters/` : c'est
// eux qui décident ce qu'il peut découvrir. `test_adapters_e2e.py` vérifie que
// chacun d'eux atteint bien une version serveur, et
// `test_supported_emulators.py` que le serveur les accepte tous.
inline bool emulator(const QString &value)
{
    // Minuscules, chiffres, tiret et souligné : la forme d'un identifiant de
    // manifeste. Bornée, parce qu'elle voyage dans une requête.
    static const QRegularExpression shape(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,31}$"));
    return shape.match(value).hasMatch();
}
inline bool unitType(const QString &value)
{
    return value == "file" || value == "dir";
}
} // namespace retrosave::network::json

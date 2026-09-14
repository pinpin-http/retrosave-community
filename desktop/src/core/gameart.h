// Fonctions pures rejouées par C++ et Kotlin sur `game_art_basic.json`.
//
// La couleur d'une vignette est dérivée de
// la clé de jeu. Si les trois clients la calculaient différemment, la même
// sauvegarde changerait de couleur d'un appareil à l'autre — et le joueur
// croirait avoir affaire à deux jeux distincts.
//
// Rien ici n'ouvre une ROM : la vraie icône, quand
// elle existe, est le `ICON0.PNG` qui vit DANS le dossier de sauvegarde PSP,
// donc dans une donnée que l'utilisateur nous a déjà confiée. Aucune image
// n'est envoyée au serveur.
//
// Une jaquette proposée par le serveur peut être téléchargée dans le cache
// local. Elle reste décorative et n'influence aucune décision du moteur.
#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace retrosave::core
{

// Plafond de lecture d'une icône ; les vraies pèsent quelques kilo-octets.
inline constexpr qsizetype MaxIconBytes = 256 * 1024;
// Plafond d'une jaquette téléchargée, plus haut que celui d'une icône locale : une
// jaquette de catalogue pèse quelques centaines de kilo-octets là où un
// `ICON0.PNG` en pèse dix. Les deux bornes restent distinctes parce qu'elles
// protègent de deux choses différentes — l'une lit le disque de l'utilisateur,
// l'autre borne un téléchargement que nous avons nous-mêmes déclenché.
inline constexpr qsizetype MaxArtworkBytes = 2 * 1024 * 1024;
// Ce qu'on refuse : les dimensions qui feraient exploser un décodeur.
inline constexpr int MaxIconDimension = 4096;

struct IconInfo {
    int width = 0;
    int height = 0;
};

// Valide la signature PNG et l'en-tête IHDR — sans décoder l'image. C'est un
// filtre bon marché qui écarte ce qui n'est manifestement pas un PNG avant de
// le confier à un décodeur : un `.sav` renommé, un fichier tronqué, une image
// absurde.
// `maxBytes` a une valeur par défaut : les appelants existants — et les
// vecteurs partagés — gardent exactement le comportement d'avant. Seule la
// jaquette téléchargée passe une borne plus haute, parce qu'elle vient d'un
// transfert que nous avons déclenché et borné nous-mêmes, pas d'un fichier
// trouvé sur le disque de l'utilisateur.
std::optional<IconInfo> readPngHeader(const QByteArray &data, qsizetype maxBytes = MaxIconBytes);
bool isValidIcon(const QByteArray &data, qsizetype maxBytes = MaxIconBytes);

// Teinte 0–359 dérivée de la CLÉ DE JEU, pas du libellé : le libellé change au
// renommage ou quand un client apprend à lire le SFO, et une vignette qui
// change de couleur donnerait l'impression d'un autre jeu.
//
// sha256 plutôt qu'un hachage de la bibliothèque standard : celui de Qt n'est
// ni celui de Python ni celui de Kotlin, et la couleur ne serait alors ni
// stable ni partagée.
int placeholderHue(const QString &gameKey);

// Une ou deux lettres tirées du libellé, en majuscules. Les mots vides sautent,
// mais jamais s'il ne reste rien : « The Legend of Zelda » donne « LZ », alors
// que « The Sims » doit encore afficher quelque chose.
QString placeholderInitials(const QString &label);

} // namespace retrosave::core

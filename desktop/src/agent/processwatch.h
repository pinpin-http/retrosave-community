/***
 * Quels émulateurs tournent en ce moment.
 *
 * C'est la seule protection, avec la stabilisation, contre la capture d'une
 * sauvegarde à moitié écrite : un émulateur ouvert peut écrire à tout instant,
 * et une archive prise à ce moment-là fige un fichier déchiré. La règle vient
 * de CLAUDE.md §9.1 et du cas 19 du banc d'acceptation.
 ***/
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace retrosave::agent
{

/**
 * Les noms de processus actuellement visibles, en minuscules.
 *
 * Lecture **best-effort** : ce qu'on ne peut pas lire est simplement absent du
 * résultat. Sous Linux, `/proc` peut refuser un processus d'un autre
 * utilisateur — ce n'est pas une erreur, c'est un processus qui ne nous
 * concerne pas.
 */
QSet<QString> runningProcessNames();

/**
 * Un émulateur donné tourne-t-il ?
 *
 * La comparaison se fait sans casse et **sans extension** : un manifeste
 * annonce `PPSSPPWindows64.exe` et `ppsspp`, et le même manifeste sert aux deux
 * systèmes. Comparer les noms bruts ferait dépendre la protection du système
 * sur lequel le manifeste a été écrit.
 */
bool anyProcessRunning(const QStringList &processNames, const QSet<QString> &running);

} // namespace retrosave::agent

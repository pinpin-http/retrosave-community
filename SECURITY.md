# Sécurité

## Signaler une faille

**N'ouvrez pas d'issue publique.** Écrivez à l'adresse de contact du dépôt
GitHub, ou utilisez l'onglet *Security → Report a vulnerability* de GitHub.

Décrivez ce que vous avez trouvé, comment le reproduire, et ce que cela permet
de faire. Une réponse vous parviendra sous quelques jours — le projet est
maintenu par une seule personne, merci d'en tenir compte.

Merci de laisser un délai raisonnable avant toute publication.

## Ce que le projet promet

- **Vos sauvegardes ne sont jamais détruites** : copie de sécurité avant tout
  remplacement, écriture atomique relue, et les deux côtés d'un conflit
  conservés. C'est l'engagement principal, et il est vérifié par des tests.
- **Vos fichiers de jeu ne sont jamais lus** : aucune ROM n'est ouverte, hachée
  ou transférée.
- **Les archives ne transitent pas par l'API** : elles vont directement de votre
  appareil au stockage objet, par des URL signées et temporaires.
- **Le jeton d'accès n'apparaît jamais dans un journal**, et le fichier qui le
  contient est restreint à votre seul compte utilisateur.

## Ce que le projet ne promet pas

Autant être clair, parce que ces limites sont des choix, pas des oublis :

- **Pas de chiffrement de bout en bout.** Le serveur — le vôtre — voit le
  contenu de vos sauvegardes. Aucun engagement E2EE n'est pris.
- **Pas de recette de sécurité complète** : le périmètre du POC est décrit au
  §12 de [`CLAUDE.md`](CLAUDE.md), et il n'a pas été audité.
- **Pas encore de comptes** : l'accès repose sur un jeton d'invitation généré
  sur le serveur. Qui détient le jeton accède aux données.
- **Le serveur est le vôtre.** Sa mise à jour, ses sauvegardes et son exposition
  au réseau vous appartiennent ; le [runbook](docs/guides/RUNBOOK.md) décrit ce
  qui est attendu.

## Versions couvertes

Le projet n'a pas encore de version publiée. Seule la branche `main` est
suivie.

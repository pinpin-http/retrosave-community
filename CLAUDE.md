# RetroSave Community — cadre de contribution

RetroSave synchronise des sauvegardes d'émulateurs entre Android, Windows et
Linux avec un serveur auto-hébergé. La version actuelle est une alpha : une
fonction prévue n'est jamais présentée comme livrée avant son implémentation et
sa validation.

## Invariants de sécurité

1. **Aucune ROM ni aucun BIOS.** Une unité correspondant à un format de jeu ou
   dépassant 256 Mio est refusée. Les fixtures du dépôt sont synthétiques.
2. **Aucune destruction silencieuse.** Avant un remplacement local, créer une
   sauvegarde rotative puis écrire dans un fichier temporaire et le renommer.
3. **Deux branches en cas de conflit.** Le serveur conserve la tête et la
   version concurrente jusqu'à une résolution explicite. Aucune fusion et
   aucune règle « le plus récent gagne ».
4. **Le serveur ordonne les versions.** Les horloges clientes restent
   informatives ; le serveur attribue les numéros avec compare-and-swap.
5. **Intégrité de bout en bout.** Une unité voyage dans une archive `tar.zst`
   déterministe. Les empreintes de l'archive et du contenu sont vérifiées avant
   toute application locale.

## Périmètre Community

- client desktop Qt 6/QML et agent C++ séparé ;
- client Android Kotlin/Compose avec SAF, Room et WorkManager ;
- API FastAPI, PostgreSQL et stockage objet S3 compatible ;
- comptes auto-hébergés créés par jeton d'invitation ;
- découverte par manifestes, historique, restauration, conflits et export ;
- déploiement Docker Compose public en HTTPS.

Le chiffrement de bout en bout, macOS, les magasins d'applications, les mises à
jour automatiques et la purge automatique de l'historique ne sont pas livrés.

## Discipline de modification

Toute évolution du moteur commence dans `tests/vectors/`. Les vecteurs doivent
rester identiques pour C++ et Kotlin. Les tests ne manipulent que des sauvegardes
synthétiques et des services jetables.

Une réception suit toujours cet ordre : télécharger, vérifier l'archive,
extraire dans un espace temporaire, vérifier le contenu, sauvegarder la cible,
puis effectuer le remplacement atomique. Une disparition locale marque l'unité
comme absente ; elle ne supprime aucune version distante.

Les choix techniques actuels sont décrits dans `ARCHITECTURE.md`. L'état réel et
les limites vérifiées sont consignés dans `docs/IMPLEMENTATION_STATUS.md`.

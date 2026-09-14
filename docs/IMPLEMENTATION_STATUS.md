# État d'implémentation

Mise à jour : 15 septembre 2026. Version préparée : `0.1.0-alpha.1`.

## Disponible

- API FastAPI `/v0`, migrations PostgreSQL, comptes par jeton d'invitation et
  vérification JWT optionnelle ;
- stockage S3 privé par URL présignée ;
- moteur C++ desktop, agent séparé, interface Qt/QML, carnet SQLite, écritures
  atomiques et sauvegardes locales ;
- client Android Kotlin/Compose avec SAF, Room et synchronisation en tâche de
  fond ;
- versions, historique, restauration, conflits conservant les deux branches,
  sélection locale, pause globale et export ;
- neuf connecteurs : PPSSPP, melonDS, Azahar, Dolphin, DuckStation, PCSX2,
  mGBA, RetroArch et dossier générique ;
- déploiement HTTPS auto-hébergé, sauvegarde des métadonnées et objets, et
  restauration vers de nouveaux volumes ;
- construction de paquets Linux, Windows et Android par GitHub Actions.

## Preuves automatisées

La préparation locale de cette version passe :

- 108 tests serveur dans des services jetables ;
- 59 tests Python de vecteurs et de conception ;
- 12 tests CTest Linux ;
- tests Kotlin, lint Android et construction APK ;
- convergence de deux moteurs, agent complet et neuf connecteurs contre le
  vrai serveur ;
- parcours graphique de connexion, synchronisation, conflit, historique,
  restauration, image, pause et redémarrage.

La CI Community rejoue ces contrôles à chaque changement. Ses résultats prouvent
la compilation et les scénarios automatisés, pas une utilisation prolongée sur
appareils réels.

## Limites connues

- aucune recette manuelle complète Windows ou Android sur appareil réel ;
- installateur Windows sans certificat d'éditeur ;
- interface en anglais et aucune mise à jour automatique ;
- pas de chiffrement de bout en bout ;
- pas de quota ni de purge automatique de l'historique ;
- pas de rotation du jeton d'un compte existant ;
- chemins d'émulateurs pas tous confirmés sur chaque OS/version ;
- MinIO intégré pratique pour l'alpha mais projet amont archivé ; un S3
  activement maintenu est préférable pour une exploitation durable.

Les critères formels non encore cochés restent dans
[ACCEPTANCE_V1.md](validation/ACCEPTANCE_V1.md).

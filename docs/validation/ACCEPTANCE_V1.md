# Recette de RetroSave Community

Une case n'est cochée qu'avec la version, le système, le mode d'installation,
le scénario observé et une preuve reproductible. Les tests automatisés utilisent
uniquement des sauvegardes synthétiques. Une capture d'écran seule ne prouve ni
l'intégrité ni la reprise après interruption.

## Preuves automatisées requises

- [ ] tests serveur avec PostgreSQL et stockage objet jetables ;
- [ ] mêmes vecteurs d'identité, de décision et d'archive en C++ et Kotlin ;
- [ ] aller-retour et conflit entre deux moteurs via la vraie API ;
- [ ] chaque manifeste d'émulateur publie une version sur le serveur ;
- [ ] agent desktop complet et parcours de la vraie interface Qt ;
- [ ] tests Kotlin, lint et construction de l'APK release ;
- [ ] compilation, tests et arbre déployé sous Linux et Windows ;
- [ ] installation, démarrage et désinstallation silencieuse du paquet Windows ;
- [ ] déploiement HTTPS, santé DB/S3, sauvegarde complète et restauration dans
  de nouveaux volumes ;
- [ ] empreintes SHA-256 de tous les artefacts de release.

## Recette sur appareils réels

- [ ] installation de l'APK sur Android 10 ou plus récent ;
- [ ] installation Windows x64 sur une machine sans SDK Qt ;
- [ ] installation Linux x64 sur une distribution prise en charge ;
- [ ] connexion de deux appareils au même serveur auto-hébergé ;
- [ ] aller-retour d'une sauvegarde PPSSPP sans ouvrir de ROM ;
- [ ] refus de capturer pendant l'exécution d'un émulateur desktop ;
- [ ] conflit provoqué hors ligne, deux branches téléchargées puis résolution ;
- [ ] restauration d'une ancienne version sans disparition de l'historique ;
- [ ] corruption d'archive refusée avant tout remplacement local ;
- [ ] perte d'accès SAF Android expliquée sans suppression distante ;
- [ ] disparition locale et désélection sans suppression de version ;
- [ ] révocation d'un appareil effectivement refusée par l'API ;
- [ ] export relu sans RetroSave ;
- [ ] sauvegarde serveur restaurée et archive téléchargée depuis l'instance de
  reprise.

Les éléments non cochés restent des limites de l'alpha. Les résultats courants
sont publiés dans [IMPLEMENTATION_STATUS.md](../IMPLEMENTATION_STATUS.md).

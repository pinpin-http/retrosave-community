# RetroSave Community 0.1.0 alpha

Première version publique destinée aux essais. Elle réunit le client desktop
Qt/C++, le client Android Kotlin/Compose, le serveur FastAPI et neuf
connecteurs d'émulateurs.

Les paquets comprennent un APK Android signé, un installateur Windows x64,
une archive Windows portable et une archive Linux x64. Les empreintes SHA-256
sont fournies dans `SHA256SUMS.txt`.

Avant le premier essai, déployer le serveur avec le
[guide d'auto-hébergement](https://github.com/pinpin-http/retrosave-community/blob/main/docs/guides/SELF_HOSTING.md). Commencer avec une copie de
sauvegarde non critique et vérifier une synchronisation dans les deux sens.

Limites connues :

- l'installeur Windows n'a pas de signature d'éditeur et peut déclencher
  SmartScreen ;
- pas de chiffrement de bout en bout, de quota d'historique ou de mise à jour
  automatique ;
- interface en anglais ;
- validation automatisée étendue, mais recette multi-appareils réels encore à
  terminer.

# RetroSave Community — notices

Le code RetroSave est distribué sous AGPL-3.0 ; le texte complet accompagne
les distributions dans `LICENSE`. Sources et scripts de compilation de chaque
version : https://github.com/pinpin-http/retrosave-community/releases

Le desktop distribue des bibliothèques Qt 6.8.3 partagées (LGPL-3.0/GPL-3.0,
selon les modules) : https://www.qt.io/licensing/open-source-lgpl-obligations
Sources exactes et licences :
https://download.qt.io/archive/qt/6.8/6.8.3/single/
Les DLL/bibliothèques partagées peuvent être remplacées par une version modifiée
compatible. Les scripts de compilation se trouvent dans `desktop/CMakeLists.txt`
et `.github/workflows/desktop.yml`.

zstd 1.5.7 : Copyright Meta Platforms, Inc. and affiliates ; BSD-3-Clause/GPL-2.0.
Sources et licences : https://github.com/facebook/zstd/tree/v1.5.7

Android utilise Kotlin, AndroidX/Compose, Room et WorkManager (Apache-2.0),
OkHttp/Okio (Apache-2.0), kotlinx.serialization (Apache-2.0), Commons Compress
(Apache-2.0) et zstd-jni (BSD-2-Clause, zstd BSD-3-Clause).
Versions exactes : `android/gradle/libs.versions.toml` et `android/app/build.gradle.kts`.

Le serveur utilise les dépendances répertoriées dans `server/requirements.lock`.
Le déploiement autonome ajoute PostgreSQL, Caddy et MinIO, chacun sous sa
propre licence. MinIO : AGPL-3.0, sources du commit compilé :
https://github.com/minio/minio/tree/9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a

Les tests utilisent exclusivement des sauvegardes synthétiques. Aucune ROM,
aucun BIOS et aucune illustration commerciale ne sont fournis par RetroSave.

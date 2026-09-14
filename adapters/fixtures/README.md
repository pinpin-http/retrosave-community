# Fixtures d’adaptateurs M3

Ces arborescences reproduisent des structures réellement observées sous
Windows et Linux, avec noms et contenus de sauvegarde anonymisés.

- aucun fichier n’est une ROM ou un BIOS ;
- les extensions ROM présentes sont de simples sentinelles texte
  `DO_NOT_OPEN` utilisées pour prouver qu’un adaptateur ne les ouvre jamais ;
- les fichiers `*.ml*` et les dossiers d’état/SYSTEM/GAME servent de
  sentinelles d’exclusion ;
- les octets de sauvegarde sont synthétiques et sans donnée personnelle.

Les tests paramétrés de `desktop/tests/tst_adapters.cpp` sont la
spécification exécutable de ces fixtures.

# Ajouter un émulateur

**Dans le cas courant, il n'y a pas de code à écrire.** Un émulateur se décrit
dans un fichier `adapters/<id>.toml`, et le moteur de découverte du desktop
l'applique tel quel. Ce fichier est partagé avec les autres implémentations :
c'est le contrat, pas un réglage propre au desktop.

Ce guide décrit ce que le manifeste peut dire, ce qu'il ne peut pas dire, et
quoi faire dans le cas rare où il ne suffit pas.

## 1. Le fichier

```toml
id = "monemu"                       # identifiant technique, sans espace
name = "Mon Émulateur"              # nom affiché
process_names = ["monemu", "monemu.exe"]   # pour ne pas capturer en pleine partie
android_packages = ["com.exemple.monemu"]  # informatif côté desktop

[roots]
windows = ["%APPDATA%\\MonEmu\\saves"]     # chemins PROPOSÉS, jamais imposés
linux = ["~/.config/monemu/saves"]

[roots.validate]                    # facultatif : reconnaître une vraie racine
markers_primary = ["saves"]
markers_secondary = ["config", "log"]
markers_secondary_min = 2

[discovery]
unit_type = "file"                  # « file » ou « dir »
pattern = "**/*.sav"
exclude = ["**/*.state*"]
max_depth = 4                       # obligatoire, strictement positif

[identity]
unit_key = "filename"
game_key = "normalized_name"
```

Déposez-le dans `adapters/`, relancez les tests : il est chargé.

## 2. Les motifs

Un motif est relatif à la racine que l'utilisateur a désignée.

| Écriture | Sens |
|---|---|
| `*` | n'importe quoi **à l'intérieur d'un seul** dossier ou nom |
| `**` | zéro, un ou plusieurs niveaux de dossiers |
| texte | doit correspondre exactement |

La comparaison **ignore la casse** : `Sonic.SRM` et `sonic.srm` sont traités
pareil, comme le fait Windows et comme le fait déjà le client Python.

`max_depth` borne la descente. Ce n'est pas une optimisation : sans borne, une
arborescence profonde ferait tourner un scan sans fin chez l'utilisateur.

## 3. L'identité

Deux règles à choisir dans un vocabulaire **fermé**. Une valeur inconnue fait
échouer le chargement du manifeste — elle n'est jamais ignorée en silence.

`unit_key` — ce qui identifie une sauvegarde :

| Valeur | Sens |
|---|---|
| `filename` | le nom du fichier trouvé |
| `dirname` | le nom du dossier trouvé |
| `path_segment:N` | le N-ième segment capturé par un `*`, numéroté depuis 0 |

`game_key` — ce qui regroupe les sauvegardes d'un même jeu :

| Valeur | Sens |
|---|---|
| `normalized_name` | le nom sans extension, sans balises `(USA)` / `[b]`, en minuscules |
| `serial9` | les neuf premiers caractères de `unit_key` |
| `prefix:<texte>` | `<texte>` collé devant `unit_key` |

**Exemple de `path_segment`.** Azahar déclare :

```toml
pattern = "sdmc/Nintendo 3DS/*/*/title/00040000/*/data"
unit_key = "path_segment:2"
game_key = "prefix:3ds:"
```

Les trois `*` capturent, dans l'ordre, `id0`, `id1` et le *title ID*.
`path_segment:2` désigne donc le troisième, et `game_key` vaut `3ds:00abcdef`.

## 4. Ce qu'un manifeste ne peut pas faire

Ces garde-fous sont dans le code et **ne se lèvent pas** depuis un TOML :

- **Aucune ROM, aucun BIOS** (invariant I1). Une unité-fichier dont l'extension
  est `.nds .3ds .cci .cxi .app .iso .cso .chd .pbp .zip .7z .rar` est écartée,
  même si le motif la réclame explicitement. Un test le vérifie avec un
  manifeste écrit exprès pour tricher.
- **Aucun lien symbolique n'est suivi** : un lien pointant hors de la racine
  ferait sortir la découverte du périmètre désigné par l'utilisateur.
- **Les fichiers de travail de RetroSave** (`.rsc-*`, `*.rsc-bak`) ne sont
  jamais du contenu.

## 5. Le cas rare : quand le manifeste ne suffit pas

Si un émulateur demande une logique que le vocabulaire ne couvre pas — choisir
entre deux identités présentes sur le disque, lire un fichier de configuration —
implémentez `Adapter` et enregistrez une fabrique sous son identifiant :

```cpp
registry.registerFactory("monemu", [](const AdapterManifest &declared) {
    return std::unique_ptr<Adapter>(new MonEmuAdapter(declared));
});
```

Le reste du programme ne voit aucune différence : il demande un adaptateur par
son identifiant. Les autres émulateurs continuent d'être servis par le moteur
générique.

**Préférez toujours étendre le vocabulaire du manifeste** plutôt qu'écrire un
adaptateur sur mesure : ce qui est déclaré profite aux trois implémentations,
ce qui est codé ne profite qu'à une.

## 6. Vérifier

Ajoutez une arborescence d'exemple sous `adapters/fixtures/`, puis un cas dans
[tst_adapters.cpp](../../desktop/tests/tst_adapters.cpp). Les tests existants
utilisent les vrais manifestes et de vraies arborescences : c'est ce qui rend
la promesse « un TOML suffit » vérifiable plutôt que déclarative.

```sh
cmake --build desktop/build/dev --parallel 4
ctest --test-dir desktop/build/dev -R desktop-adapters --output-on-failure
```

# Identité visuelle — « Botanical Wellness & Glass »

Le desktop Qt/QML et l'application Android doivent se ressembler assez pour
qu'un joueur reconnaisse le même produit sur ses deux écrans. Cela ne s'obtient
pas en recopiant des couleurs : cela se vérifie.

## La source

Les composants suivis dans le dépôt et leurs tests font foi. Leurs valeurs
sont définies dans
[`design/tokens.json`](../../design/tokens.json), qui est la **seule**
référence partagée : couleurs, rayons, rythme, échelle typographique, durées de
mouvement, géométrie de la marque.

Les deux implémentations en sont des miroirs écrits à la main —
[`desktop/qml/Theme.qml`](../../desktop/qml/Theme.qml) et
[`RetroSaveTheme.kt`](../../android/app/src/main/kotlin/com/retrosave/ui/theme/RetroSaveTheme.kt) —
parce qu'une application ne doit pas dépendre d'un fichier de conception pour
démarrer. [`tests/test_design_tokens.py`](../../tests/test_design_tokens.py)
relit les deux et échoue dès qu'une valeur s'écarte : **59 cas**, vérifiés par
mutation.

Les jetons sont des **rôles Material 3**, ce qui rend la transposition Compose
directe (`lightColorScheme`).

## Ce que la DA impose

**Le thème est clair.** Fond porcelaine `#f6fbf5`, cartes **blanches** à grands
rayons, encre carbone `#181d1a`. La DA rompt explicitement avec le sombre
saturé — c'est écrit dans sa propre spécification.

**L'action principale est une gélule vert-noir** (`primary` `#07241a`), jamais
un aplat vif. Le vert vif `#4de082` ne sert qu'aux pastilles d'état vivantes.

**La hiérarchie tient à la matière, pas à la taille** : une carte blanche pleine,
une gélule sombre, une chip en porcelaine. Le verre dépoli est réservé aux
barres **flottantes** — latérale et supérieure —, jamais au fond d'une carte.

**Graisse plafonnée à 600.** Pas de noir gras sur les grands titres ; le test le
fait respecter des deux côtés.

**Le couple éditorial** : un mot ancré en gras suivi d'un mot en italique léger
dans un même titre — « Vos parties, *partout.* ». Il donne le ton sans ajouter
une couleur.

| Élément | Forme | Rôle |
|---|---|---|
| Carte | Blanche, rayon 24, ombre ambiante diffuse | l'ordinaire |
| Action principale | Gélule `primary`, encre blanche | une par carte |
| Action calme | Gélule porcelaine | à côté |
| État | Chip en gélule : verte, grise ou rouge | jamais la couleur seule |
| Mesure | Tuile : nom en petit, valeur en grand | un rapport |
| Barre latérale, barre haute | Verre dépoli, flottantes | la structure |

## Les vignettes

Une sauvegarde se reconnaît à son image, pas à sa clé. Deux cas, et seulement
deux :

1. **l'icône réelle**, quand la sauvegarde en porte une — `ICON0.PNG` vit dans
   le dossier PSP que l'utilisateur nous a confié. Elle n'est ni téléchargée ni
   envoyée au serveur, et **aucune ROM n'est ouverte** (invariant I1) ;
2. **le repli dessiné** : une teinte dérivée de la clé de jeu et une ou deux
   initiales tirées du libellé.

La teinte vient de la **clé**, pas du libellé : un renommage ne doit pas changer
la couleur, sinon on croit voir un autre jeu. Elle est calculée par le noyau
partagé (`core/gameart.*`, `game_art.py`, `GameArt.kt`) sur les **mêmes
vecteurs** — la recalculer en QML ferait une quatrième implémentation d'une
règle qui doit rendre le même résultat partout.

Les marques d'émulateur sont dessinées, et ce sont des **silhouettes, pas des
logos** : reproduire la marque d'un émulateur dans notre interface suggérerait
un partenariat qui n'existe pas.

## Le mouvement

Il explique, il ne décore pas. **Trois registres, et rien d'autre** :

- **survol, 140 ms** — imperceptible ; il confirme seulement qu'un élément répond ;
- **apparition, 320 ms décalée de 60 ms** — l'œil suit l'ordre de lecture au lieu
  de recevoir la page d'un bloc ;
- **état vivant, 1,8 s** — une connexion établie, un conflit qui attend, respirent
  lentement. **Ce qui ne bouge pas est ce qui ne demande rien** : c'est ainsi
  qu'on repère un conflit sans lire la barre entière.

Les durées sont des jetons, partagées et vérifiées par le test.

## Pièges rencontrés, à ne pas refaire

- **`onQuelqueChose` est un nom interdit en QML.** Tout identifiant `on` suivi
  d'une majuscule est lu comme un gestionnaire de signal, et le fichier ne
  charge plus. Or **tous** les rôles d'encre Material 3 commencent par « on ».
  Ils sont donc renommés côté Qt (`ink`, `inkMuted`, `inkInverse`,
  `inkOnContainer`, `inkAccent`, `inkAlert`), la correspondance est écrite dans
  `Theme.qml`, et un test interdit désormais la récidive.
- **Chaque fichier QML du module a besoin de son `QT_RESOURCE_ALIAS`**, sinon le
  `qmldir` généré pointe à côté et le type reste introuvable.
- **Ne jamais animer `x`/`y` dans un layout.** La position appartient au layout ;
  l'animer le met en désaccord avec lui et les cartes se chevauchent. L'entrée
  passe par une `Translate`, qui n'affecte pas la mise en page.
- **`MultiEffect` rend l'élément INVISIBLE sous le rendu logiciel.** L'ombre
  ambiante des cartes passait par un effet de couche ; sous
  `QT_QUICK_BACKEND=software` — machines sans GPU, bureaux distants,
  machines virtuelles — la carte disparaissait purement et simplement, sans le
  moindre avertissement. Géométrie correcte, opacité 1, et rien à l'écran.
  L'ombre est donc **dessinée** : trois rectangles arrondis empilés derrière la
  carte, de plus en plus pâles. Une ombre décorative ne vaut pas de faire
  disparaître le contenu chez une partie des utilisateurs.
- **Sur un poste où Qt journalise vers systemd, les erreurs QML ne sortent pas
  sur stderr** : elles se lisent avec `journalctl --user`. Le diagnostic a été
  rétabli dans `main.cpp` — brancher le signal `warnings` supprime l'affichage
  par défaut de Qt, il faut donc le réémettre soi-même.
- **Une capture d'écran doit attendre la fin des animations d'entrée**, sinon
  elle montre un état que personne ne verra. `--smoke-screenshot` laisse
  retomber la page avant de saisir.

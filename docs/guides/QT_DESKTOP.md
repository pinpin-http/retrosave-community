# Comprendre le desktop Qt, pas à pas

Ce guide accompagne le [prototype desktop](../../desktop/README.md).
Commencer par le faire compiler, puis suivre ce parcours. Les commentaires du
code sont en français et expliquent les responsabilités, la durée de vie des
objets et les mécanismes Qt ; ils ne paraphrasent pas chaque instruction C++.

## 0. Repères si vous venez de Java, C# ou Python

Les paradigmes objet se transposent presque tels quels. Ce qui change vraiment
tient en quatre points, et c'est là que se concentrent les erreurs.

### 0.1 Il n'y a pas de ramasse-miettes

En Java, C# ou Python, un objet vit tant que quelqu'un le référence. En C++,
**vous décidez** de sa durée de vie, et le projet utilise trois moyens :

| Écriture | Qui détruit l'objet | À utiliser quand |
|---|---|---|
| `AgentClient agent(endpoint);` | La fin du bloc, automatiquement | Par défaut. C'est le cas le plus simple et le plus sûr |
| `new QProcess(this)` | Le parent Qt, quand il meurt | L'objet doit survivre à la fonction qui le crée |
| `std::unique_ptr<QMenu>` | Le pointeur lui-même, quand il meurt | Propriété unique, sans parent Qt possible |

La destruction automatique en fin de bloc s'appelle **RAII**. C'est le
mécanisme que C# imite avec `using` et Python avec `with`, sauf qu'en C++ il
est la norme et non l'exception : un fichier se ferme, un verrou se rend et un
socket se libère parce que l'objet correspondant meurt.

Conséquence dans `main.cpp` : **l'ordre de déclaration est une décision de
conception**. Les objets meurent dans l'ordre inverse de leur création, donc le
moteur QML est déclaré en dernier pour être détruit en premier.

### 0.2 Un fichier `.h` et un fichier `.cpp` pour la même classe

Java et C# mettent tout dans un fichier ; leur compilateur explore le projet.
C++ compile chaque `.cpp` isolément et ne connaît que ce qu'on lui a inclus.
D'où la séparation : le `.h` déclare (« ce qui existe »), le `.cpp` définit
(« comment c'est fait »). `#include` n'est pas un `import` : c'est une copie
textuelle du fichier à cet endroit.

### 0.3 Signaux et slots remplacent la réflexion

C++ n'a aucune introspection à l'exécution. Qt la fabrique : la macro
`Q_OBJECT` fait passer l'en-tête par un générateur appelé **moc**, qui produit
un `.cpp` supplémentaire décrivant signaux et propriétés.

| Qt | Équivalent le plus proche |
|---|---|
| `signals: void changed();` | Un `event` C#, un listener Java sans interface à écrire |
| `connect(a, &A::sig, b, &B::slot)` | `a.sig += b.slot` en C#, `addListener` en Java |
| `Q_PROPERTY(... NOTIFY changed)` | Propriété C# + `INotifyPropertyChanged` |
| `Q_INVOKABLE` | Rendre une méthode visible depuis QML ; rien n'est exposé par défaut |

Oublier `Q_OBJECT` ne donne pas une erreur claire, mais un échec d'édition de
liens mentionnant une « vtable ». C'est presque toujours la cause.

### 0.4 Asynchrone sans threads

Toute l'application tourne dans **un seul fil d'exécution**, autour d'une
boucle d'événements. `connectToServer()` rend la main immédiatement et Qt
appellera plus tard le code branché sur `connected`. C'est le modèle de
`async/await` en C# ou d'`asyncio` en Python, écrit à la main sous forme de
signaux.

Deux conséquences : aucun verrou n'est nécessaire, et **aucune attente
bloquante n'est permise** dans l'interface. Les fonctions `waitForConnected` et
consorts n'apparaissent que dans les tests et au tout début du démarrage, avant
qu'une fenêtre existe.

### 0.5 Le piège des lambdas

Une lambda C++ dit explicitement ce qu'elle capture : `[this, socket]`.
Capturer `this` est utile et dangereux — si l'objet meurt avant que la lambda
ne s'exécute, on lit de la mémoire libérée. Java et C# n'ont pas ce risque.

La parade est systématique dans ce code : donner un **objet de contexte** en
troisième argument de `connect`. Qt débranche alors la connexion tout seul
quand ce contexte est détruit.

```cpp
connect(socket, &QLocalSocket::disconnected, this, [this, socket] { ... });
//                                           ^^^^ le contexte, jamais omis
```

### 0.6 Petit lexique

| Écriture | Sens |
|---|---|
| `const QString &nom` | Passage par référence sans copie, promesse de ne pas modifier |
| `QString nom() const` | Cette méthode ne modifie pas l'objet |
| `explicit` | Interdit une conversion implicite involontaire |
| `override` | Redéfinit une méthode virtuelle ; le compilateur vérifie |
| `auto` | Type déduit, comme `var` |
| `std::move(x)` | Autorise à voler le contenu de `x` au lieu de le copier |
| `enum class` | Énumération fortement typée, comme en Java/C# |
| `namespace { ... }` | Contenu privé à ce fichier |
| `#ifdef Q_OS_WIN` | Code supprimé avant compilation hors Windows |

## 1. Ce que recouvrent Qt, Qt Quick et QML

**Qt** est un ensemble de bibliothèques C++. Ici, Qt Core fournit les objets,
signaux, timers et processus ; Qt Network fournit le canal local. **Qt Quick**
dessine l'interface. **QML** décrit cette interface sous forme d'un arbre
d'objets : une fenêtre contient des layouts, labels et boutons. **Qt Widgets**
n'est présent que pour un usage : `QSystemTrayIcon` et son menu, seule voie
portable vers la zone de notification. Aucune vue Widgets n'est dessinée.

QML n'est pas une page web : aucun HTML, CSS ou navigateur n'est embarqué.
Les expressions de propriété et les petits gestionnaires d'événements utilisent
une syntaxe JavaScript. Le moteur de synchronisation restera en dehors de QML.

## 2. Les deux points d'entrée

Lire [app/main.cpp](../../desktop/src/app/main.cpp), puis
[agent/main.cpp](../../desktop/src/agent/main.cpp).

L'application graphique construit une `QApplication` (et non `QGuiApplication`,
car la zone de notification appartient à Qt Widgets), un `AgentClient`, une
`TrayShell`, des `Preferences` et un `QQmlApplicationEngine`. Ce dernier charge
`Main.qml` depuis les ressources compilées. `setInitialProperties` lui transmet
ces instances et la version. Avant tout cela, un `InstanceGuard` revendique le
canal de la fenêtre : si une interface existe déjà, ce processus la rappelle à
l'écran et se termine.

L'agent utilise `QCoreApplication` : il n'a besoin d'aucun écran. Les deux appels
`app.exec()` lancent une **boucle d'événements** : Qt attend du travail, puis
distribue connexions, réponses et timers aux objets concernés. Ce n'est pas une
boucle qui consomme en permanence un cœur CPU.

## 3. Une propriété C++ devient une information affichée

Lire [agentclient.h](../../desktop/src/app/agentclient.h) puis
[Main.qml](../../desktop/qml/Main.qml).

```cpp
Q_PROPERTY(QString state READ state NOTIFY changed)
```

- `state` est le nom visible depuis QML.
- `READ state` indique la méthode C++ qui renvoie la valeur.
- `NOTIFY changed` annonce le signal émis quand l'information change.
- L'absence de `WRITE` empêche QML de modifier directement cet état.

```qml
readonly property bool agentOnline: agent.state === "online"
```

Le `:` crée un **binding** : cette expression est réévaluée quand une dépendance
notifie un changement. Le label lié à `agentOnline` se met alors à jour sans
qu'on recherche le widget pour lui affecter un texte. Une affectation impérative
à une propriété liée peut casser ce binding ; garder l'état dans le C++ et
les calculs de présentation dans les expressions QML.

Le type `AgentConnection` est déclaré dans
[qmlregistrations.h](../../desktop/src/app/qmlregistrations.h). `QML_FOREIGN`
expose une classe déjà définie ailleurs ; `QML_UNCREATABLE` réserve sa création
au C++. `required property AgentConnection agent` rend cette dépendance
explicite et permet aux outils de vérifier ses propriétés.

## 4. Un clic devient une requête asynchrone

```qml
onClicked: window.agent.refresh()
```

`Q_INVOKABLE` autorise cet appel vers la méthode C++. `refresh()` ouvre une
connexion ; le signal `connected` déclenche l'envoi. `readyRead` annonce des
octets disponibles et `receive()` reconstitue la réponse. Un timer borne
l'attente. Pendant ce temps, l'interface reste utilisable.

Ne pas appeler `waitForConnected`, `waitForReadyRead` ou une longue opération
disque depuis le thread graphique. Les tests peuvent attendre un processus
distinct, mais ce n'est pas un modèle à recopier dans l'interface.

## 5. Signaux, slots et durée de vie

`connect(source, signal, destinataire, méthode)` relie un événement à une action.
Un signal n'est pas nécessairement un nouveau thread : ici, les callbacks
s'exécutent dans la boucle d'événements de leur processus.

Un `QObject` détruit ses enfants. Par exemple, le timer de délai serveur a le
socket comme parent et disparaît avec lui. `deleteLater()` attend un moment sûr
de la boucle d'événements pour détruire un objet utilisé dans un callback.
Les connexions avec contexte sont automatiquement débranchées quand ce contexte
est détruit. Toujours indiquer un contexte aux lambdas qui capturent un objet.

Les membres C++ automatiques ont leur propre durée de vie : dans `main.cpp`,
l'engine est construit après le client, donc détruit avant lui. QML ne conserve
ainsi jamais un pointeur vers un client déjà détruit.

## 6. La frontière entre les deux processus

Lire [protocol.cpp](../../desktop/src/ipc/protocol.cpp), puis
[localservice.cpp](../../desktop/src/ipc/localservice.cpp).

`LocalService` porte la mécanique commune : verrou, découpage des trames et
bornes. Le SENS des messages est passé en paramètre — `reply` pour l'agent,
`windowReply` pour la fenêtre. Deux canaux, deux vocabulaires, une seule
implémentation à auditer. Le canal de la fenêtre n'accepte donc pas les
commandes de l'agent, et réciproquement.

`QLocalServer` / `QLocalSocket` utilisent un socket local sous Linux et un named
pipe sous Windows. `UserAccessOption` limite l'accès à l'utilisateur. Cela ne
protège pas contre un logiciel malveillant déjà lancé avec les mêmes droits.

Un flux peut livrer une requête en plusieurs morceaux : on accumule jusqu'au
saut de ligne. La taille maximale et le délai empêchent une connexion de rester
ouverte indéfiniment ou d'accumuler des données sans limite. Le verrou
`QLockFile` empêche deux agents du prototype de revendiquer le même canal.

Un arrêt propre se lit dans le même fichier : l'agent écrit d'abord son accusé,
ferme son serveur, et ne quitte qu'à la fermeture effective de ce socket. Écrire
puis quitter immédiatement perdrait la réponse que le client attend encore.

Le contrat réseau et les limites générales sont résumés dans
[ARCHITECTURE.md](../../ARCHITECTURE.md). Un appel IPC réussi prouve la liaison
locale avec l'agent, pas le succès d'une synchronisation distante.

## 6 bis. Ce que le bureau a le droit de faire

Lire [trayshell.cpp](../../desktop/src/app/trayshell.cpp) et
[preferences.cpp](../../desktop/src/app/preferences.cpp).

Deux réflexes à garder pour la suite du produit :

- **Ne jamais afficher un réglage que le système n'a pas accepté.**
  `Preferences` relit l'état réel après chaque écriture, et expose le refus.
- **Ne jamais rendre une fenêtre introuvable.** Se cacher n'est permis que si le
  système déclare une zone de notification, et le canal de la fenêtre reste le
  filet quand cette déclaration ment.

`Preferences` retient un chemin de dossier et s'arrête là : existence, nature et
droits, jamais une énumération ni une ouverture. Un test choisit d'ailleurs un
dossier dont le contenu est illisible — il échouerait si le prototype cherchait
à le parcourir.

## 7. Ce que fait CMake

CMake joue le rôle de Maven/Gradle, de `dotnet build` ou de `pip` + `setup.py`,
avec une différence de taille : il ne télécharge rien. C++ n'a pas de dépôt de
paquets standard, donc CMake **cherche** les bibliothèques déjà présentes sur la
machine (`find_package`) au lieu de les récupérer. C'est pourquoi il faut
parfois lui indiquer où se trouve Qt avec `CMAKE_PREFIX_PATH`.

Il travaille en deux temps : la **configuration** inspecte la machine et génère
des fichiers de construction, puis la **construction** les exécute. D'où les
deux commandes distinctes, `cmake -S ... -B ...` puis `cmake --build ...`.

[CMakeLists.txt](../../desktop/CMakeLists.txt) définit les exécutables et leurs
dépendances. `qt_standard_project_setup` active notamment `AUTOMOC` : le programme
`moc` lit `Q_OBJECT` et produit les métadonnées nécessaires aux propriétés et
signaux. `qt_add_qml_module` enregistre les types et embarque les fichiers QML.

Les fichiers générés dans `desktop/build/` ne se modifient pas et ne se
committent pas. Pour ajouter un écran, créer son `.qml`, le déclarer dans
`QML_FILES`, reconstruire et lancer `all_qmllint`. Pour ajouter un état, modifier
le client C++ avec son signal et son test, puis le présenter dans QML.

## 8. Premier exercice de lecture

1. Lancer l'interface avec un nom de socket de test : elle affiche un agent absent.
2. Lancer l'agent avec le même nom : le contrôle passe à connecté.
3. Relancer une seconde interface avec le même nom : elle se termine aussitôt et
   la première revient au premier plan.
4. Fermer l'interface : l'agent reste vivant.
5. Rouvrir l'interface : elle retrouve le même PID.
6. Arrêter cet agent depuis le bouton : le message reste « arrêté à votre
   demande », y compris après les contrôles périodiques suivants, et la date du
   dernier succès est conservée.

## 9. Livrer l'application

Compiler ne suffit pas : le binaire va chercher les bibliothèques Qt là où il
les a trouvées à la compilation. Livrer, c'est produire une arborescence qui
embarque Qt et fonctionne ailleurs. `-DRSC_DEPLOY_QT=ON` la produit ; la
procédure et ses pièges — Qt officiel obligatoire, plugins Wayland à demander
explicitement — sont dans le [README du composant](../../desktop/README.md).

Le contrôle qui compte est de **déplacer** l'arbre avant de le lancer : c'est
ce qui distingue « ça marche sur ma machine » de « c'est livrable ».

## Références officielles

Le client HTTP prolonge ces notions avec `QNetworkAccessManager`,
`QNetworkReply`, annulation et durée de vie des appels.
Ses DTO `v0catalog.h` sont des valeurs copiables, sans QObject : une liste
valide n'est publiée qu'après décodage de toutes ses entrées. `std::optional`
représente un champ nullable, sans transformer une valeur invalide en absence.
Ses commentaires expliquent notamment pourquoi `finished` peut signifier un
conflit métier plutôt qu'un succès HTTP, et pourquoi annuler une requête ne
garantit pas que le serveur a annulé sa transaction.

- [Attributs C++ exposés à QML](https://doc.qt.io/qt-6/qtqml-cppintegration-exposecppattributes.html)
- [Modules QML et CMake](https://doc.qt.io/qt-6/qt-add-qml-module.html)
- [Serveur de communication locale](https://doc.qt.io/qt-6/qlocalserver.html)
- [Commandes aqt utilisées en CI](https://aqtinstall.readthedocs.io/en/stable/cli.html)
- [Requêtes réseau asynchrones](https://doc.qt.io/qt-6/qnetworkaccessmanager.html)
- [Lecture et cycle de vie d'une réponse](https://doc.qt.io/qt-6/qnetworkreply.html)

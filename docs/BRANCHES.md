# Branches et versions

`main` est la branche de développement de RetroSave Community. Elle contient
le serveur, les clients, le moteur, les connecteurs et leur documentation.

Les contributions partent d'une branche courte créée depuis `main` et
reviennent par pull request. Les correctifs de sécurité suivent le même chemin,
avec divulgation privée selon `SECURITY.md`.

Les versions publiques sont des tags signés au format
`vMAJEUR.MINEUR.CORRECTIF`, éventuellement suivis de `-alpha.N`. Le workflow
`Community release` ne crée une draft que lorsque les suites serveur, Android,
Linux et Windows réussissent. La draft est inspectée avant publication.

Le développement d'autres distributions ou services se fait dans des dépôts
séparés. Aucun secret, endpoint personnel, ressource de marque externe ou
historique privé ne doit entrer dans ce dépôt. Un correctif du moteur ou d'une
fonction communautaire doit être proposé ici afin que les utilisateurs
auto-hébergés en bénéficient.

L'anglais est la langue source des interfaces. Les commentaires expliquent les
mécanismes Qt, l'asynchronisme et les limites de sécurité ; ils ne servent pas de
journal de session.

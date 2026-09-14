# Scripts du dépôt

Exécuter les commandes depuis la racine du dépôt. Les scripts de développement
Windows recalculent cette racine depuis leur propre emplacement.

| Dossier | Usage | Point d’entrée |
|---|---|---|
| `dev/` | Préparer le poste Windows, charger son environnement, gérer l’émulateur | [setup-dev.ps1](dev/setup-dev.ps1), [dev-env.ps1](dev/dev-env.ps1), [doctor.ps1](dev/doctor.ps1) |
| `testing/` | Vérifier la documentation, les archives et le transport S3 | [check_docs.py](testing/check_docs.py), [test_archive_interop.py](testing/test_archive_interop.py), [test_s3_interop.py](testing/test_s3_interop.py) |
| `testing/` | Bancs deux clients et lecture croisée du format | [test_engine_interop.py](testing/test_engine_interop.py), [save_format.py](testing/save_format.py) |
| `data/` | Construire les vecteurs et la table de titres 3DS | [fetch_3ds_titles.py](data/fetch_3ds_titles.py) |
| `../deploy/` | Installer, sauvegarder et restaurer un serveur | [SELF_HOSTING.md](../docs/guides/SELF_HOSTING.md) |

```powershell
. .\scripts\dev\dev-env.ps1
.\scripts\dev\doctor.ps1
.\scripts\dev\start-emulator.ps1 -WaitForBoot
.\scripts\dev\stop-emulator.ps1
```

```sh
python scripts/testing/check_docs.py
uv run python scripts/testing/test_archive_interop.py
```

L’interopérabilité nécessite le poste Android configuré. Pour les opérations
serveur, utiliser les environnements et précautions du
[runbook](../docs/guides/RUNBOOK.md). `make test-server` crée une base
jetable : c'est le point d'entrée recommandé.

Le banc S3 est indépendant de PostgreSQL et d'Android. Il nécessite Docker et
un outil C++ construit, puis crée et détruit un MinIO isolé sans volume existant :

```sh
uv run python scripts/testing/test_s3_interop.py \
  --cpp desktop/build/dev/bin/retrosave-s3-interop
```

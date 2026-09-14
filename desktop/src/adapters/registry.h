// ─── Le point de branchement ──────────────────────────────────────────────
// Le registre charge tous les manifestes d'un dossier et construit un
// adaptateur par émulateur. Dans le cas courant il n'a rien à décider : le
// manifeste suffit, et c'est `ManifestAdapter` qui sert.
//
// Pour un émulateur qui sortirait du vocabulaire déclaratif, on enregistre une
// fabrique sous son identifiant AVANT le chargement. Le reste du programme ne
// voit pas la différence : il demande un adaptateur par son identifiant.
#pragma once

#include "adapters/discovery.h"

#include <QHash>
#include <QString>
#include <functional>
#include <memory>

namespace retrosave::adapters
{

class AdapterRegistry
{
  public:
    using Factory = std::function<std::unique_ptr<Adapter>(const AdapterManifest &)>;

    // À appeler avant `loadDirectory`. Remplace la fabrique générique pour
    // cet identifiant précis.
    void registerFactory(const QString &id, Factory factory);

    // Charge tous les `*.toml` du dossier. Un manifeste refusé n'empêche pas
    // les autres de se charger : son motif est consigné dans `problems()`,
    // parce qu'un émulateur cassé ne doit pas priver le joueur des trois autres.
    void loadDirectory(const QString &directory);

    QStringList ids() const;
    const Adapter *adapter(const QString &id) const;
    QStringList problems() const { return m_problems; }

  private:
    QHash<QString, Factory> m_factories;
    QHash<QString, std::shared_ptr<Adapter>> m_adapters;
    QStringList m_problems;
};

} // namespace retrosave::adapters

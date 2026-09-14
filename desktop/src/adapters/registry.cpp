#include "adapters/registry.h"

#include <QDir>

namespace retrosave::adapters
{

void AdapterRegistry::registerFactory(const QString &id, Factory factory)
{
    m_factories.insert(id, std::move(factory));
}

void AdapterRegistry::loadDirectory(const QString &directory)
{
    const QDir folder(directory);
    // Tri par nom : l'ordre de chargement ne doit pas dépendre du système de
    // fichiers, sinon deux machines listeraient les émulateurs différemment.
    for (const auto &entry : folder.entryList({"*.toml"}, QDir::Files, QDir::Name)) {
        const auto path = folder.filePath(entry);
        try {
            auto manifest = loadManifest(path);
            const auto id = manifest.id;
            if (m_adapters.contains(id)) {
                m_problems.append("identifiant en double : " + id);
                continue;
            }
            // Fabrique enregistrée si elle existe, adaptateur générique sinon.
            // C'est tout le mécanisme d'extension : une ligne, ici.
            auto factory = m_factories.value(id, [](const AdapterManifest &declared) {
                return std::unique_ptr<Adapter>(new ManifestAdapter(declared));
            });
            m_adapters.insert(id, std::shared_ptr<Adapter>(factory(manifest)));
        } catch (const ManifestError &error) {
            m_problems.append(QString::fromStdString(error.what()));
        }
    }
}

QStringList AdapterRegistry::ids() const
{
    auto keys = m_adapters.keys();
    keys.sort();
    return keys;
}

const Adapter *AdapterRegistry::adapter(const QString &id) const
{
    const auto found = m_adapters.constFind(id);
    return found == m_adapters.constEnd() ? nullptr : found->get();
}

} // namespace retrosave::adapters

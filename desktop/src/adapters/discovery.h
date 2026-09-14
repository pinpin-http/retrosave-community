// ─── Le moteur générique de découverte ────────────────────────────────────
// Il n'existe qu'UNE implémentation, et elle sert tous les émulateurs dont le
// manifeste suffit à décrire le comportement — c'est-à-dire les quatre
// existants. Ajouter un émulateur revient donc à déposer un TOML.
//
// Pour le cas rare qui sortirait du vocabulaire, l'interface `Adapter`
// ci-dessous reste le point d'extension : on enregistre une fabrique sous
// l'identifiant concerné (voir registry.h) sans toucher à ce moteur.
#pragma once

#include "adapters/manifest.h"

#include <QString>
#include <memory>
#include <vector>

namespace retrosave::adapters
{

// Extensions interdites par l'invariant I1 : RetroSave n'ouvre, ne hache et ne
// transfère JAMAIS une ROM ou un BIOS. La règle porte sur l'unité elle-même —
// un `DATA.BIN` à l'intérieur d'un dossier de sauvegarde PSP reste normal.
QStringList blockedUnitExtensions();

// Fichiers de travail de RetroSave : ils ne doivent jamais devenir du contenu.
bool isRetrosaveInternalName(const QString &name);

struct DiscoveredUnit {
    QString emulator;
    QString unitKey;
    QString unitType; // « file » ou « dir »
    QString gameKey;
    QString gameLabel;
    QString relPath;  // relatif à la racine configurée
    QString problem;  // vide si l'unité est exploitable
};

class Adapter
{
  public:
    virtual ~Adapter() = default;
    virtual QString id() const = 0;
    virtual const AdapterManifest &manifest() const = 0;
    // Ne lit AUCUN contenu de fichier : seulement des noms et des types.
    virtual std::vector<DiscoveredUnit> discover(const QString &rootPath) const = 0;
};

// L'adaptateur piloté par le manifeste, seul nécessaire aujourd'hui.
class ManifestAdapter final : public Adapter
{
  public:
    explicit ManifestAdapter(AdapterManifest manifest);
    QString id() const override { return m_manifest.id; }
    const AdapterManifest &manifest() const override { return m_manifest; }
    std::vector<DiscoveredUnit> discover(const QString &rootPath) const override;

  private:
    AdapterManifest m_manifest;
};

} // namespace retrosave::adapters

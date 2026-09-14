#include "network/v0client.h"
#include "network/v0json_p.h"

#include <QJsonArray>
#include <QSet>

namespace retrosave::network
{
using namespace json;
namespace
{
// isString/isNull AVANT conversion : toString() seul transforme aussi un champ
// absent ou mal typé en chaîne vide et ferait passer une réponse cassée pour vraie.
bool nullableText(const QJsonValue &value, std::optional<QString> &out, bool date = false)
{
    if (value.isNull()) {
        out.reset();
        return true;
    }
    if (!value.isString() || (date && !timestamp(value.toString())))
        return false;
    out = value.toString();
    return true;
}
std::optional<RemoteHead> readHead(const QJsonValue &value)
{
    if (!value.isObject())
        return {};
    const auto object = value.toObject();
    const auto number = integer(object.value("number"), 1);
    const auto size = integer(object.value("size_bytes"));
    const auto digest = object.value("content_sha256").toString();
    const auto date = object.value("created_at").toString();
    std::optional<QString> origin;
    if (!number || !size || !hash(digest) || !timestamp(date) ||
        !nullableText(object.value("origin_device_name"), origin))
        return {};
    return RemoteHead{*number, digest, *size, date, origin};
}
std::optional<RemoteUnit> readUnit(const QJsonValue &value)
{
    if (!value.isObject())
        return {};
    const auto object = value.toObject();
    const auto number = integer(object.value("head_version"));
    UnitDeclaration identity{
        object.value("emulator").toString(), object.value("unit_key").toString(),
        object.value("unit_type").toString(), object.value("game_key").toString(),
        object.value("game_label").toString()};
    const auto id = object.value("id").toString();
    const auto source = object.value("label_source").toString();
    const auto state = object.value("state").toString();
    const auto date = object.value("updated_at").toString();
    if (!number || !uuid(id) || !emulator(identity.emulator) || !unitType(identity.unitType) ||
        !object.value("unit_key").isString() || !object.value("game_key").isString() ||
        !object.value("game_label").isString() || (source != "auto" && source != "user") ||
        (state != "active" && state != "missing") || !timestamp(date))
        return {};
    auto head = readHead(object.value("head"));
    // Une tête malformée n'est JAMAIS une unité neuve (base=0) : cela pourrait
    // déclencher un PUSH injustifié. La cohérence est vérifiée avant le résultat.
    if ((*number == 0 && !object.value("head").isNull()) ||
        (*number != 0 && (!head || head->number != *number)))
        return {};
    // Une adresse malformée est simplement ignorée : une jaquette ne fait
    // jamais échouer la lecture d'une unité.
    auto artwork = object.value("artwork_url").toString();
    if (!artwork.startsWith("https://"))
        artwork.clear();
    return RemoteUnit{id, identity, source, *number, state, date, head, artwork};
}
std::optional<RemoteDevice> readDevice(const QJsonValue &value)
{
    if (!value.isObject())
        return {};
    const auto object = value.toObject();
    const auto id = object.value("id").toString();
    const auto platform = object.value("os").toString();
    std::optional<QString> seen;
    std::optional<QString> revoked;
    if (!uuid(id) || !os(platform) || !object.value("name").isString() ||
        !nullableText(object.value("last_seen_at"), seen, true))
        return {};
    // Champ additif : un serveur qui ne le connaît pas reste utilisable. Mal
    // formé, en revanche, il est refusé — mieux vaut ne rien afficher que
    // laisser croire qu'un appareil est actif alors qu'il est révoqué.
    if (object.contains("revoked_at") && !nullableText(object.value("revoked_at"), revoked, true))
        return {};
    return RemoteDevice{id, object.value("name").toString(), platform, seen, revoked};
}
std::optional<RemoteConflict> readConflict(const QJsonValue &value)
{
    if (!value.isObject())
        return {};
    const auto object = value.toObject();
    const auto first = readHead(object.value("version_a"));
    const auto second = readHead(object.value("version_b"));
    const auto id = object.value("id").toString();
    const auto unitId = object.value("unit_id").toString();
    const auto date = object.value("created_at").toString();
    if (!uuid(id) || !uuid(unitId) || !first || !second || first->number == second->number ||
        !object.value("unit_label").isString() || !timestamp(date))
        return {};
    return RemoteConflict{id, unitId, object.value("unit_label").toString(), *first, *second, date};
}
std::optional<RemoteVersion> readVersion(const QJsonValue &value)
{
    auto summary = readHead(value);
    if (!summary)
        return {};
    const auto object = value.toObject();
    const auto kind = object.value("kind").toString();
    std::optional<QString> mtime;
    std::optional<qint64> parent;
    if (!object.value("parent_number").isNull()) {
        parent = integer(object.value("parent_number"), 1);
        if (!parent)
            return {};
    }
    if ((kind != "normal" && kind != "restore" && kind != "conflict_branch") ||
        !nullableText(object.value("client_mtime"), mtime, true))
        return {};
    return RemoteVersion{*summary, kind, mtime, parent};
}
// Une liste est validée ENTIÈREMENT avant publication. Ignorer une ligne invalide
// ferait croire à une disparition d'unité ou à l'absence d'un conflit.
template <typename T, typename Reader>
std::optional<QList<T>> readList(const QJsonValue &value, Reader reader)
{
    if (!value.isArray())
        return {};
    QList<T> items;
    QSet<QString> ids;
    for (const auto &entry : value.toArray()) {
        const auto item = reader(entry);
        if (!item || ids.contains(item->id.toLower()))
            return {};
        ids.insert(item->id.toLower());
        items.append(*item);
    }
    return items;
}
} // namespace

ApiCall *V0Client::health()
{
    return send(Operation::Health, {}, 0, {}, {}, true);
}
ApiCall *V0Client::registerDevice(const DeviceRegistration &device, const QByteArray &key)
{
    return send(Operation::RegisterDevice, {}, 0,
                {{"name", device.name}, {"os", device.os}, {"app_version", device.appVersion}}, key,
                os(device.os));
}
ApiCall *V0Client::listDevices()
{
    return send(Operation::Devices, {}, 0, {}, {}, true);
}
ApiCall *V0Client::renameDevice(const QString &deviceId, const QString &name,
                                const QByteArray &key)
{
    // Le nom est borné ici aussi : le serveur le refuserait, mais une requête
    // qu'on sait invalide n'a pas à partir.
    const auto trimmed = name.trimmed();
    return send(Operation::RenameDevice, deviceId, 0, QJsonObject{{"name", trimmed}}, key,
                !trimmed.isEmpty() && trimmed.size() <= 64);
}

ApiCall *V0Client::revokeDevice(const QString &deviceId, const QByteArray &key)
{
    return send(Operation::RevokeDevice, deviceId, 0, {}, key, true);
}

ApiCall *V0Client::listUnits()
{
    return send(Operation::Units, {}, 0, {}, {}, true);
}
ApiCall *V0Client::createUnit(const UnitDeclaration &unit, const QByteArray &key)
{
    return send(Operation::CreateUnit, {}, 0,
                {{"emulator", unit.emulator},
                 {"unit_key", unit.unitKey},
                 {"unit_type", unit.unitType},
                 {"game_key", unit.gameKey},
                 {"game_label", unit.gameLabel}},
                key, emulator(unit.emulator) && unitType(unit.unitType));
}
ApiCall *V0Client::renameUnit(const QString &id, const QString &label, const QByteArray &key)
{
    return send(Operation::Rename, id, 0, {{"game_label", label}}, key, true);
}
ApiCall *V0Client::history(const QString &id)
{
    return send(Operation::History, id, 0, {}, {}, true);
}
ApiCall *V0Client::restore(const QString &id, qint64 version, const QByteArray &key)
{
    return send(Operation::Restore, id, 0, {{"version", version}}, key,
                version > 0 && safeInteger(version));
}
ApiCall *V0Client::markMissing(const QString &id, const QByteArray &key)
{
    return send(Operation::Missing, id, 0, {}, key, true);
}
ApiCall *V0Client::listConflicts()
{
    return send(Operation::Conflicts, {}, 0, {}, {}, true);
}
ApiCall *V0Client::resolveConflict(const QString &id, qint64 winner, const QByteArray &key)
{
    return send(Operation::Resolve, id, 0, {{"winner", winner}}, key,
                winner > 0 && safeInteger(winner));
}

ApiResult V0Client::decodeCatalog(Operation operation, int status, const QJsonObject &object)
{
    ApiResult result;
    result.httpStatus = status;
    result.failure = Failure::InvalidResponse;
    const bool accepted = operation == Operation::RegisterDevice ? status == 201
                          : operation == Operation::CreateUnit   ? status == 200 || status == 201
                                                                 : status == 200;
    if (!accepted)
        return result;
    switch (operation) {
    case Operation::Health: {
        std::optional<QString> version;
        if (!object.value("status").isString() || !object.value("db").isBool() ||
            !object.value("s3").isBool() ||
            (object.contains("version") && !nullableText(object.value("version"), version)))
            return result;
        result.payload = Health{object.value("status").toString(), object.value("db").toBool(),
                                object.value("s3").toBool(), version};
        break;
    }
    case Operation::RegisterDevice: {
        const auto id = object.value("device_id").toString();
        if (!uuid(id))
            return result;
        result.payload = RegisteredDevice{id};
        break;
    }
    case Operation::Devices: {
        const auto items = readList<RemoteDevice>(object.value("devices"), readDevice);
        if (!items)
            return result;
        result.payload = Devices{*items};
        break;
    }
    case Operation::RenameDevice:
    case Operation::RevokeDevice: {
        // Les deux routes rendent l'appareil lui-même, à plat. L'interface
        // relira la liste ensuite ; ce retour sert à confirmer l'effet du
        // geste, pas à reconstruire un état.
        const auto device = readDevice(object);
        if (!device)
            return result;
        result.payload = *device;
        break;
    }
    case Operation::Units: {
        const auto items = readList<RemoteUnit>(object.value("units"), readUnit);
        if (!items)
            return result;
        result.payload = Units{*items};
        break;
    }
    case Operation::CreateUnit:
    case Operation::Rename: {
        const auto unit = readUnit(object.value("unit"));
        if (!unit)
            return result;
        result.payload = *unit;
        break;
    }
    case Operation::History: {
        const auto number = integer(object.value("head_version"));
        if (!number || !object.value("versions").isArray())
            return result;
        History history{{}, *number};
        QSet<qint64> numbers;
        for (const auto &entry : object.value("versions").toArray()) {
            const auto version = readVersion(entry);
            if (!version || numbers.contains(version->summary.number))
                return result;
            numbers.insert(version->summary.number);
            history.versions.append(*version);
        }
        // La tête explicite fait foi, même si une branche plus récente existe.
        if ((*number == 0 && !numbers.isEmpty()) || (*number > 0 && !numbers.contains(*number)))
            return result;
        result.payload = history;
        break;
    }
    case Operation::Conflicts: {
        const auto items = readList<RemoteConflict>(object.value("conflicts"), readConflict);
        if (!items)
            return result;
        result.payload = Conflicts{*items};
        break;
    }
    case Operation::Resolve: {
        const auto id = object.value("unit_id").toString();
        const auto number = integer(object.value("head_version"), 1);
        if (!uuid(id) || !number)
            return result;
        result.payload = ResolvedConflict{id, *number};
        break;
    }
    case Operation::Missing:
        if (object.value("state") != QJsonValue("missing"))
            return result;
        result.payload = Missing{};
        break;
    default:
        return result;
    }
    result.failure = Failure::None;
    return result;
}
} // namespace retrosave::network

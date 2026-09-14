#include "core/archive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <zstd.h>

namespace retrosave::core
{
namespace
{

// Décalages du bloc d'en-tête tar, 512 octets, format POSIX ustar. Ces
// positions sont figées depuis les années 1980 : elles ne se négocient pas.
constexpr qsizetype OffName = 0, LenName = 100;
constexpr qsizetype OffMode = 100, OffUid = 108, OffGid = 116;
constexpr qsizetype OffSize = 124, OffMtime = 136, OffChksum = 148;
constexpr qsizetype OffTypeflag = 156, OffMagic = 257, OffVersion = 263;

// Champ numérique tar : de l'octal ASCII, complété à gauche par des zéros et
// terminé par un NUL. « 5 » sur 12 octets donne « 00000000005\0 ».
QByteArray octalField(quint64 value, qsizetype width)
{
    auto text = QByteArray::number(value, 8);
    return text.rightJustified(width - 1, '0') + '\0';
}

void put(QByteArray &block, qsizetype offset, const QByteArray &value)
{
    // `replace` écrit sur place : le bloc garde ses 512 octets.
    block.replace(offset, value.size(), value);
}

// Somme de contrôle historique : la somme non signée de tous les octets de
// l'en-tête, le champ de somme lui-même étant compté comme huit espaces.
QByteArray checksumField(const QByteArray &block)
{
    quint64 sum = 0;
    for (qsizetype i = 0; i < block.size(); ++i) {
        const bool inChecksum = i >= OffChksum && i < OffChksum + 8;
        sum += inChecksum ? ' ' : static_cast<unsigned char>(block.at(i));
    }
    // Forme émise par les producteurs usuels : six chiffres, NUL, espace.
    return QByteArray::number(sum, 8).rightJustified(6, '0') + QByteArray("\0 ", 2);
}

// Un nom non ASCII ne tient pas dans le champ historique. On y met un repli
// lisible et le vrai nom part dans un en-tête étendu PAX, comme le fait le
// module `tarfile` de Python (§5.3, règle 7).
bool needsPaxHeader(const QString &name)
{
    return name.toUtf8().size() > LenName ||
           std::any_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() > 127; });
}

QByteArray asciiFallback(const QString &name)
{
    QByteArray fallback;
    for (const QChar c : name)
        fallback += c.unicode() > 127 ? '?' : static_cast<char>(c.unicode());
    return fallback.left(LenName);
}

// Un enregistrement PAX déclare sa propre longueur, laquelle fait partie de
// l'enregistrement : « 17 path=a/é.dat\n » fait bien 17 octets. On itère
// jusqu'au point fixe, parce qu'ajouter un chiffre peut changer la longueur.
QByteArray paxRecord(const QByteArray &keyValue)
{
    qsizetype length = keyValue.size() + 2; // au moins « N » et l'espace
    for (;;) {
        const auto candidate = QByteArray::number(length) + " " + keyValue;
        if (candidate.size() == length)
            return candidate;
        length = candidate.size();
    }
}

QByteArray header(const QByteArray &name, quint64 size, char typeflag, quint64 mode)
{
    QByteArray block(TarBlockSize, '\0');
    put(block, OffName, name.left(LenName));
    put(block, OffMode, octalField(mode, 8));
    put(block, OffUid, octalField(0, 8));
    put(block, OffGid, octalField(0, 8));
    put(block, OffSize, octalField(size, 12));
    // mtime figé à zéro : c'est ce qui rend l'archive reproductible. Une date
    // de fichier ferait changer les octets sans que le contenu bouge.
    put(block, OffMtime, octalField(0, 12));
    block[OffTypeflag] = typeflag;
    put(block, OffMagic, QByteArray("ustar\0", 6));
    put(block, OffVersion, QByteArray("00", 2));
    // uname, gname, devmajor, devminor et prefix restent nuls : aucun nom
    // d'utilisateur ne doit fuir dans une archive de sauvegarde.
    put(block, OffChksum, checksumField(block));
    return block;
}

void appendPadded(QByteArray &out, const QByteArray &payload)
{
    out += payload;
    const auto remainder = payload.size() % TarBlockSize;
    if (remainder != 0)
        out += QByteArray(TarBlockSize - remainder, '\0');
}

QByteArray sortKeyUtf8(const QString &relPath)
{
    return relPath.toUtf8();
}

bool byteOrderLess(const QByteArray &left, const QByteArray &right)
{
    return std::lexicographical_compare(
        reinterpret_cast<const unsigned char *>(left.constData()),
        reinterpret_cast<const unsigned char *>(left.constData() + left.size()),
        reinterpret_cast<const unsigned char *>(right.constData()),
        reinterpret_cast<const unsigned char *>(right.constData() + right.size()));
}

void requireSafeName(const QString &name, std::vector<QString> &seen)
{
    const auto components = name.split('/');
    const bool unsafe =
        name.isEmpty() || name.startsWith('/') || name.contains('\\') ||
        std::any_of(components.cbegin(), components.cend(), [](const QString &component) {
            return component.isEmpty() || component == "." || component == "..";
        });
    if (unsafe)
        throw ArchiveError("entrée d'archive dangereuse : " + name.toStdString());
    if (std::find(seen.cbegin(), seen.cend(), name) != seen.cend())
        throw ArchiveError("entrée d'archive en double : " + name.toStdString());
    seen.push_back(name);
}

quint64 parseOctal(const QByteArray &field)
{
    // Les champs sont complétés par des zéros, des NUL ou des espaces selon
    // les producteurs. On coupe au premier caractère non octal.
    quint64 value = 0;
    for (const char c : field) {
        if (c < '0' || c > '7')
            break;
        value = value * 8 + static_cast<quint64>(c - '0');
    }
    return value;
}


// La mémoire utilisée ne dépend pas de la taille de l'unité : compression et
// décompression travaillent par tranches de 64 Kio.
//
// Ce sont des classes plutôt que des fonctions parce qu'elles possèdent une
// ressource C (le contexte zstd) qu'il faut libérer même si une exception
// traverse. Le destructeur s'en charge selon le principe RAII.

constexpr qint64 StreamChunk = 64 * 1024;

class StreamCompressor
{
  public:
    explicit StreamCompressor(QIODevice &destination) : m_destination(destination)
    {
        m_context = ZSTD_createCCtx();
        if (m_context == nullptr)
            throw ArchiveError("zstd : contexte de compression indisponible");
        // Mêmes réglages que la voie tamponnée : c'est le contrat Q25, pas un
        // choix local. Deux modes d'écriture différents seraient un piège.
        ZSTD_CCtx_setParameter(m_context, ZSTD_c_compressionLevel, ZstdLevel);
        ZSTD_CCtx_setParameter(m_context, ZSTD_c_contentSizeFlag, 0);
        ZSTD_CCtx_setParameter(m_context, ZSTD_c_checksumFlag, 1);
        ZSTD_CCtx_setParameter(m_context, ZSTD_c_dictIDFlag, 0);
        ZSTD_CCtx_setParameter(m_context, ZSTD_c_nbWorkers, 0);
        m_out.resize(static_cast<qsizetype>(ZSTD_CStreamOutSize()));
    }

    // Ni copie ni affectation : deux objets libéreraient le même contexte.
    StreamCompressor(const StreamCompressor &) = delete;
    StreamCompressor &operator=(const StreamCompressor &) = delete;
    ~StreamCompressor() { ZSTD_freeCCtx(m_context); }

    void write(const char *data, qint64 size) { push(data, size, ZSTD_e_continue); }
    void finish() { push(nullptr, 0, ZSTD_e_end); }

    // `result()` rend l'empreinte sans réinitialiser l'objet : on peut donc
    // l'appeler après `finish()` sans avoir gardé une copie des octets.
    QString digest() { return QString::fromLatin1(m_hash.result().toHex()); }
    qint64 total() const { return m_total; }

  private:
    void push(const char *data, qint64 size, ZSTD_EndDirective mode)
    {
        ZSTD_inBuffer input{data, static_cast<size_t>(size), 0};
        for (;;) {
            ZSTD_outBuffer output{m_out.data(), static_cast<size_t>(m_out.size()), 0};
            const auto remaining = ZSTD_compressStream2(m_context, &output, &input, mode);
            if (ZSTD_isError(remaining))
                throw ArchiveError(std::string("zstd : ") + ZSTD_getErrorName(remaining));
            if (output.pos > 0) {
                const auto written =
                    m_destination.write(m_out.constData(), static_cast<qint64>(output.pos));
                if (written != static_cast<qint64>(output.pos))
                    throw ArchiveError("écriture de l'archive incomplète");
                // On empreinte l'archive AU PASSAGE : la relire ensuite pour
                // la hacher coûterait une seconde traversée de 256 Mo.
                m_hash.addData(QByteArrayView(m_out.constData(), static_cast<qsizetype>(output.pos)));
                m_total += written;
            }
            if (mode == ZSTD_e_end ? remaining == 0 : input.pos == input.size)
                return;
        }
    }

    QIODevice &m_destination;
    ZSTD_CCtx *m_context = nullptr;
    QByteArray m_out;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_total = 0;
};

class StreamDecompressor
{
  public:
    explicit StreamDecompressor(QIODevice &source) : m_source(source)
    {
        m_context = ZSTD_createDCtx();
        if (m_context == nullptr)
            throw ArchiveError("zstd : contexte de décompression indisponible");
        m_in.resize(static_cast<qsizetype>(ZSTD_DStreamInSize()));
    }

    StreamDecompressor(const StreamDecompressor &) = delete;
    StreamDecompressor &operator=(const StreamDecompressor &) = delete;
    ~StreamDecompressor() { ZSTD_freeDCtx(m_context); }

    // Renvoie le nombre d'octets réellement produits ; 0 signale la fin.
    qint64 read(char *out, qint64 size)
    {
        ZSTD_outBuffer output{out, static_cast<size_t>(size), 0};
        while (output.pos < output.size && !m_finished) {
            if (m_input.pos == m_input.size) {
                const auto got = m_source.read(m_in.data(), m_in.size());
                if (got <= 0) {
                    // Le fichier s'arrête avant la fin de la trame.
                    throw ArchiveError("trame zstd incomplète");
                }
                m_input = {m_in.constData(), static_cast<size_t>(got), 0};
            }
            const auto status = ZSTD_decompressStream(m_context, &output, &m_input);
            if (ZSTD_isError(status))
                throw ArchiveError("trame zstd corrompue");
            if (status == 0)
                m_finished = true;
        }
        return static_cast<qint64>(output.pos);
    }

    // Lit exactement `size` octets, ou signale que le flux s'est arrêté avant.
    bool readExactly(char *out, qint64 size)
    {
        qint64 filled = 0;
        while (filled < size) {
            const auto got = read(out + filled, size - filled);
            if (got == 0)
                return false;
            filled += got;
        }
        return true;
    }

  private:
    QIODevice &m_source;
    ZSTD_DCtx *m_context = nullptr;
    QByteArray m_in;
    ZSTD_inBuffer m_input{nullptr, 0, 0};
    bool m_finished = false;
};

// Un chemin d'archive peut contenir des sous-dossiers. Le dossier d'attente,
// lui, reste PLAT : aucun dossier n'y est créé à partir d'un nom venu de
// l'archive. L'empreinte du chemin suffit à distinguer deux entrées.
QString stagedName(const QString &relPath)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(relPath.toUtf8(), QCryptographicHash::Sha256).toHex());
}

} // namespace

QByteArray createTar(std::vector<ContentFile> files)
{
    std::sort(files.begin(), files.end(), [](const ContentFile &a, const ContentFile &b) {
        return byteOrderLess(sortKeyUtf8(a.relPath), sortKeyUtf8(b.relPath));
    });
    std::vector<QString> seen;
    for (const auto &file : files)
        requireSafeName(file.relPath, seen);

    QByteArray out;
    for (const auto &file : files) {
        const auto utf8Name = file.relPath.toUtf8();
        if (needsPaxHeader(file.relPath)) {
            const auto records = paxRecord("path=" + utf8Name + "\n");
            // Mode 0 et nom conventionnel : ce bloc décrit le suivant, ce
            // n'est pas un fichier que quelqu'un extraira.
            out += header("././@PaxHeader", static_cast<quint64>(records.size()), 'x', 0);
            appendPadded(out, records);
        }
        const auto storedName = needsPaxHeader(file.relPath) ? asciiFallback(file.relPath) : utf8Name;
        out += header(storedName, static_cast<quint64>(file.content.size()), '0', 0644);
        appendPadded(out, file.content);
    }

    // Fin d'archive : deux blocs nuls, puis remplissage jusqu'au multiple de
    // 10 240. Sans ce remplissage, certains lecteurs signalent une troncature.
    out += QByteArray(2 * TarBlockSize, '\0');
    const auto remainder = out.size() % TarRecordSize;
    if (remainder != 0)
        out += QByteArray(TarRecordSize - remainder, '\0');
    return out;
}

std::vector<ContentFile> readTar(const QByteArray &raw)
{
    if (raw.size() % TarBlockSize != 0)
        throw ArchiveError("archive tar de taille non alignée");

    std::vector<ContentFile> files;
    std::vector<QString> seen;
    QString pendingPath; // nom fourni par un en-tête PAX pour l'entrée suivante
    qsizetype offset = 0;

    while (offset + TarBlockSize <= raw.size()) {
        const auto block = raw.mid(offset, TarBlockSize);
        offset += TarBlockSize;
        if (block == QByteArray(TarBlockSize, '\0'))
            break; // premier bloc nul : fin d'archive

        const auto stored = checksumField(block);
        if (block.mid(OffChksum, 8) != stored)
            throw ArchiveError("somme de contrôle d'en-tête tar invalide");

        const auto size = parseOctal(block.mid(OffSize, 12));
        const auto payloadBlocks =
            static_cast<qsizetype>((size + TarBlockSize - 1) / TarBlockSize) * TarBlockSize;
        if (offset + payloadBlocks > raw.size())
            throw ArchiveError("entrée d'archive tronquée");
        const auto payload = raw.mid(offset, static_cast<qsizetype>(size));
        offset += payloadBlocks;

        const char typeflag = block.at(OffTypeflag);
        if (typeflag == 'x' || typeflag == 'X') {
            // En-tête étendu : on n'en retient que « path ». Les
            // enregistrements de temps n'existent pas dans nos archives, et
            // une taille PAX supposerait un fichier de plus de 8 Gio.
            qsizetype cursor = 0;
            while (cursor < payload.size()) {
                const auto space = payload.indexOf(' ', cursor);
                if (space < 0)
                    throw ArchiveError("enregistrement PAX illisible");
                const auto length = payload.mid(cursor, space - cursor).toLongLong();
                if (length <= 0 || cursor + length > payload.size())
                    throw ArchiveError("longueur d'enregistrement PAX invalide");
                const auto record = payload.mid(space + 1, cursor + length - space - 2);
                if (record.startsWith("path="))
                    pendingPath = QString::fromUtf8(record.mid(5));
                cursor += length;
            }
            continue;
        }
        if (typeflag != '0' && typeflag != '\0')
            throw ArchiveError("type d'entrée tar non pris en charge");

        auto name = pendingPath;
        pendingPath.clear();
        if (name.isEmpty()) {
            const auto rawName = block.mid(OffName, LenName);
            name = QString::fromUtf8(rawName.left(rawName.indexOf('\0') < 0 ? rawName.size()
                                                                            : rawName.indexOf('\0')));
        }
        requireSafeName(name, seen);
        files.push_back({name, payload});
    }
    return files;
}

ArchiveBlob createArchive(const QString &unitType, std::vector<ContentFile> files)
{
    const auto content = contentSha256(unitType, files);
    qint64 total = 0;
    for (const auto &file : files)
        total += file.content.size();
    const auto raw = createTar(std::move(files));

    // ZSTD_CCtx est un contexte de compression : on l'alloue, on le paramètre,
    // on l'utilise, on le libère. C'est une API C, sans destructeur — d'où le
    // soin apporté à ne pas sortir de la fonction sans libérer.
    ZSTD_CCtx *context = ZSTD_createCCtx();
    if (context == nullptr)
        throw ArchiveError("zstd : contexte de compression indisponible");
    QByteArray compressed(static_cast<qsizetype>(ZSTD_compressBound(raw.size())), '\0');
    // Q25 : taille décompressée ABSENTE partout, checksum de trame activé,
    // pas d'identifiant de dictionnaire, un seul fil. C'est le seul mode
    // qu'un producteur en flux peut tenir, donc le seul que l'on émet.
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, ZstdLevel);
    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0);
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setParameter(context, ZSTD_c_dictIDFlag, 0);
    ZSTD_CCtx_setParameter(context, ZSTD_c_nbWorkers, 0);
    const auto written = ZSTD_compress2(context, compressed.data(),
                                        static_cast<size_t>(compressed.size()), raw.constData(),
                                        static_cast<size_t>(raw.size()));
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(written))
        throw ArchiveError(std::string("zstd : ") + ZSTD_getErrorName(written));
    compressed.truncate(static_cast<qsizetype>(written));

    return {compressed, content,
            QString::fromLatin1(
                QCryptographicHash::hash(compressed, QCryptographicHash::Sha256).toHex()),
            total};
}

std::vector<ContentFile> extractArchive(const QByteArray &archive, const QString &unitType,
                                        const QString &expectedContentSha256)
{
    // On décompresse EN FLUX et non avec ZSTD_decompress : ce dernier exige
    // que la trame déclare sa taille décompressée, ce que nos producteurs
    // n'écrivent pas. Un lecteur n'a pas à réclamer un champ facultatif.
    ZSTD_DCtx *context = ZSTD_createDCtx();
    if (context == nullptr)
        throw ArchiveError("zstd : contexte de décompression indisponible");
    QByteArray raw;
    QByteArray window(static_cast<qsizetype>(ZSTD_DStreamOutSize()), '\0');
    ZSTD_inBuffer input{archive.constData(), static_cast<size_t>(archive.size()), 0};
    size_t status = 0;
    while (input.pos < input.size) {
        ZSTD_outBuffer output{window.data(), static_cast<size_t>(window.size()), 0};
        status = ZSTD_decompressStream(context, &output, &input);
        if (ZSTD_isError(status)) {
            ZSTD_freeDCtx(context);
            throw ArchiveError("trame zstd corrompue");
        }
        raw += QByteArray(window.constData(), static_cast<qsizetype>(output.pos));
    }
    ZSTD_freeDCtx(context);
    if (status != 0)
        throw ArchiveError("trame zstd incomplète");

    auto files = readTar(raw);
    // Le verdict tombe ICI, avant que quoi que ce soit ne soit écrit chez
    // l'utilisateur : une archive refusée ne modifie aucun fichier.
    if (contentSha256(unitType, files) != expectedContentSha256)
        throw ArchiveError("l'identité de contenu de l'archive ne correspond pas");
    return files;
}

ArchiveInfo createArchiveFrom(const QString &unitType, std::vector<ArchiveSource> entries,
                              const QString &destination)
{
    if (unitType != "file" && unitType != "dir")
        throw ArchiveError("type d'unité inconnu : " + unitType.toStdString());
    if (unitType == "file" && entries.size() != 1)
        throw ArchiveError("une unité-fichier contient exactement un fichier");

    std::sort(entries.begin(), entries.end(), [](const ArchiveSource &a, const ArchiveSource &b) {
        return byteOrderLess(sortKeyUtf8(a.relPath), sortKeyUtf8(b.relPath));
    });
    std::vector<QString> seen;
    for (const auto &entry : entries)
        requireSafeName(entry.relPath, seen);

    QDir().mkpath(QFileInfo(destination).absolutePath());
    QFile output(destination);
    if (!output.open(QIODevice::WriteOnly))
        throw ArchiveError("archive non écrivable : " + destination.toStdString());

    std::vector<ContentDigest> digests;
    qint64 logicalSize = 0;
    qint64 tarBytes = 0;
    QString archiveDigest;
    qint64 archiveBytes = 0;
    {
        // Le compresseur vit dans ce bloc : sa destruction libère le contexte
        // zstd même si une exception traverse, avant la fermeture du fichier.
        StreamCompressor compressor(output);
        QByteArray buffer(StreamChunk, '\0');

        for (const auto &entry : entries) {
            const auto utf8Name = entry.relPath.toUtf8();
            const bool pax = needsPaxHeader(entry.relPath);
            if (pax) {
                const auto records = paxRecord("path=" + utf8Name + "\n");
                const auto head =
                    header("././@PaxHeader", static_cast<quint64>(records.size()), 'x', 0);
                compressor.write(head.constData(), head.size());
                tarBytes += head.size();
                compressor.write(records.constData(), records.size());
                tarBytes += records.size();
                const auto pad = (TarBlockSize - records.size() % TarBlockSize) % TarBlockSize;
                if (pad > 0) {
                    const QByteArray padding(pad, '\0');
                    compressor.write(padding.constData(), padding.size());
                    tarBytes += pad;
                }
            }
            const auto storedName = pax ? asciiFallback(entry.relPath) : utf8Name;
            const auto head =
                header(storedName, static_cast<quint64>(entry.sizeBytes), '0', 0644);
            compressor.write(head.constData(), head.size());
            tarBytes += head.size();

            // La source n'est ouverte qu'ici, et une seule fois. Son contenu
            // est haché AU PASSAGE : on ne le relira pas pour l'identifier.
            auto source = entry.open ? entry.open() : nullptr;
            if (!source || !source->isReadable())
                throw ArchiveError("source illisible : " + entry.relPath.toStdString());
            QCryptographicHash fileHash(QCryptographicHash::Sha256);
            qint64 read = 0;
            for (;;) {
                const auto got = source->read(buffer.data(), buffer.size());
                if (got < 0)
                    throw ArchiveError("lecture interrompue : " + entry.relPath.toStdString());
                if (got == 0)
                    break;
                // Un fichier qui grandit pendant la capture rendrait l'archive
                // incohérente avec l'en-tête déjà écrit : on s'arrête net.
                if (read + got > entry.sizeBytes)
                    throw ArchiveError("la source a changé pendant l'archivage : " +
                                       entry.relPath.toStdString());
                compressor.write(buffer.constData(), got);
                fileHash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(got)));
                read += got;
            }
            if (read != entry.sizeBytes)
                throw ArchiveError("la source a changé pendant l'archivage : " +
                                   entry.relPath.toStdString());
            tarBytes += read;
            const auto pad = (TarBlockSize - read % TarBlockSize) % TarBlockSize;
            if (pad > 0) {
                const QByteArray padding(pad, '\0');
                compressor.write(padding.constData(), padding.size());
                tarBytes += pad;
            }
            digests.push_back({entry.relPath, entry.sizeBytes,
                               QString::fromLatin1(fileHash.result().toHex())});
            logicalSize += entry.sizeBytes;
        }

        const QByteArray endOfArchive(2 * TarBlockSize, '\0');
        compressor.write(endOfArchive.constData(), endOfArchive.size());
        tarBytes += endOfArchive.size();
        const auto tail = (TarRecordSize - tarBytes % TarRecordSize) % TarRecordSize;
        if (tail > 0) {
            const QByteArray padding(tail, '\0');
            compressor.write(padding.constData(), padding.size());
        }
        compressor.finish();
        archiveDigest = compressor.digest();
        archiveBytes = compressor.total();
    }
    output.close();

    const auto content = unitType == "file" ? digests.front().sha256Hex
                                            : directoryContentSha256(digests);
    return {content, archiveDigest, logicalSize, archiveBytes};
}

std::vector<ContentDigest> extractArchiveTo(const QString &archivePath, const QString &unitType,
                                            const QString &expectedContentSha256,
                                            const QString &stagingDirectory,
                                            const std::function<void(const StagedEntry &)> &sink)
{
    if (unitType != "file" && unitType != "dir")
        throw ArchiveError("type d'unité inconnu : " + unitType.toStdString());

    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly))
        throw ArchiveError("archive illisible : " + archivePath.toStdString());

    const auto scratch = QDir(stagingDirectory)
                             .filePath(".rsc-stage-" +
                                       QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(scratch))
        throw ArchiveError("dossier d'attente impossible à créer");

    // Effacement garanti au retour, y compris si une exception traverse : sans
    // cela une archive refusée laisserait ses octets sur le disque.
    struct ScratchGuard {
        QString path;
        ~ScratchGuard() { QDir(path).removeRecursively(); }
    } guard{scratch};

    std::vector<ContentDigest> digests;
    std::vector<QString> seen;
    QString pendingPath;
    QByteArray block(TarBlockSize, '\0');
    QByteArray buffer(StreamChunk, '\0');
    StreamDecompressor decompressor(input);

    for (;;) {
        if (!decompressor.readExactly(block.data(), TarBlockSize))
            break;
        if (block == QByteArray(TarBlockSize, '\0'))
            break;
        if (block.mid(OffChksum, 8) != checksumField(block))
            throw ArchiveError("somme de contrôle d'en-tête tar invalide");

        const auto size = static_cast<qint64>(parseOctal(block.mid(OffSize, 12)));
        const auto padding = (TarBlockSize - size % TarBlockSize) % TarBlockSize;
        const char typeflag = block.at(OffTypeflag);

        if (typeflag == 'x' || typeflag == 'X') {
            // Les enregistrements PAX sont minuscules par construction : les
            // tenir en mémoire ne contredit pas la règle du tampon constant.
            if (size < 0 || size > 64 * 1024)
                throw ArchiveError("en-tête PAX de taille déraisonnable");
            QByteArray records(static_cast<qsizetype>(size), '\0');
            if (!decompressor.readExactly(records.data(), size))
                throw ArchiveError("en-tête PAX tronqué");
            QByteArray discard(static_cast<qsizetype>(padding), '\0');
            if (padding > 0 && !decompressor.readExactly(discard.data(), padding))
                throw ArchiveError("en-tête PAX tronqué");
            qsizetype cursor = 0;
            while (cursor < records.size()) {
                const auto space = records.indexOf(' ', cursor);
                if (space < 0)
                    throw ArchiveError("enregistrement PAX illisible");
                const auto length = records.mid(cursor, space - cursor).toLongLong();
                if (length <= 0 || cursor + length > records.size())
                    throw ArchiveError("longueur d'enregistrement PAX invalide");
                const auto record = records.mid(space + 1, cursor + length - space - 2);
                if (record.startsWith("path="))
                    pendingPath = QString::fromUtf8(record.mid(5));
                cursor += length;
            }
            continue;
        }
        if (typeflag != '0' && typeflag != '\0')
            throw ArchiveError("type d'entrée tar non pris en charge");

        auto name = pendingPath;
        pendingPath.clear();
        if (name.isEmpty()) {
            const auto rawName = block.mid(OffName, LenName);
            const auto end = rawName.indexOf('\0');
            name = QString::fromUtf8(rawName.left(end < 0 ? rawName.size() : end));
        }
        requireSafeName(name, seen);

        QFile staged(QDir(scratch).filePath(stagedName(name)));
        if (!staged.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            throw ArchiveError("entrée déjà présente dans le dossier d'attente");
        QCryptographicHash fileHash(QCryptographicHash::Sha256);
        qint64 written = 0;
        while (written < size) {
            const auto want = std::min<qint64>(buffer.size(), size - written);
            if (!decompressor.readExactly(buffer.data(), want))
                throw ArchiveError("entrée d'archive tronquée : " + name.toStdString());
            if (staged.write(buffer.constData(), want) != want)
                throw ArchiveError("écriture du dossier d'attente incomplète");
            fileHash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(want)));
            written += want;
        }
        staged.close();
        if (padding > 0) {
            QByteArray discard(static_cast<qsizetype>(padding), '\0');
            if (!decompressor.readExactly(discard.data(), padding))
                throw ArchiveError("remplissage d'entrée tronqué");
        }
        digests.push_back({name, size, QString::fromLatin1(fileHash.result().toHex())});
    }

    if (unitType == "file" && digests.size() != 1)
        throw ArchiveError("une unité-fichier contient exactement un fichier");
    const auto actual = unitType == "file" ? digests.front().sha256Hex
                                           : directoryContentSha256(digests);
    // Le verdict tombe ICI : tout est encore dans le dossier d'attente, et
    // aucun fichier de l'utilisateur n'a été approché.
    if (actual != expectedContentSha256)
        throw ArchiveError("l'identité de contenu de l'archive ne correspond pas");

    for (const auto &digest : digests)
        sink({digest.relPath, QDir(scratch).filePath(stagedName(digest.relPath)),
              digest.sha256Hex});
    return digests;
}

} // namespace retrosave::core

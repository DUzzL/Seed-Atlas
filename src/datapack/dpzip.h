#ifndef DPZIP_H
#define DPZIP_H

// A minimal reader for zip archives (and jar files, which are zip archives):
// central directory, stored and deflated entries, zip64 sizes and offsets.
// Written for this project; it reads data packs and the game's jar, it is not
// a general purpose zip library (no encryption, no multi-disk archives, no
// other compression methods).
//
// Internal to src/datapack.

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdint>
#include <vector>

namespace dp
{

// Inflates a raw deflate stream (RFC 1951) of @srcLen bytes into exactly
// @dstLen bytes. Returns false if the stream is damaged or does not have
// that length.
bool inflateRaw(const unsigned char *src, size_t srcLen, unsigned char *dst, size_t dstLen);

uint32_t crc32(const unsigned char *data, size_t len);

class ZipReader
{
public:
    struct Entry
    {
        QString name;       // path inside the archive, '/' separated
        uint16_t method = 0;
        uint16_t flags = 0;
        uint32_t crc = 0;
        uint64_t compressedSize = 0;
        uint64_t size = 0;
        uint64_t headerOffset = 0;
    };

    ZipReader() = default;

    // Opens the archive and reads its directory. Archives whose entries
    // overlap are refused (here or by read()). The file stays open; entries
    // are read from it with explicit reads (no memory mapping).
    bool open(const QString& path, QString *error);
    // The same for an archive that is held in memory (an archive inside
    // another one).
    bool openData(const QByteArray& data, QString *error);

    const std::vector<Entry>& entries() const { return list; }

    // Reads and checks (CRC-32) the entry @index. Entries that unpack to
    // more than @maxSize bytes (at most MAX_ENTRY) are refused.
    bool read(size_t index, QByteArray *data, QString *error, uint64_t maxSize = MAX_ENTRY);

    // The largest entry read() unpacks
    static constexpr uint64_t MAX_ENTRY = 1ull << 30;

private:
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    QFile file;
    uint64_t length = 0;
    QByteArray loaded;                      // openData()
    bool inMemory = false;
    std::vector<Entry> list;
    std::vector<uint64_t> limit;            // per entry: where the next entry (or the directory) starts

    bool readDirectory(QString *error);
    // Reads @len bytes at @offset (false if they are not all there).
    bool readAt(uint64_t offset, uint64_t len, unsigned char *dst);
};

} // namespace dp

#endif // DPZIP_H

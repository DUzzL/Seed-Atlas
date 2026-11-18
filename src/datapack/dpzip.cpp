#include "dpzip.h"

#include <algorithm>
#include <cstring>

namespace dp
{

//==============================================================================
// inflate (RFC 1951)
//==============================================================================

namespace
{

enum { FAST_BITS = 9, MAX_BITS = 15 };

struct Huffman
{
    uint16_t count[MAX_BITS + 1];   // number of codes of each length
    uint16_t symbol[288];           // symbols ordered by code
    uint16_t fast[1 << FAST_BITS];  // (symbol << 4) | length for short codes, 0 = long code
};

// Builds the canonical code for @n code lengths. Returns false if the
// lengths are over-subscribed (more codes than the lengths allow).
bool buildHuffman(Huffman *h, const uint8_t *lengths, int n)
{
    memset(h->count, 0, sizeof(h->count));
    memset(h->fast, 0, sizeof(h->fast));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;

    int left = 1;
    for (int len = 1; len <= MAX_BITS; len++)
    {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return false;
    }

    uint16_t offs[MAX_BITS + 2];
    uint16_t code[MAX_BITS + 2];
    offs[1] = 0;
    code[1] = 0;
    for (int len = 1; len <= MAX_BITS; len++)
    {
        offs[len + 1] = offs[len] + h->count[len];
        code[len + 1] = (uint16_t) ((code[len] + h->count[len]) << 1);
    }
    for (int i = 0; i < n; i++)
    {
        int len = lengths[i];
        if (len == 0)
            continue;
        h->symbol[offs[len]++] = (uint16_t) i;
        uint16_t c = code[len]++;
        if (len <= FAST_BITS)
        {
            // codes are sent most significant bit first, the stream is read
            // least significant bit first: reverse the code
            unsigned r = 0;
            for (int b = 0; b < len; b++)
                r |= ((c >> b) & 1u) << (len - 1 - b);
            for (unsigned f = r; f < (1u << FAST_BITS); f += 1u << len)
                h->fast[f] = (uint16_t) ((i << 4) | len);
        }
    }
    return true;
}

struct BitReader
{
    const uint8_t *in;
    const uint8_t *end;
    uint64_t buf = 0;
    int cnt = 0;        // bits in buf
    int padded = 0;     // of them zero bits beyond the end of the input

    inline void fill()
    {
        while (cnt <= 56)
        {
            if (in < end)
                buf |= (uint64_t) *in++ << cnt;
            else
                padded += 8;
            cnt += 8;
        }
    }
    inline unsigned bits(int n)
    {
        if (cnt < n)
            fill();
        unsigned v = (unsigned) (buf & ((1ull << n) - 1));
        buf >>= n;
        cnt -= n;
        return v;
    }
    // true once bits beyond the end of the input were consumed
    inline bool overrun() const { return cnt < padded; }
};

inline int decodeSymbol(BitReader& br, const Huffman& h)
{
    if (br.cnt < MAX_BITS)
        br.fill();
    unsigned e = h.fast[br.buf & ((1u << FAST_BITS) - 1)];
    if (e)
    {
        int len = e & 15;
        br.buf >>= len;
        br.cnt -= len;
        return (int) (e >> 4);
    }
    // long code: walk the canonical code bit by bit
    int code = 0, first = 0, index = 0;
    uint64_t b = br.buf;
    for (int len = 1; len <= MAX_BITS; len++)
    {
        code |= (int) (b & 1);
        b >>= 1;
        int count = h.count[len];
        if (code - count < first)
        {
            br.buf >>= len;
            br.cnt -= len;
            return h.symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t g_lenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
const uint8_t g_lenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const uint16_t g_distBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
const uint8_t g_distExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

bool inflateBlock(BitReader& br, const Huffman& lit, const Huffman& dist,
    unsigned char *dst, size_t dstLen, size_t& pos)
{
    for (;;)
    {
        int sym = decodeSymbol(br, lit);
        if (sym < 0)
            return false;
        if (sym < 256)
        {
            if (pos >= dstLen)
                return false;
            dst[pos++] = (unsigned char) sym;
            continue;
        }
        if (sym == 256)
            return !br.overrun();
        sym -= 257;
        if (sym >= 29)
            return false;
        size_t len = g_lenBase[sym] + br.bits(g_lenExtra[sym]);
        int ds = decodeSymbol(br, dist);
        if (ds < 0 || ds >= 30)
            return false;
        size_t d = g_distBase[ds] + br.bits(g_distExtra[ds]);
        if (d > pos || len > dstLen - pos)
            return false;
        const unsigned char *from = dst + pos - d;
        for (size_t i = 0; i < len; i++)
            dst[pos + i] = from[i];     // may overlap: byte by byte
        pos += len;
        if (br.overrun())
            return false;
    }
}

} // namespace

bool inflateRaw(const unsigned char *src, size_t srcLen, unsigned char *dst, size_t dstLen)
{
    BitReader br;
    br.in = src;
    br.end = src + srcLen;
    size_t pos = 0;

    Huffman lit, dist;
    bool final;
    do
    {
        final = br.bits(1) != 0;
        unsigned type = br.bits(2);
        if (type == 0)
        {
            // stored: skip to the byte boundary
            br.bits(br.cnt & 7);
            unsigned len = br.bits(16);
            unsigned nlen = br.bits(16);
            if (br.overrun() || len != (~nlen & 0xffffu) || len > dstLen - pos)
                return false;
            for (unsigned i = 0; i < len; i++)
                dst[pos++] = (unsigned char) br.bits(8);
            if (br.overrun())
                return false;
        }
        else if (type == 1 || type == 2)
        {
            uint8_t lengths[320];
            if (type == 1)
            {
                int i = 0;
                for (; i < 144; i++) lengths[i] = 8;
                for (; i < 256; i++) lengths[i] = 9;
                for (; i < 280; i++) lengths[i] = 7;
                for (; i < 288; i++) lengths[i] = 8;
                buildHuffman(&lit, lengths, 288);
                for (i = 0; i < 30; i++) lengths[i] = 5;
                buildHuffman(&dist, lengths, 30);
            }
            else
            {
                static const uint8_t order[19] = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
                };
                int nlen = (int) br.bits(5) + 257;
                int ndist = (int) br.bits(5) + 1;
                int ncode = (int) br.bits(4) + 4;
                if (nlen > 286 || ndist > 30)
                    return false;
                memset(lengths, 0, 19);
                for (int i = 0; i < ncode; i++)
                    lengths[order[i]] = (uint8_t) br.bits(3);
                Huffman lencode;
                if (!buildHuffman(&lencode, lengths, 19))
                    return false;
                int i = 0;
                while (i < nlen + ndist)
                {
                    int sym = decodeSymbol(br, lencode);
                    if (sym < 0 || br.overrun())
                        return false;
                    if (sym < 16)
                    {
                        lengths[i++] = (uint8_t) sym;
                        continue;
                    }
                    int prev = 0, rep;
                    if (sym == 16)
                    {
                        if (i == 0)
                            return false;
                        prev = lengths[i - 1];
                        rep = 3 + (int) br.bits(2);
                    }
                    else if (sym == 17)
                        rep = 3 + (int) br.bits(3);
                    else
                        rep = 11 + (int) br.bits(7);
                    if (i + rep > nlen + ndist)
                        return false;
                    while (rep--)
                        lengths[i++] = (uint8_t) prev;
                }
                if (lengths[256] == 0)
                    return false;   // no end-of-block code
                if (!buildHuffman(&lit, lengths, nlen))
                    return false;
                if (!buildHuffman(&dist, lengths + nlen, ndist))
                    return false;
            }
            if (!inflateBlock(br, lit, dist, dst, dstLen, pos))
                return false;
        }
        else
        {
            return false;
        }
    }
    while (!final);

    return pos == dstLen && !br.overrun();
}

namespace
{

struct CrcTable
{
    uint32_t v[256];
    CrcTable()
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            v[i] = c;
        }
    }
};

} // namespace

uint32_t crc32(const unsigned char *data, size_t len)
{
    // a function-local static is initialised once, thread safe
    static const CrcTable table;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < len; i++)
        c = table.v[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

//==============================================================================
// zip
//==============================================================================

namespace
{

inline uint16_t rd16(const unsigned char *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
inline uint32_t rd32(const unsigned char *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
inline uint64_t rd64(const unsigned char *p)
{
    return (uint64_t) rd32(p) | ((uint64_t) rd32(p + 4) << 32);
}

// The largest central directory that is read (a directory entry has at least
// 46 bytes: about 5 million entries)
const uint64_t MAX_DIRECTORY = 256ull << 20;

} // namespace

bool ZipReader::open(const QString& path, QString *error)
{
    list.clear();
    loaded.clear();
    inMemory = false;
    length = 0;
    file.close();
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
    {
        *error = QStringLiteral("The file cannot be opened.");
        return false;
    }
    // The file is read with explicit reads, not memory mapped: a file that is
    // cut off or rewritten while it is read gives short reads (an error), a
    // mapping would raise SIGBUS.
    length = (uint64_t) std::max<qint64>(0, file.size());
    return readDirectory(error);
}

bool ZipReader::openData(const QByteArray& data, QString *error)
{
    list.clear();
    file.close();
    loaded = data;
    inMemory = true;
    length = (uint64_t) loaded.size();
    return readDirectory(error);
}

bool ZipReader::readAt(uint64_t offset, uint64_t len, unsigned char *dst)
{
    if (offset > length || len > length - offset)
        return false;
    if (len == 0)
        return true;
    if (inMemory)
    {
        memcpy(dst, loaded.constData() + offset, (size_t) len);
        return true;
    }
    if (!file.seek((qint64) offset))
        return false;
    uint64_t done = 0;
    while (done < len)
    {
        qint64 n = file.read((char*) dst + done, (qint64) std::min<uint64_t>(len - done, 1u << 30));
        if (n <= 0)
            return false;   // the file got shorter
        done += (uint64_t) n;
    }
    return true;
}

bool ZipReader::readDirectory(QString *error)
{
    // end of central directory record: at the end, before an optional comment
    const uint64_t eocdSize = 22;
    if (length < eocdSize)
    {
        *error = QStringLiteral("The file is not a zip archive (too short).");
        return false;
    }
    // the end of the file: the record, its comment (up to 65535 bytes) and
    // the zip64 locator in front of the record
    const uint64_t tailLen = std::min<uint64_t>(length, eocdSize + 0xffff + 20);
    const uint64_t tailStart = length - tailLen;
    std::vector<unsigned char> tail((size_t) tailLen);
    if (!readAt(tailStart, tailLen, tail.data()))
    {
        *error = QStringLiteral("The file cannot be read.");
        return false;
    }
    const unsigned char *t = tail.data();
    uint64_t eocd = tailLen - eocdSize;     // relative to tailStart
    const uint64_t lowest = tailLen > eocdSize + 0xffff ? tailLen - eocdSize - 0xffff : 0;
    bool found = false;
    for (;; eocd--)
    {
        if (rd32(t + eocd) == 0x06054b50u)
        {
            found = true;
            break;
        }
        if (eocd == lowest)
            break;
    }
    if (!found)
    {
        *error = QStringLiteral("The file is not a zip archive or it is cut off (no directory at its end).");
        return false;
    }

    uint64_t count = rd16(t + eocd + 10);
    uint64_t cdSize = rd32(t + eocd + 12);
    uint64_t cdOffset = rd32(t + eocd + 16);
    if (rd16(t + eocd + 4) != 0 || rd16(t + eocd + 6) != 0)
    {
        if (rd16(t + eocd + 4) != 0xffff)
        {
            *error = QStringLiteral("Zip archives split into several files are not supported.");
            return false;
        }
    }
    // zip64: a locator in front of the record points to the 64 bit record
    if (eocd >= 20 && rd32(t + eocd - 20) == 0x07064b50u)
    {
        const uint64_t z = rd64(t + eocd - 20 + 8);
        unsigned char rec[56];
        // z comes from the file: compare without forming z + 56 (it may wrap)
        if (z <= length && length - z >= 56 && readAt(z, 56, rec) && rd32(rec) == 0x06064b50u)
        {
            count = rd64(rec + 32);
            cdSize = rd64(rec + 40);
            cdOffset = rd64(rec + 48);
        }
    }
    if (cdOffset > length || cdSize > length - cdOffset)
    {
        *error = QStringLiteral("The zip archive is damaged (its directory lies outside the file).");
        return false;
    }
    if (cdSize > MAX_DIRECTORY)
    {
        *error = QStringLiteral("The zip archive's directory is too large.");
        return false;
    }
    std::vector<unsigned char> dir((size_t) cdSize + 1);
    if (!readAt(cdOffset, cdSize, dir.data()))
    {
        *error = QStringLiteral("The file cannot be read.");
        return false;
    }

    const unsigned char *p = dir.data();
    const unsigned char *end = p + cdSize;
    std::vector<uint64_t> nameLens;     // as stored (the decoded name may differ in length)
    for (uint64_t i = 0; i < count; i++)
    {
        if (end - p < 46 || rd32(p) != 0x02014b50u)
        {
            *error = QStringLiteral("The zip archive is damaged (broken directory entry).");
            return false;
        }
        Entry e;
        e.flags = rd16(p + 8);
        e.method = rd16(p + 10);
        e.crc = rd32(p + 16);
        e.compressedSize = rd32(p + 20);
        e.size = rd32(p + 24);
        size_t nameLen = rd16(p + 28);
        size_t extraLen = rd16(p + 30);
        size_t commentLen = rd16(p + 32);
        e.headerOffset = rd32(p + 42);
        if ((size_t) (end - p) < 46 + nameLen + extraLen + commentLen)
        {
            *error = QStringLiteral("The zip archive is damaged (broken directory entry).");
            return false;
        }
        e.name = QString::fromUtf8((const char*) p + 46, (qsizetype) nameLen);
        e.name.replace(QLatin1Char('\\'), QLatin1Char('/'));

        // zip64 extra field: the values that did not fit, in this order
        const unsigned char *x = p + 46 + nameLen;
        const unsigned char *xend = x + extraLen;
        while (xend - x >= 4)
        {
            uint16_t id = rd16(x);
            size_t sz = rd16(x + 2);
            x += 4;
            if ((size_t) (xend - x) < sz)
                break;
            if (id == 0x0001)
            {
                const unsigned char *q = x;
                const unsigned char *qend = x + sz;
                if (e.size == 0xffffffffu && qend - q >= 8) { e.size = rd64(q); q += 8; }
                if (e.compressedSize == 0xffffffffu && qend - q >= 8) { e.compressedSize = rd64(q); q += 8; }
                if (e.headerOffset == 0xffffffffu && qend - q >= 8) { e.headerOffset = rd64(q); q += 8; }
            }
            x += sz;
        }
        list.push_back(e);
        nameLens.push_back(nameLen);
        p += 46 + nameLen + extraLen + commentLen;
    }

    // The bytes of the entries (local header, name, extra field and data) must
    // not overlap: several directory entries that share one compressed stream
    // would multiply the data an archive of a given size hands out. Here,
    // from the directory alone: no two entries start at the same offset, and
    // none reaches the next one with its header, name and data (the local
    // extra field only makes it longer). read() checks the exact end of an
    // entry's data against the start of the next one (limit).
    std::vector<size_t> order(list.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        return list[a].headerOffset < list[b].headerOffset;
    });
    limit.assign(list.size(), length);
    for (size_t k = 0; k < order.size(); k++)
    {
        const Entry& e = list[order[k]];
        uint64_t next = cdOffset > e.headerOffset ? cdOffset : length;
        if (k + 1 < order.size())
        {
            const uint64_t h = list[order[k + 1]].headerOffset;
            if (h == e.headerOffset)
            {
                *error = QStringLiteral("The zip archive is damaged or crafted (its entries overlap).");
                list.clear();
                return false;
            }
            next = std::min(next, h);
        }
        limit[order[k]] = next;
        // the entry's shortest possible extent (header, the name as in the
        // directory, data) must end before the next entry; a broken entry
        // (outside the file) is left to read()
        const uint64_t extent = e.headerOffset + 30 + nameLens[order[k]] + e.compressedSize;
        if (k + 1 < order.size() && e.headerOffset <= length && e.compressedSize <= length && extent <= length &&
            extent > list[order[k + 1]].headerOffset)
        {
            *error = QStringLiteral("The zip archive is damaged or crafted (its entries overlap).");
            list.clear();
            return false;
        }
    }
    return true;
}

bool ZipReader::read(size_t index, QByteArray *data, QString *error, uint64_t maxSize)
{
    data->clear();
    if (index >= list.size())
    {
        *error = QStringLiteral("No such entry.");
        return false;
    }
    const Entry& e = list[index];
    if (e.flags & 1)
    {
        *error = QStringLiteral("The entry is encrypted.");
        return false;
    }
    unsigned char h[30];
    if (e.headerOffset > length || length - e.headerOffset < 30 || !readAt(e.headerOffset, 30, h) ||
        rd32(h) != 0x04034b50u)
    {
        *error = QStringLiteral("The zip archive is damaged or cut off (the entry's data is missing).");
        return false;
    }
    uint64_t off = e.headerOffset + 30 + rd16(h + 26) + rd16(h + 28);
    if (off > length || e.compressedSize > length - off)
    {
        *error = QStringLiteral("The zip archive is damaged or cut off (the entry's data is missing).");
        return false;
    }
    if (off + e.compressedSize > limit[index])
    {
        *error = QStringLiteral("The zip archive is damaged or crafted (its entries overlap).");
        return false;
    }
    if (e.size > maxSize || e.size > MAX_ENTRY)
    {
        *error = QStringLiteral("The entry is too large (%1 MiB unpacked, at most %2 MiB).")
            .arg((qulonglong) (e.size >> 20)).arg((qulonglong) (std::min(maxSize, MAX_ENTRY) >> 20));
        return false;
    }
    if (e.method == 0)
    {
        if (e.compressedSize != e.size)
        {
            *error = QStringLiteral("The zip archive is damaged (wrong size of a stored entry).");
            return false;
        }
        data->resize((qsizetype) e.size);
        if (!readAt(off, e.size, (unsigned char*) data->data()))
        {
            *error = QStringLiteral("The zip archive is damaged or cut off (the entry's data is missing).");
            data->clear();
            return false;
        }
    }
    else if (e.method == 8)
    {
        // deflate makes data at most a little larger (stored blocks: 5 bytes
        // per 65535); a much larger stream is damaged (and is not read)
        if (e.compressedSize > e.size + (e.size >> 1) + 65536)
        {
            *error = QStringLiteral("The zip archive is damaged (the entry cannot be decompressed).");
            return false;
        }
        std::vector<unsigned char> packed((size_t) e.compressedSize + 1);
        if (!readAt(off, e.compressedSize, packed.data()))
        {
            *error = QStringLiteral("The zip archive is damaged or cut off (the entry's data is missing).");
            return false;
        }
        data->resize((qsizetype) e.size);
        if (!inflateRaw(packed.data(), (size_t) e.compressedSize, (unsigned char*) data->data(), (size_t) e.size))
        {
            *error = QStringLiteral("The zip archive is damaged (the entry cannot be decompressed).");
            data->clear();
            return false;
        }
    }
    else
    {
        *error = QStringLiteral("The entry uses compression method %1, which is not supported.").arg(e.method);
        return false;
    }
    if (crc32((const unsigned char*) data->constData(), (size_t) e.size) != e.crc)
    {
        *error = QStringLiteral("The zip archive is damaged (checksum mismatch).");
        data->clear();
        return false;
    }
    return true;
}

} // namespace dp

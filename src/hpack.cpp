#include "qtng/private/hpack_p.h"

#include <algorithm>
#include <cstring>

using namespace std;

namespace qtng {

namespace {

struct StaticEntry {
    const char *name;
    const char *value;
};

static const StaticEntry kStaticTable[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};
static const size_t kStaticTableSize = sizeof(kStaticTable) / sizeof(kStaticTable[0]);

struct HuffmanCode { uint32_t code; uint8_t bits; };
static const HuffmanCode kHuffmanTable[257] = {
    {0x1ff8u, 13},
    {0x7fffd8u, 23},
    {0xfffffe2u, 28},
    {0xfffffe3u, 28},
    {0xfffffe4u, 28},
    {0xfffffe5u, 28},
    {0xfffffe6u, 28},
    {0xfffffe7u, 28},
    {0xfffffe8u, 28},
    {0xffffeau, 24},
    {0x3ffffffcu, 30},
    {0xfffffe9u, 28},
    {0xfffffeau, 28},
    {0x3ffffffdu, 30},
    {0xfffffebu, 28},
    {0xfffffecu, 28},
    {0xfffffedu, 28},
    {0xfffffeeu, 28},
    {0xfffffefu, 28},
    {0xffffff0u, 28},
    {0xffffff1u, 28},
    {0xffffff2u, 28},
    {0x3ffffffeu, 30},
    {0xffffff3u, 28},
    {0xffffff4u, 28},
    {0xffffff5u, 28},
    {0xffffff6u, 28},
    {0xffffff7u, 28},
    {0xffffff8u, 28},
    {0xffffff9u, 28},
    {0xffffffau, 28},
    {0xffffffbu, 28},
    {0x14u, 6},
    {0x3f8u, 10},
    {0x3f9u, 10},
    {0xffau, 12},
    {0x1ff9u, 13},
    {0x15u, 6},
    {0xf8u, 8},
    {0x7fau, 11},
    {0x3fau, 10},
    {0x3fbu, 10},
    {0xf9u, 8},
    {0x7fbu, 11},
    {0xfau, 8},
    {0x16u, 6},
    {0x17u, 6},
    {0x18u, 6},
    {0x0u, 5},
    {0x1u, 5},
    {0x2u, 5},
    {0x19u, 6},
    {0x1au, 6},
    {0x1bu, 6},
    {0x1cu, 6},
    {0x1du, 6},
    {0x1eu, 6},
    {0x1fu, 6},
    {0x5cu, 7},
    {0xfbu, 8},
    {0x7ffcu, 15},
    {0x20u, 6},
    {0xffbu, 12},
    {0x3fcu, 10},
    {0x1ffau, 13},
    {0x21u, 6},
    {0x5du, 7},
    {0x5eu, 7},
    {0x5fu, 7},
    {0x60u, 7},
    {0x61u, 7},
    {0x62u, 7},
    {0x63u, 7},
    {0x64u, 7},
    {0x65u, 7},
    {0x66u, 7},
    {0x67u, 7},
    {0x68u, 7},
    {0x69u, 7},
    {0x6au, 7},
    {0x6bu, 7},
    {0x6cu, 7},
    {0x6du, 7},
    {0x6eu, 7},
    {0x6fu, 7},
    {0x70u, 7},
    {0x71u, 7},
    {0x72u, 7},
    {0xfcu, 8},
    {0x73u, 7},
    {0xfdu, 8},
    {0x1ffbu, 13},
    {0x7fff0u, 19},
    {0x1ffcu, 13},
    {0x3ffcu, 14},
    {0x22u, 6},
    {0x7ffdu, 15},
    {0x3u, 5},
    {0x23u, 6},
    {0x4u, 5},
    {0x24u, 6},
    {0x5u, 5},
    {0x25u, 6},
    {0x26u, 6},
    {0x27u, 6},
    {0x6u, 5},
    {0x74u, 7},
    {0x75u, 7},
    {0x28u, 6},
    {0x29u, 6},
    {0x2au, 6},
    {0x7u, 5},
    {0x2bu, 6},
    {0x76u, 7},
    {0x2cu, 6},
    {0x8u, 5},
    {0x9u, 5},
    {0x2du, 6},
    {0x77u, 7},
    {0x78u, 7},
    {0x79u, 7},
    {0x7au, 7},
    {0x7bu, 7},
    {0x7ffeu, 15},
    {0x7fcu, 11},
    {0x3ffdu, 14},
    {0x1ffdu, 13},
    {0xffffffcu, 28},
    {0xfffe6u, 20},
    {0x3fffd2u, 22},
    {0xfffe7u, 20},
    {0xfffe8u, 20},
    {0x3fffd3u, 22},
    {0x3fffd4u, 22},
    {0x3fffd5u, 22},
    {0x7fffd9u, 23},
    {0x3fffd6u, 22},
    {0x7fffdau, 23},
    {0x7fffdbu, 23},
    {0x7fffdcu, 23},
    {0x7fffddu, 23},
    {0x7fffdeu, 23},
    {0xffffebu, 24},
    {0x7fffdfu, 23},
    {0xffffecu, 24},
    {0xffffedu, 24},
    {0x3fffd7u, 22},
    {0x7fffe0u, 23},
    {0xffffeeu, 24},
    {0x7fffe1u, 23},
    {0x7fffe2u, 23},
    {0x7fffe3u, 23},
    {0x7fffe4u, 23},
    {0x1fffdcu, 21},
    {0x3fffd8u, 22},
    {0x7fffe5u, 23},
    {0x3fffd9u, 22},
    {0x7fffe6u, 23},
    {0x7fffe7u, 23},
    {0xffffefu, 24},
    {0x3fffdau, 22},
    {0x1fffddu, 21},
    {0xfffe9u, 20},
    {0x3fffdbu, 22},
    {0x3fffdcu, 22},
    {0x7fffe8u, 23},
    {0x7fffe9u, 23},
    {0x1fffdeu, 21},
    {0x7fffeau, 23},
    {0x3fffddu, 22},
    {0x3fffdeu, 22},
    {0xfffff0u, 24},
    {0x1fffdfu, 21},
    {0x3fffdfu, 22},
    {0x7fffebu, 23},
    {0x7fffecu, 23},
    {0x1fffe0u, 21},
    {0x1fffe1u, 21},
    {0x3fffe0u, 22},
    {0x1fffe2u, 21},
    {0x7fffedu, 23},
    {0x3fffe1u, 22},
    {0x7fffeeu, 23},
    {0x7fffefu, 23},
    {0xfffeau, 20},
    {0x3fffe2u, 22},
    {0x3fffe3u, 22},
    {0x3fffe4u, 22},
    {0x7ffff0u, 23},
    {0x3fffe5u, 22},
    {0x3fffe6u, 22},
    {0x7ffff1u, 23},
    {0x3ffffe0u, 26},
    {0x3ffffe1u, 26},
    {0xfffebu, 20},
    {0x7fff1u, 19},
    {0x3fffe7u, 22},
    {0x7ffff2u, 23},
    {0x3fffe8u, 22},
    {0x1ffffecu, 25},
    {0x3ffffe2u, 26},
    {0x3ffffe3u, 26},
    {0x3ffffe4u, 26},
    {0x7ffffdeu, 27},
    {0x7ffffdfu, 27},
    {0x3ffffe5u, 26},
    {0xfffff1u, 24},
    {0x1ffffedu, 25},
    {0x7fff2u, 19},
    {0x1fffe3u, 21},
    {0x3ffffe6u, 26},
    {0x7ffffe0u, 27},
    {0x7ffffe1u, 27},
    {0x3ffffe7u, 26},
    {0x7ffffe2u, 27},
    {0xfffff2u, 24},
    {0x1fffe4u, 21},
    {0x1fffe5u, 21},
    {0x3ffffe8u, 26},
    {0x3ffffe9u, 26},
    {0xffffffdu, 28},
    {0x7ffffe3u, 27},
    {0x7ffffe4u, 27},
    {0x7ffffe5u, 27},
    {0xfffecu, 20},
    {0xfffff3u, 24},
    {0xfffedu, 20},
    {0x1fffe6u, 21},
    {0x3fffe9u, 22},
    {0x1fffe7u, 21},
    {0x1fffe8u, 21},
    {0x7ffff3u, 23},
    {0x3fffeau, 22},
    {0x3fffebu, 22},
    {0x1ffffeeu, 25},
    {0x1ffffefu, 25},
    {0xfffff4u, 24},
    {0xfffff5u, 24},
    {0x3ffffeau, 26},
    {0x7ffff4u, 23},
    {0x3ffffebu, 26},
    {0x7ffffe6u, 27},
    {0x3ffffecu, 26},
    {0x3ffffedu, 26},
    {0x7ffffe7u, 27},
    {0x7ffffe8u, 27},
    {0x7ffffe9u, 27},
    {0x7ffffeau, 27},
    {0x7ffffebu, 27},
    {0xffffffeu, 28},
    {0x7ffffecu, 27},
    {0x7ffffedu, 27},
    {0x7ffffeeu, 27},
    {0x7ffffefu, 27},
    {0x7fffff0u, 27},
    {0x3ffffeeu, 26},
    {0x3fffffffu, 30}
};


static void encodeInteger(string *out, uint32_t value, uint8_t prefixBits, uint8_t firstByte)
{
    const uint32_t maxPrefix = (1u << prefixBits) - 1u;
    if (value < maxPrefix) {
        out->push_back(static_cast<char>(firstByte | value));
        return;
    }
    out->push_back(static_cast<char>(firstByte | maxPrefix));
    value -= maxPrefix;
    while (value >= 128) {
        out->push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<char>(value));
}

static bool decodeInteger(const string &data, size_t *pos, uint8_t prefixBits, uint32_t *value)
{
    if (*pos >= data.size()) {
        return false;
    }
    const uint32_t maxPrefix = (1u << prefixBits) - 1u;
    uint8_t b = static_cast<uint8_t>(data[*pos]);
    ++(*pos);
    uint32_t v = b & maxPrefix;
    if (v < maxPrefix) {
        *value = v;
        return true;
    }
    uint32_t m = 0;
    while (*pos < data.size()) {
        b = static_cast<uint8_t>(data[*pos]);
        ++(*pos);
        v += (b & 0x7f) << m;
        m += 7;
        if ((b & 0x80) == 0) {
            *value = v;
            return true;
        }
        if (m > 28) {
            return false;
        }
    }
    return false;
}

static string huffmanEncode(const string &input)
{
    string out;
    uint64_t buffer = 0;
    int bits = 0;
    for (unsigned char ch : input) {
        const HuffmanCode &hc = kHuffmanTable[ch];
        buffer = (buffer << hc.bits) | hc.code;
        bits += hc.bits;
        while (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    if (bits > 0) {
        buffer = (buffer << (8 - bits)) | (0xffu >> bits);
        out.push_back(static_cast<char>(buffer & 0xff));
    }
    return out;
}

struct HuffmanNode {
    int symbol; // >=0 leaf, -1 internal
    int left;
    int right;
};

static vector<HuffmanNode> buildHuffmanTree()
{
    vector<HuffmanNode> nodes;
    nodes.push_back(HuffmanNode{-1, -1, -1}); // root
    nodes.reserve(1024);
    for (int sym = 0; sym < 257; ++sym) {
        uint32_t code = kHuffmanTable[sym].code;
        int bitLen = kHuffmanTable[sym].bits;
        int idx = 0;
        for (int i = bitLen - 1; i >= 0; --i) {
            const int bit = (code >> i) & 1;
            int child = bit ? nodes[idx].right : nodes[idx].left;
            if (child < 0) {
                child = static_cast<int>(nodes.size());
                nodes.push_back(HuffmanNode{-1, -1, -1});
                if (bit) {
                    nodes[idx].right = child;
                } else {
                    nodes[idx].left = child;
                }
            }
            idx = child;
        }
        nodes[idx].symbol = sym;
    }
    return nodes;
}

static const vector<HuffmanNode> &huffmanTree()
{
    static const vector<HuffmanNode> tree = buildHuffmanTree();
    return tree;
}

static bool huffmanDecode(const string &input, string *out)
{
    const vector<HuffmanNode> &tree = huffmanTree();
    int idx = 0;
    out->clear();
    for (unsigned char byte : input) {
        for (int i = 7; i >= 0; --i) {
            int bit = (byte >> i) & 1;
            idx = bit ? tree[idx].right : tree[idx].left;
            if (idx < 0) {
                return false;
            }
            if (tree[idx].symbol >= 0) {
                if (tree[idx].symbol == 256) {
                    // EOS padding — accept and finish.
                    return true;
                }
                out->push_back(static_cast<char>(tree[idx].symbol));
                idx = 0;
            }
        }
    }
    // Accept if we ended at root (padding of 1-bits may leave us mid-path toward EOS).
    return idx == 0 || tree[idx].symbol == 256;
}

static bool decodeString(const string &data, size_t *pos, string *out)
{
    if (*pos >= data.size()) {
        return false;
    }
    uint8_t b = static_cast<uint8_t>(data[*pos]);
    bool huffman = (b & 0x80) != 0;
    uint32_t length = 0;
    if (!decodeInteger(data, pos, 7, &length)) {
        return false;
    }
    if (*pos + length > data.size()) {
        return false;
    }
    string raw = data.substr(*pos, length);
    *pos += length;
    if (huffman) {
        return huffmanDecode(raw, out);
    }
    *out = raw;
    return true;
}

static void encodeString(string *out, const string &value, bool useHuffman)
{
    if (useHuffman) {
        string encoded = huffmanEncode(value);
        encodeInteger(out, static_cast<uint32_t>(encoded.size()), 7, 0x80);
        *out += encoded;
    } else {
        encodeInteger(out, static_cast<uint32_t>(value.size()), 7, 0x00);
        *out += value;
    }
}

static uint32_t entrySize(const HpackHeader &h)
{
    return static_cast<uint32_t>(h.name.size() + h.value.size() + 32);
}

static int findStaticIndex(const HpackHeader &h, bool *nameOnly)
{
    *nameOnly = false;
    int nameIndex = -1;
    for (size_t i = 0; i < kStaticTableSize; ++i) {
        if (strcmp(kStaticTable[i].name, h.name.c_str()) == 0) {
            if (nameIndex < 0) {
                nameIndex = static_cast<int>(i + 1);
            }
            if (strcmp(kStaticTable[i].value, h.value.c_str()) == 0) {
                *nameOnly = false;
                return static_cast<int>(i + 1);
            }
        }
    }
    if (nameIndex > 0) {
        *nameOnly = true;
        return nameIndex;
    }
    return -1;
}

}  // namespace

HpackEncoder::HpackEncoder()
    : maxDynamicTableSize(4096)
    , dynamicTableSize(0)
{
}

void HpackEncoder::setMaxDynamicTableSize(uint32_t size)
{
    maxDynamicTableSize = size;
    evict();
}

void HpackEncoder::evict()
{
    while (dynamicTableSize > maxDynamicTableSize && !dynamicTable.empty()) {
        dynamicTableSize -= entrySize(dynamicTable.back());
        dynamicTable.pop_back();
    }
}

void HpackEncoder::addToDynamicTable(const HpackHeader &header)
{
    const uint32_t size = entrySize(header);
    if (size > maxDynamicTableSize) {
        dynamicTable.clear();
        dynamicTableSize = 0;
        return;
    }
    dynamicTable.insert(dynamicTable.begin(), header);
    dynamicTableSize += size;
    evict();
}

string HpackEncoder::encode(const vector<HpackHeader> &headers)
{
    string out;
    for (const HpackHeader &h : headers) {
        bool nameOnly = false;
        int staticIndex = findStaticIndex(h, &nameOnly);
        if (staticIndex > 0 && !nameOnly) {
            encodeInteger(&out, static_cast<uint32_t>(staticIndex), 7, 0x80);
            continue;
        }
        // Literal with incremental indexing — name as index or literal.
        if (staticIndex > 0 && nameOnly) {
            encodeInteger(&out, static_cast<uint32_t>(staticIndex), 6, 0x40);
        } else {
            out.push_back(0x40);
            encodeString(&out, h.name, false);
        }
        encodeString(&out, h.value, false);
        addToDynamicTable(h);
    }
    return out;
}

HpackDecoder::HpackDecoder()
    : maxDynamicTableSize(4096)
    , dynamicTableSize(0)
{
}

void HpackDecoder::setMaxDynamicTableSize(uint32_t size)
{
    maxDynamicTableSize = size;
    evict();
}

void HpackDecoder::evict()
{
    while (dynamicTableSize > maxDynamicTableSize && !dynamicTable.empty()) {
        dynamicTableSize -= entrySize(dynamicTable.back());
        dynamicTable.pop_back();
    }
}

void HpackDecoder::addToDynamicTable(const HpackHeader &header)
{
    const uint32_t size = entrySize(header);
    if (size > maxDynamicTableSize) {
        dynamicTable.clear();
        dynamicTableSize = 0;
        return;
    }
    dynamicTable.insert(dynamicTable.begin(), header);
    dynamicTableSize += size;
    evict();
}

bool HpackDecoder::lookup(uint32_t index, HpackHeader *out) const
{
    if (index == 0) {
        return false;
    }
    if (index <= kStaticTableSize) {
        out->name = kStaticTable[index - 1].name;
        out->value = kStaticTable[index - 1].value;
        return true;
    }
    const uint32_t dynIndex = index - static_cast<uint32_t>(kStaticTableSize) - 1;
    if (dynIndex >= dynamicTable.size()) {
        return false;
    }
    *out = dynamicTable[dynIndex];
    return true;
}

bool HpackDecoder::decode(const string &data, vector<HpackHeader> *headers)
{
    headers->clear();
    size_t pos = 0;
    while (pos < data.size()) {
        uint8_t b = static_cast<uint8_t>(data[pos]);
        if ((b & 0x80) != 0) {
            uint32_t index = 0;
            if (!decodeInteger(data, &pos, 7, &index)) {
                headers->clear();
                return false;
            }
            HpackHeader h;
            if (!lookup(index, &h)) {
                headers->clear();
                return false;
            }
            headers->push_back(h);
        } else if ((b & 0xc0) == 0x40) {
            uint32_t index = 0;
            HpackHeader h;
            if (!decodeInteger(data, &pos, 6, &index)) {
                return false;
            }
            if (index == 0) {
                if (!decodeString(data, &pos, &h.name)) {
                    return false;
                }
            } else if (!lookup(index, &h)) {
                return false;
            } else {
                h.value.clear();
            }
            if (!decodeString(data, &pos, &h.value)) {
                return false;
            }
            addToDynamicTable(h);
            headers->push_back(h);
        } else if ((b & 0xe0) == 0x20) {
            uint32_t size = 0;
            if (!decodeInteger(data, &pos, 5, &size)) {
                return false;
            }
            setMaxDynamicTableSize(size);
        } else {
            // Literal without indexing / never indexed
            uint8_t prefix = ((b & 0xf0) == 0x10) ? 4 : 4;
            uint32_t index = 0;
            HpackHeader h;
            if (!decodeInteger(data, &pos, prefix, &index)) {
                return false;
            }
            if (index == 0) {
                if (!decodeString(data, &pos, &h.name)) {
                    return false;
                }
            } else if (!lookup(index, &h)) {
                return false;
            } else {
                h.value.clear();
            }
            if (!decodeString(data, &pos, &h.value)) {
                return false;
            }
            headers->push_back(h);
        }
    }
    return true;
}

}  // namespace qtng

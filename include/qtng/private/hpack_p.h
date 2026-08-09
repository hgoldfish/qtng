#ifndef QTNG_HPACK_P_H
#define QTNG_HPACK_P_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace qtng {

struct HpackHeader
{
    std::string name;
    std::string value;
};

class HpackEncoder
{
public:
    HpackEncoder();
    std::string encode(const std::vector<HpackHeader> &headers);
    void setMaxDynamicTableSize(std::uint32_t size);
private:
    std::vector<HpackHeader> dynamicTable;
    std::uint32_t maxDynamicTableSize;
    std::uint32_t dynamicTableSize;
    void evict();
    void addToDynamicTable(const HpackHeader &header);
};

class HpackDecoder
{
public:
    HpackDecoder();
    bool decode(const std::string &data, std::vector<HpackHeader> *headers);
    void setMaxDynamicTableSize(std::uint32_t size);
private:
    std::vector<HpackHeader> dynamicTable;
    std::uint32_t maxDynamicTableSize;
    std::uint32_t dynamicTableSize;
    void evict();
    void addToDynamicTable(const HpackHeader &header);
    bool lookup(std::uint32_t index, HpackHeader *out) const;
};

}  // namespace qtng

#endif  // QTNG_HPACK_P_H

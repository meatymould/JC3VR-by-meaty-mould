#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
namespace jc3vr {
// D3D11 Map/Unmap ownership is context + resource + subresource, not resource
// alone. Deferred contexts can record separate discard maps of one buffer.
struct MappedWriteKey {
    const void* context{};
    const void* resource{};
    unsigned subresource{};
    bool operator==(const MappedWriteKey&) const = default;
};
struct MappedWriteKeyHash {
    std::size_t operator()(const MappedWriteKey& key) const {
        auto h=std::hash<const void*>{}(key.context);
        h ^= std::hash<const void*>{}(key.resource)+(h<<6)+(h>>2);
        return h ^ (std::hash<unsigned>{}(key.subresource)+(h<<6)+(h>>2));
    }
};
template<class Write> class MappedWriteRegistry {
    std::mutex mutex;
    std::unordered_map<MappedWriteKey,Write,MappedWriteKeyHash> entries;
public:
    void Publish(MappedWriteKey key,Write write) {
        std::lock_guard lock(mutex); entries.insert_or_assign(key,write);
    }
    bool Consume(MappedWriteKey key,Write& write) {
        std::lock_guard lock(mutex);
        const auto it=entries.find(key);
        if(it==entries.end()) return false;
        write=it->second; entries.erase(it); return true;
    }
};
}

#pragma once

#include <unordered_map>
#include <list>
#include <functional>
#include <optional>

template <typename Key, typename Value>
class STLRUCache {
public:
    using iterator = typename std::list<std::pair<Key, Value>>::iterator;
    using const_iterator = typename std::list<std::pair<Key, Value>>::const_iterator;

    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }
    const_iterator cbegin() const { return items_.cbegin(); }
    const_iterator cend() const { return items_.cend(); }

    using EvictCallback = std::function<void(const Key&, const Value&)>;

    STLRUCache(size_t maxSize, EvictCallback onEvict = nullptr)
        : maxSize_(maxSize), onEvict_(std::move(onEvict)) {
        map_.reserve(maxSize_);
    }

    // Get a pointer to the value, or nullptr if not found.
    Value* get(const Key& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return nullptr;

        // Move accessed element to front (MRU position)
        items_.splice(items_.begin(), items_, it->second);
        return &it->second->second;
    }

    // Insert or update a value. Assumes the key doesn't exist
    Value& put(const Key& key, const Value& value) {
        // Insert new entry
        items_.emplace_front(key, value);
        map_[key] = items_.begin();

        // Evict if over capacity
        if (map_.size() > maxSize_) {
            auto& back = items_.back();
            if (onEvict_)
                onEvict_(back.first, back.second);

            map_.erase(back.first);
            items_.pop_back();
        }

        return items_.begin()->second;
    }

    size_t size() const { return map_.size(); }
    size_t capacity() const { return maxSize_; }

private:
    size_t maxSize_;
    EvictCallback onEvict_;

    // list of (key, value)
    std::list<std::pair<Key, Value>> items_;

    // map from key to node in list
    std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator> map_;
};

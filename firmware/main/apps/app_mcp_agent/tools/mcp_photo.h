/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <esp_heap_caps.h>

namespace mcp_photo {

// PSRAM-backed immutable byte buffer shared by the store and HTTP snapshots.
template <typename T>
struct PsramAllocator {
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = PsramAllocator<U>;
    };

    T* allocate(std::size_t count)
    {
        auto* p = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM);
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) { heap_caps_free(p); }
};

using PhotoBytes = std::vector<uint8_t, PsramAllocator<uint8_t>>;

struct PhotoSnapshot {
    std::shared_ptr<const PhotoBytes> bytes;
    int width = 0;
    int height = 0;
    int quality = 0;
    uint32_t captures = 0;
};

bool capture(int quality, std::string* err);

// Returns an immutable snapshot. The next capture may replace the store, but
// cannot invalidate a snapshot already held by an HTTP handler or a tool.
bool get(PhotoSnapshot* out);

// Releases the retained last photo when the owning App stops. Existing
// snapshots remain valid until their HTTP handlers finish.
void clear();

int width();
int height();
int quality();
uint32_t count();
std::string base64(std::string* err);

}  // namespace mcp_photo

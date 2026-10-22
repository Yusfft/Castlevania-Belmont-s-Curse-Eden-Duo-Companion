// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal Unity asset reader for the Castlevania: Belmont's Curse companion (0100BA4026E94000,
// Unity 2022.3.62f2, Switch). It reads the player's own romfs at runtime; nothing decoded is
// stored or shipped.
//
//   UnityFS bundle (format 6..8, "5.x.x"), header big endian:
//     signature, u32 format, cstr player version, cstr engine version, i64 size,
//     u32 compressed / uncompressed block-info size, u32 flags
//       flags & 0x3F   block-info compression (0 none, 2 LZ4, 3 LZ4HC; LZMA is refused)
//       flags & 0x80   block info at the end of the file
//       flags & 0x200  16-byte alignment before the first data block
//     format >= 7: the block info starts 16-aligned.
//     block info: 16-byte hash, i32 block count {u32 usize, u32 csize, u16 flags},
//                 i32 node count {i64 offset, i64 size, u32 flags, cstr path}
//   SerializedFile (version 22 here, little endian on Switch) with embedded type trees.
//   Objects are read by a generic type-tree walker into bc_unity::Value, following UnityPy
//   1.25's read_value rules (alignment, arrays, pairs). A ManagedReferencesRegistry field ends
//   the read of that object (the fields this companion needs all come before it).
//
// Every read is bounds-checked. Nothing throws out of the public API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bc_unity {

using Bytes = std::vector<std::uint8_t>;

/// Hard cap on any allocation driven by file contents.
inline constexpr std::size_t kMaxAlloc = 64u * 1024u * 1024u;

/// Decodes one LZ4 block (no frame header). Returns false on malformed input or a size mismatch.
bool Lz4Decompress(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                   std::size_t dst_size);

struct BundleNode {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t flags = 0;
    std::string path;
};

/// A decompressed UnityFS bundle: the concatenated block stream plus its directory.
struct Bundle {
    std::uint32_t format = 0;
    std::string engine_version;
    std::vector<BundleNode> nodes;
    Bytes data;

    static std::optional<Bundle> Parse(const std::uint8_t* file, std::size_t size,
                                       std::string* error = nullptr);
    /// The node's bytes, or nullopt when it is out of range.
    std::optional<std::pair<const std::uint8_t*, std::size_t>> NodeBytes(const BundleNode& n) const;
};

/// A type-tree value. Kinds mirror what UnityPy produces.
struct Value {
    enum class Kind : std::uint8_t { Null, Int, Float, String, Bytes, List, Object };
    Kind kind = Kind::Null;
    std::int64_t i = 0;
    double f = 0.0;
    std::string s;                                    // String / Bytes payload
    std::vector<Value> list;        // List (also pairs: two entries); Object field values
    std::vector<std::string> keys;  // Object field names, parallel to `list`, type-tree order

    const Value* Get(std::string_view name) const;
    std::int64_t AsInt(std::int64_t fallback = 0) const;
    double AsFloat(double fallback = 0.0) const;
    bool IsList() const { return kind == Kind::List; }
};

struct TypeTreeNode {
    std::string type;
    std::string name;
    std::int32_t byte_size = 0;
    std::uint8_t level = 0;
    std::uint32_t meta_flag = 0;
    std::vector<std::uint32_t> children; // indices into the owning tree's node list
};

struct SerializedType {
    std::int32_t class_id = 0;
    std::int16_t script_type_index = -1;
    std::vector<TypeTreeNode> nodes; // nodes[0] is the root when present
};

struct ObjectInfo {
    std::int64_t path_id = 0;
    std::uint64_t byte_start = 0;
    std::uint32_t byte_size = 0;
    std::int32_t type_index = -1;
    std::int32_t class_id = 0;
};

class SerializedFile {
public:
    static std::optional<SerializedFile> Parse(const std::uint8_t* data, std::size_t size,
                                               std::string* error = nullptr);

    std::uint32_t Version() const { return version_; }
    const std::string& UnityVersion() const { return unity_version_; }
    const std::vector<ObjectInfo>& Objects() const { return objects_; }
    const std::vector<SerializedType>& Types() const { return types_; }

    /// Reads an object through its type tree. `truncated` (optional) reports whether a
    /// ManagedReferencesRegistry field stopped the read early.
    std::optional<Value> ReadObject(const ObjectInfo& obj, bool* truncated = nullptr) const;

private:
    std::uint32_t version_ = 0;
    bool little_endian_ = true;
    std::string unity_version_;
    std::uint64_t data_offset_ = 0;
    std::vector<SerializedType> types_;
    std::vector<ObjectInfo> objects_;
    // The file bytes are owned here so objects can be read later.
    std::shared_ptr<const Bytes> bytes_;
};

} // namespace bc_unity

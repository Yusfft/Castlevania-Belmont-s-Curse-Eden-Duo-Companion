// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal UnityFS / SerializedFile / type-tree reader. See bc_unity.h.

#include "bc_unity.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "bc_common_strings.inc"

namespace bc_unity {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

void SetError(std::string* error, const char* what) {
    if (error)
        *error = what;
}

/// Bounded cursor over a byte range. Every read checks the bounds and sets `ok` to false on the
/// first failure; later reads return zero. Alignment is relative to the start of the range.
struct Cursor {
    const u8* p = nullptr;
    std::size_t n = 0;
    std::size_t pos = 0;
    bool le = false;
    bool ok = true;
    std::size_t base = 0; // absolute offset of p[0] in the file (alignment is absolute)

    bool Has(std::size_t k) const { return ok && pos <= n && k <= n - pos; }
    bool Skip(std::size_t k) {
        if (!Has(k)) {
            ok = false;
            return false;
        }
        pos += k;
        return true;
    }
    void Align(std::size_t a) {
        const std::size_t aligned = ((base + pos + (a - 1)) & ~(a - 1)) - base;
        if (aligned > n) {
            ok = false;
            return;
        }
        pos = aligned;
    }
    template <typename T>
    T Read() {
        if (!Has(sizeof(T))) {
            ok = false;
            return T{};
        }
        u64 v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            const u64 b = p[pos + i];
            v |= le ? (b << (8 * i)) : (b << (8 * (sizeof(T) - 1 - i)));
        }
        pos += sizeof(T);
        T out;
        std::memcpy(&out, &v, sizeof(T)); // little-endian host (arm64, x86-64)
        return out;
    }
    u8 U8() { return Read<u8>(); }
    u16 U16() { return Read<u16>(); }
    u32 U32() { return Read<u32>(); }
    u64 U64() { return Read<u64>(); }
    s16 S16() { return Read<s16>(); }
    s32 S32() { return Read<s32>(); }
    s64 S64() { return Read<s64>(); }
    float F32() {
        const u32 v = U32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    double F64() {
        const u64 v = U64();
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
    std::string CStr(std::size_t max = 4096) {
        std::string s;
        while (ok) {
            if (!Has(1)) {
                ok = false;
                break;
            }
            const char c = static_cast<char>(p[pos++]);
            if (c == '\0')
                return s;
            if (s.size() >= max) {
                ok = false;
                break;
            }
            s.push_back(c);
        }
        return {};
    }
};

std::string CommonStringAt(u32 offset) {
    for (const auto& cs : kCommonStrings) {
        if (cs.offset == offset)
            return cs.text;
    }
    return std::to_string(offset);
}

} // namespace

// ---------------------------------------------------------------------------------------- LZ4

bool Lz4Decompress(const u8* src, std::size_t src_size, u8* dst, std::size_t dst_size) {
    std::size_t ip = 0, op = 0;
    while (ip < src_size) {
        const u8 token = src[ip++];
        std::size_t lit = token >> 4;
        if (lit == 15) {
            u8 b;
            do {
                if (ip >= src_size)
                    return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > src_size - ip || lit > dst_size - op)
            return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= src_size)
            break; // the last sequence carries literals only
        if (src_size - ip < 2)
            return false;
        const std::size_t offset = src[ip] | (std::size_t(src[ip + 1]) << 8);
        ip += 2;
        if (offset == 0 || offset > op)
            return false;
        std::size_t match = token & 15;
        if (match == 15) {
            u8 b;
            do {
                if (ip >= src_size)
                    return false;
                b = src[ip++];
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (match > dst_size - op)
            return false;
        // Byte-wise copy: matches may overlap their own output.
        const u8* from = dst + op - offset;
        for (std::size_t k = 0; k < match; ++k)
            dst[op + k] = from[k];
        op += match;
    }
    return op == dst_size;
}

// ------------------------------------------------------------------------------------- Bundle

std::optional<Bundle> Bundle::Parse(const u8* file, std::size_t size, std::string* error) {
    Cursor c{file, size, 0, false, true};
    const std::string sig = c.CStr(16);
    if (!c.ok || sig != "UnityFS") {
        SetError(error, "not a UnityFS bundle");
        return std::nullopt;
    }
    Bundle b;
    b.format = c.U32();
    c.CStr(64); // player version "5.x.x"
    b.engine_version = c.CStr(64);
    const s64 total = c.S64();
    const u32 cinfo = c.U32();
    const u32 uinfo = c.U32();
    const u32 flags = c.U32();
    if (!c.ok || b.format < 6 || b.format > 8 || total <= 0) {
        SetError(error, "unsupported bundle header");
        return std::nullopt;
    }
    if (b.format >= 7)
        c.Align(16);
    if (cinfo > kMaxAlloc || uinfo > kMaxAlloc || !c.ok) {
        SetError(error, "bad block-info size");
        return std::nullopt;
    }
    const u8* info_src = nullptr;
    if (flags & 0x80) {
        if (cinfo > size) {
            SetError(error, "block info out of range");
            return std::nullopt;
        }
        info_src = file + (size - cinfo);
    } else {
        if (!c.Has(cinfo)) {
            SetError(error, "block info out of range");
            return std::nullopt;
        }
        info_src = file + c.pos;
        c.pos += cinfo;
    }
    Bytes info(uinfo);
    switch (flags & 0x3F) {
    case 0:
        if (cinfo != uinfo) {
            SetError(error, "uncompressed block info size mismatch");
            return std::nullopt;
        }
        std::memcpy(info.data(), info_src, uinfo);
        break;
    case 2:
    case 3:
        if (!Lz4Decompress(info_src, cinfo, info.data(), uinfo)) {
            SetError(error, "block info LZ4 failed");
            return std::nullopt;
        }
        break;
    default:
        SetError(error, "unsupported block info compression");
        return std::nullopt;
    }
    if (flags & 0x200)
        c.Align(16);
    if (!c.ok) {
        SetError(error, "truncated bundle");
        return std::nullopt;
    }

    Cursor ic{info.data(), info.size(), 0, false, true};
    ic.Skip(16); // uncompressed data hash
    const s32 block_count = ic.S32();
    if (!ic.ok || block_count < 0 || block_count > 1 << 16) {
        SetError(error, "bad block count");
        return std::nullopt;
    }
    struct Block {
        u32 usize, csize;
        u16 flags;
    };
    std::vector<Block> blocks(static_cast<std::size_t>(block_count));
    u64 total_u = 0;
    for (auto& bl : blocks) {
        bl.usize = ic.U32();
        bl.csize = ic.U32();
        bl.flags = ic.U16();
        total_u += bl.usize;
    }
    const s32 node_count = ic.S32();
    if (!ic.ok || node_count < 0 || node_count > 4096 || total_u > kMaxAlloc) {
        SetError(error, "bad node table");
        return std::nullopt;
    }
    for (s32 i = 0; i < node_count; ++i) {
        BundleNode node;
        node.offset = ic.U64();
        node.size = ic.U64();
        node.flags = ic.U32();
        node.path = ic.CStr(1024);
        if (!ic.ok) {
            SetError(error, "truncated node table");
            return std::nullopt;
        }
        b.nodes.push_back(std::move(node));
    }

    b.data.resize(static_cast<std::size_t>(total_u));
    std::size_t out = 0;
    for (const auto& bl : blocks) {
        if (!c.Has(bl.csize)) {
            SetError(error, "block out of range");
            return std::nullopt;
        }
        const u8* src = file + c.pos;
        c.pos += bl.csize;
        switch (bl.flags & 0x3F) {
        case 0:
            if (bl.csize != bl.usize) {
                SetError(error, "stored block size mismatch");
                return std::nullopt;
            }
            std::memcpy(b.data.data() + out, src, bl.usize);
            break;
        case 2:
        case 3:
            if (!Lz4Decompress(src, bl.csize, b.data.data() + out, bl.usize)) {
                SetError(error, "data block LZ4 failed");
                return std::nullopt;
            }
            break;
        default:
            SetError(error, "unsupported data block compression (LZMA?)");
            return std::nullopt;
        }
        out += bl.usize;
    }
    return b;
}

std::optional<std::pair<const u8*, std::size_t>> Bundle::NodeBytes(const BundleNode& n) const {
    if (n.offset > data.size() || n.size > data.size() - n.offset)
        return std::nullopt;
    return std::make_pair(data.data() + n.offset, static_cast<std::size_t>(n.size));
}

// --------------------------------------------------------------------------------- LazyBundle

std::shared_ptr<LazyBundle> LazyBundle::Open(ReadAt read, std::uint64_t file_size, std::string* error) {
    if (!read || file_size < 64) {
        SetError(error, "no file");
        return nullptr;
    }
    // Header: read a generous prefix; the block info follows it unless flagged at the end.
    Bytes head(static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 4096)));
    if (read(0, head.data(), head.size()) != head.size()) {
        SetError(error, "short read");
        return nullptr;
    }
    Cursor c{head.data(), head.size(), 0, false, true};
    const std::string sig = c.CStr(16);
    if (!c.ok || sig != "UnityFS") {
        SetError(error, "not a UnityFS bundle");
        return nullptr;
    }
    const u32 format = c.U32();
    c.CStr(64);
    c.CStr(64);
    const s64 total = c.S64();
    const u32 cinfo = c.U32();
    const u32 uinfo = c.U32();
    const u32 flags = c.U32();
    if (!c.ok || format < 6 || format > 8 || total <= 0 || cinfo > kMaxAlloc || uinfo > kMaxAlloc) {
        SetError(error, "unsupported bundle header");
        return nullptr;
    }
    if (format >= 7)
        c.Align(16);
    u64 info_off = 0;
    if (flags & 0x80) {
        if (cinfo > file_size) {
            SetError(error, "block info out of range");
            return nullptr;
        }
        info_off = file_size - cinfo;
    } else {
        info_off = c.pos;
    }
    Bytes cblock(cinfo);
    if (read(info_off, cblock.data(), cblock.size()) != cblock.size()) {
        SetError(error, "short block info read");
        return nullptr;
    }
    u64 data_off = (flags & 0x80) ? c.pos : info_off + cinfo;
    if (flags & 0x200)
        data_off = (data_off + 15) & ~u64{15};
    Bytes info(uinfo);
    switch (flags & 0x3F) {
    case 0:
        if (cinfo != uinfo)
            return nullptr;
        info = cblock;
        break;
    case 2:
    case 3:
        if (!Lz4Decompress(cblock.data(), cinfo, info.data(), uinfo)) {
            SetError(error, "block info LZ4 failed");
            return nullptr;
        }
        break;
    default:
        SetError(error, "unsupported block info compression");
        return nullptr;
    }
    auto b = std::make_shared<LazyBundle>();
    b->read_ = std::move(read);
    Cursor ic{info.data(), info.size(), 0, false, true};
    ic.Skip(16);
    const s32 block_count = ic.S32();
    if (!ic.ok || block_count < 0 || block_count > 1 << 20) {
        SetError(error, "bad block count");
        return nullptr;
    }
    u64 coff = data_off, uoff = 0;
    b->blocks_.resize(static_cast<std::size_t>(block_count));
    for (auto& bl : b->blocks_) {
        bl.usize = ic.U32();
        bl.csize = ic.U32();
        bl.flags = ic.U16();
        bl.coff = coff;
        bl.uoff = uoff;
        coff += bl.csize;
        uoff += bl.usize;
    }
    const s32 node_count = ic.S32();
    if (!ic.ok || node_count < 0 || node_count > 4096 || coff > file_size) {
        SetError(error, "bad node table");
        return nullptr;
    }
    for (s32 i = 0; i < node_count; ++i) {
        BundleNode node;
        node.offset = ic.U64();
        node.size = ic.U64();
        node.flags = ic.U32();
        node.path = ic.CStr(1024);
        if (!ic.ok || node.offset > uoff || node.size > uoff - node.offset) {
            SetError(error, "bad node");
            return nullptr;
        }
        b->nodes_.push_back(std::move(node));
    }
    return b;
}

const BundleNode* LazyBundle::FindNode(std::string_view path) const {
    for (const auto& n : nodes_) {
        if (n.path == path)
            return &n;
        if (n.path.size() > path.size() && n.path.compare(n.path.size() - path.size(), path.size(), path) == 0 &&
            n.path[n.path.size() - path.size() - 1] == '/')
            return &n;
    }
    return nullptr;
}

bool LazyBundle::LoadBlock(std::size_t index, Bytes& out) {
    const Block& bl = blocks_[index];
    if (bl.usize > kMaxAlloc || bl.csize > kMaxAlloc)
        return false;
    Bytes src(bl.csize);
    if (read_(bl.coff, src.data(), src.size()) != src.size())
        return false;
    out.resize(bl.usize);
    switch (bl.flags & 0x3F) {
    case 0:
        if (bl.csize != bl.usize)
            return false;
        out = std::move(src);
        return true;
    case 2:
    case 3:
        return Lz4Decompress(src.data(), src.size(), out.data(), out.size());
    default:
        return false;
    }
}

bool LazyBundle::Read(std::uint64_t offset, std::size_t size, std::uint8_t* out) {
    if (size == 0)
        return true;
    if (blocks_.empty())
        return false;
    const u64 end = offset + size;
    if (end < offset || end > blocks_.back().uoff + blocks_.back().usize)
        return false;
    // First block containing `offset`.
    std::size_t lo = 0, hi = blocks_.size();
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (blocks_[mid].uoff <= offset)
            lo = mid;
        else
            hi = mid;
    }
    u64 pos = offset;
    for (std::size_t i = lo; i < blocks_.size() && pos < end; ++i) {
        const Block& bl = blocks_[i];
        std::shared_ptr<Bytes> data;
        {
            std::lock_guard lock(mu_);
            for (std::size_t k = 0; k < cache_.size(); ++k) {
                if (cache_[k].first == i) {
                    data = cache_[k].second;
                    if (k) {
                        auto e = cache_[k];
                        cache_.erase(cache_.begin() + static_cast<std::ptrdiff_t>(k));
                        cache_.insert(cache_.begin(), e);
                    }
                    break;
                }
            }
        }
        if (!data) {
            auto fresh = std::make_shared<Bytes>();
            if (!LoadBlock(i, *fresh))
                return false;
            data = fresh;
            std::lock_guard lock(mu_);
            cache_.insert(cache_.begin(), {i, data});
            if (cache_.size() > 8)
                cache_.pop_back();
        }
        const u64 from = pos - bl.uoff;
        const u64 take = std::min<u64>(bl.usize - from, end - pos);
        std::memcpy(out + (pos - offset), data->data() + from, static_cast<std::size_t>(take));
        pos += take;
    }
    return pos == end;
}

bool LazyBundle::ReadNodeRange(const BundleNode& n, std::uint64_t offset, std::size_t size, std::uint8_t* out) {
    if (offset > n.size || size > n.size - offset)
        return false;
    return Read(n.offset + offset, size, out);
}

std::optional<Bytes> LazyBundle::ReadNode(const BundleNode& n) {
    if (n.size > kMaxAlloc)
        return std::nullopt;
    Bytes out(static_cast<std::size_t>(n.size));
    if (!Read(n.offset, out.size(), out.data()))
        return std::nullopt;
    return out;
}

// -------------------------------------------------------------------------------------- Value

const Value* Value::Get(std::string_view name) const {
    if (kind != Kind::Object)
        return nullptr;
    for (std::size_t k = 0; k < keys.size() && k < list.size(); ++k) {
        if (keys[k] == name)
            return &list[k];
    }
    return nullptr;
}

std::int64_t Value::AsInt(std::int64_t fallback) const {
    if (kind == Kind::Int)
        return i;
    if (kind == Kind::Float)
        return static_cast<std::int64_t>(f);
    return fallback;
}

double Value::AsFloat(double fallback) const {
    if (kind == Kind::Float)
        return f;
    if (kind == Kind::Int)
        return static_cast<double>(i);
    return fallback;
}

// ----------------------------------------------------------------------------- SerializedFile

namespace {

constexpr u32 kAlignBytes = 0x4000;

bool ParseTypeTreeBlob(Cursor& c, u32 version, std::vector<TypeTreeNode>& out) {
    const s32 node_count = c.S32();
    const s32 string_size = c.S32();
    if (!c.ok || node_count < 0 || node_count > 65536 || string_size < 0 ||
        static_cast<std::size_t>(string_size) > kMaxAlloc)
        return false;
    const std::size_t node_size = version >= 19 ? 32 : 24;
    if (!c.Has(static_cast<std::size_t>(node_count) * node_size + string_size))
        return false;
    struct Raw {
        u8 level;
        u32 type_off, name_off, meta;
        s32 byte_size;
    };
    std::vector<Raw> raw(static_cast<std::size_t>(node_count));
    for (auto& r : raw) {
        c.U16(); // node version
        r.level = c.U8();
        c.U8(); // type flags
        r.type_off = c.U32();
        r.name_off = c.U32();
        r.byte_size = c.S32();
        c.S32(); // index
        r.meta = c.U32();
        if (version >= 19)
            c.U64(); // ref type hash
    }
    const u8* strings = c.p + c.pos;
    c.Skip(static_cast<std::size_t>(string_size));
    if (!c.ok)
        return false;
    const auto str = [&](u32 off) -> std::string {
        if (off & 0x80000000u)
            return CommonStringAt(off & 0x7FFFFFFFu);
        if (off >= static_cast<u32>(string_size))
            return {};
        const u8* s = strings + off;
        const std::size_t room = static_cast<std::size_t>(string_size) - off;
        const void* nul = std::memchr(s, 0, room);
        const std::size_t len = nul ? static_cast<const u8*>(nul) - s : room;
        return std::string(reinterpret_cast<const char*>(s), len);
    };
    out.clear();
    out.reserve(raw.size());
    std::vector<u32> stack; // index of the last node seen at each level
    for (std::size_t i = 0; i < raw.size(); ++i) {
        TypeTreeNode n;
        n.type = str(raw[i].type_off);
        n.name = str(raw[i].name_off);
        n.byte_size = raw[i].byte_size;
        n.level = raw[i].level;
        n.meta_flag = raw[i].meta;
        out.push_back(std::move(n));
        const u32 level = raw[i].level;
        if (i == 0) {
            if (level != 0)
                return false;
        } else {
            if (level == 0 || level > stack.size())
                return false;
            out[stack[level - 1]].children.push_back(static_cast<u32>(i));
        }
        stack.resize(level);
        stack.push_back(static_cast<u32>(i));
    }
    return true;
}

bool ParseSerializedType(Cursor& c, u32 version, bool enable_tree, bool is_ref,
                         SerializedType& t) {
    t.class_id = c.S32();
    if (version >= 16)
        c.U8(); // is stripped
    if (version >= 17)
        t.script_type_index = c.S16();
    if (version >= 13) {
        if ((is_ref && t.script_type_index >= 0) || (version < 16 && t.class_id < 0) ||
            (version >= 16 && t.class_id == 114))
            c.Skip(16); // script id
        c.Skip(16);     // old type hash
    }
    if (!c.ok)
        return false;
    if (enable_tree) {
        if (version < 12 && version != 10)
            return false; // pre-5.x type trees are not supported (not used by this game)
        if (!ParseTypeTreeBlob(c, version, t.nodes))
            return false;
        if (version >= 21) {
            if (is_ref) {
                t.class_name = c.CStr(1024);
                t.name_space = c.CStr(1024);
                t.assembly = c.CStr(1024);
            } else {
                const s32 deps = c.S32();
                if (deps < 0 || deps > 1 << 20)
                    return false;
                c.Skip(static_cast<std::size_t>(deps) * 4);
            }
        }
    }
    return c.ok;
}

// The type-tree walker, following UnityPy's read_value. A ManagedReferencesRegistry is read when
// the file carries the referenced types (SerializedFile v20+); a nested registry is skipped, as
// UnityPy does. `stopped` is set when a registry cannot be read; everything before it is kept.
struct Walker {
    using Tree = std::vector<TypeTreeNode>;
    Cursor& c;
    const std::vector<SerializedType>* ref_types = nullptr;
    bool stopped = false;
    bool in_registry = false;
    std::size_t budget = 4u * 1024u * 1024u; // max values created by one object read

    bool Spend() {
        if (budget == 0) {
            c.ok = false;
            return false;
        }
        --budget;
        return true;
    }

    static bool Aligned(u32 meta) { return (meta & kAlignBytes) != 0; }

    static bool IsArrayNode(const Tree& t, const TypeTreeNode& n) {
        return !n.children.empty() && t[n.children[0]].type == "Array";
    }

    // Reads one primitive / string / typeless value. Returns false when `n` is not one of them.
    bool ReadLeaf(const TypeTreeNode& n, Value& v) {
        const std::string& t = n.type;
        const auto as_int = [&](s64 x) {
            v.kind = Value::Kind::Int;
            v.i = x;
        };
        if (t == "SInt8")
            as_int(static_cast<std::int8_t>(c.U8()));
        else if (t == "UInt8" || t == "char" || t == "bool")
            as_int(c.U8());
        else if (t == "short" || t == "SInt16")
            as_int(c.S16());
        else if (t == "unsigned short" || t == "UInt16")
            as_int(c.U16());
        else if (t == "int" || t == "SInt32")
            as_int(c.S32());
        else if (t == "unsigned int" || t == "UInt32" || t == "Type*")
            as_int(c.U32());
        else if (t == "long long" || t == "SInt64")
            as_int(c.S64());
        else if (t == "unsigned long long" || t == "UInt64" || t == "FileSize")
            as_int(static_cast<s64>(c.U64()));
        else if (t == "float") {
            v.kind = Value::Kind::Float;
            v.f = c.F32();
        } else if (t == "double") {
            v.kind = Value::Kind::Float;
            v.f = c.F64();
        } else if (t == "string") {
            v.kind = Value::Kind::String;
            const s32 len = c.S32();
            if (len > 0 && c.Has(static_cast<std::size_t>(len))) {
                v.s.assign(reinterpret_cast<const char*>(c.p + c.pos), static_cast<std::size_t>(len));
                c.pos += static_cast<std::size_t>(len);
                c.Align(4);
            }
        } else if (t == "TypelessData") {
            v.kind = Value::Kind::Bytes;
            const s32 len = c.S32();
            if (len < 0 || !c.Has(static_cast<std::size_t>(len))) {
                c.ok = false;
                return true;
            }
            v.s.assign(reinterpret_cast<const char*>(c.p + c.pos), static_cast<std::size_t>(len));
            c.pos += static_cast<std::size_t>(len);
        } else {
            return false;
        }
        return true;
    }

    const SerializedType* RefType(const Value& obj) const {
        const Value* type = obj.Get("type");
        if (!type || !ref_types)
            return nullptr;
        const Value* cls = type->Get("class");
        const Value* ns = type->Get("ns");
        const Value* asm_ = type->Get("asm");
        if (!cls || cls->s.empty())
            return nullptr;
        for (const auto& rt : *ref_types) {
            if (rt.class_name == cls->s && (!ns || rt.name_space == ns->s) && (!asm_ || rt.assembly == asm_->s))
                return &rt;
        }
        return nullptr;
    }

    // One ReferencedObject: rid, type, then the data laid out by the referenced type.
    void ReadReferencedObject(const Tree& t, const TypeTreeNode& n, Value& v) {
        v.kind = Value::Kind::Object;
        for (const u32 ci : n.children) {
            const TypeTreeNode& child = t[ci];
            if (child.type == "ReferencedObjectData") {
                const Value* type = v.Get("type");
                const Value* cls = type ? type->Get("class") : nullptr;
                if (!cls || cls->s.empty())
                    continue; // terminator entry: no data
                const SerializedType* rt = RefType(v);
                if (!rt || rt->nodes.empty()) {
                    stopped = true; // cannot know the layout; keep what was read
                    return;
                }
                v.keys.push_back(child.name);
                v.list.emplace_back();
                Read(rt->nodes, rt->nodes[0], v.list.back());
            } else {
                v.keys.push_back(child.name);
                v.list.emplace_back();
                Read(t, child, v.list.back());
            }
            if (!c.ok || stopped)
                return;
        }
    }

    // UnityPy read_value. `own_align` false is used for elements read in bulk by ReadArrayBulk,
    // whose single trailing alignment is applied there instead.
    void Read(const Tree& t, const TypeTreeNode& n, Value& v, bool own_align = true) {
        if (!c.ok || stopped || !Spend())
            return;
        bool align = own_align && Aligned(n.meta_flag);
        if (ReadLeaf(n, v)) {
            // primitive done
        } else if (n.type == "pair" && n.children.size() >= 2) {
            v.kind = Value::Kind::List;
            v.list.resize(2);
            Read(t, t[n.children[0]], v.list[0]);
            Read(t, t[n.children[1]], v.list[1]);
        } else if (n.type == "ReferencedObject") {
            ReadReferencedObject(t, n, v);
        } else if (IsArrayNode(t, n)) {
            const TypeTreeNode& arr = t[n.children[0]];
            if (own_align && Aligned(arr.meta_flag))
                align = true;
            const s32 size = c.S32();
            if (!c.ok || size < 0 || arr.children.size() < 2 ||
                static_cast<std::size_t>(size) > c.n) {
                c.ok = false;
                return;
            }
            const TypeTreeNode& sub = t[arr.children[1]];
            v.kind = Value::Kind::List;
            if (Aligned(sub.meta_flag)) {
                ReadArrayBulk(t, sub, static_cast<std::size_t>(size), v);
            } else {
                v.list.resize(static_cast<std::size_t>(size));
                for (auto& e : v.list) {
                    Read(t, sub, e);
                    if (!c.ok || stopped)
                        break;
                }
            }
        } else {
            v.kind = Value::Kind::Object;
            for (const u32 ci : n.children) {
                const TypeTreeNode& child = t[ci];
                if (child.type == "ManagedReferencesRegistry") {
                    if (in_registry)
                        continue; // nested registries are not serialized
                    if (!ref_types) {
                        stopped = true;
                        return;
                    }
                    in_registry = true;
                }
                v.keys.push_back(child.name);
                v.list.emplace_back();
                Read(t, child, v.list.back());
                if (!c.ok || stopped)
                    return;
            }
        }
        if (align && c.ok && !stopped)
            c.Align(4);
    }

    // UnityPy read_value_array: `size` elements of an aligned element type, then one alignment.
    void ReadArrayBulk(const Tree& t, const TypeTreeNode& sub, std::size_t size, Value& v) {
        v.list.resize(size);
        for (auto& e : v.list) {
            if (IsArrayNode(t, sub)) {
                // Nested vector inside a bulk array: each inner vector reads its own size and
                // elements; alignment happens once at the end (UnityPy semantics).
                const TypeTreeNode& arr = t[sub.children[0]];
                const s32 inner = c.S32();
                if (!c.ok || inner < 0 || arr.children.size() < 2 ||
                    static_cast<std::size_t>(inner) > c.n) {
                    c.ok = false;
                    return;
                }
                e.kind = Value::Kind::List;
                const TypeTreeNode& isub = t[arr.children[1]];
                if (Aligned(isub.meta_flag)) {
                    ReadArrayBulk(t, isub, static_cast<std::size_t>(inner), e);
                } else {
                    e.list.resize(static_cast<std::size_t>(inner));
                    for (auto& ie : e.list)
                        Read(t, isub, ie);
                }
            } else if (sub.type == "ReferencedObject") {
                ReadReferencedObject(t, sub, e);
            } else {
                Read(t, sub, e, /*own_align=*/false);
            }
            if (!c.ok || stopped)
                return;
        }
        if (c.ok && !stopped)
            c.Align(4);
    }
};

} // namespace

std::optional<SerializedFile> SerializedFile::Parse(const u8* data, std::size_t size,
                                                    std::string* error) {
    auto owned = std::make_shared<Bytes>(data, data + size);
    ReadRange read = [owned](u64 off, std::size_t n, u8* out) {
        if (off > owned->size() || n > owned->size() - off)
            return false;
        std::memcpy(out, owned->data() + off, n);
        return true;
    };
    return Open(std::move(read), size, error);
}

std::optional<SerializedFile> SerializedFile::Open(ReadRange read, std::uint64_t size, std::string* error) {
    if (size < 48 || !read) {
        SetError(error, "serialized file too small");
        return std::nullopt;
    }
    // The header tells how much metadata follows it.
    u8 head[48];
    if (!read(0, sizeof head, head)) {
        SetError(error, "serialized header unreadable");
        return std::nullopt;
    }
    std::size_t meta_end = 0;
    {
        Cursor h{head, sizeof head, 0, false, true};
        const u32 meta_size = h.U32();
        h.U32();
        const u32 version = h.U32();
        h.U32();
        const u8 endian = h.U8();
        h.Skip(3);
        u64 m = meta_size;
        if (version >= 22) {
            h.le = false;
            m = h.U32();
        }
        (void)endian;
        meta_end = static_cast<std::size_t>(std::min<u64>(size, m + 64));
        if (!h.ok || meta_end > kMaxAlloc) {
            SetError(error, "bad serialized header");
            return std::nullopt;
        }
    }
    Bytes meta(meta_end);
    if (!read(0, meta.size(), meta.data())) {
        SetError(error, "serialized metadata unreadable");
        return std::nullopt;
    }
    SerializedFile f;
    f.read_ = std::move(read);
    f.file_size_ = size;
    const u8* data = meta.data();
    (void)data;
    Cursor c{meta.data(), meta.size(), 0, false, true};
    c.U32(); // legacy metadata size
    c.U32(); // legacy file size
    f.version_ = c.U32();
    u64 data_offset = c.U32();
    if (f.version_ < 9 || f.version_ > 23) {
        SetError(error, "unsupported serialized file version");
        return std::nullopt;
    }
    const u8 endian = c.U8();
    c.Skip(3);
    if (f.version_ >= 22) {
        c.U32();               // metadata size
        c.S64();               // file size
        data_offset = c.U64(); // data offset
        c.S64();               // unknown
    }
    if (!c.ok || data_offset > f.file_size_) {
        SetError(error, "bad serialized header");
        return std::nullopt;
    }
    f.data_offset_ = data_offset;
    f.little_endian_ = endian == 0;
    c.le = f.little_endian_;

    if (f.version_ >= 7)
        f.unity_version_ = c.CStr(64);
    if (f.version_ >= 8)
        c.S32(); // target platform
    bool enable_tree = true;
    if (f.version_ >= 13)
        enable_tree = c.U8() != 0;
    const s32 type_count = c.S32();
    if (!c.ok || type_count < 0 || type_count > 65536) {
        SetError(error, "bad type count");
        return std::nullopt;
    }
    f.types_.resize(static_cast<std::size_t>(type_count));
    for (auto& t : f.types_) {
        if (!ParseSerializedType(c, f.version_, enable_tree, false, t)) {
            SetError(error, "bad serialized type");
            return std::nullopt;
        }
    }
    if (f.version_ >= 7 && f.version_ < 14)
        c.S32(); // big id enabled
    const s32 object_count = c.S32();
    if (!c.ok || object_count < 0 || object_count > 1 << 22) {
        SetError(error, "bad object count");
        return std::nullopt;
    }
    f.objects_.reserve(static_cast<std::size_t>(object_count));
    for (s32 i = 0; i < object_count; ++i) {
        ObjectInfo o;
        if (f.version_ >= 14) {
            c.Align(4);
            o.path_id = c.S64();
        } else {
            o.path_id = c.S32();
        }
        o.byte_start = f.version_ >= 22 ? c.U64() : c.U32();
        o.byte_size = c.U32();
        o.type_index = c.S32();
        if (f.version_ < 16)
            o.class_id = c.U16();
        else if (o.type_index >= 0 && o.type_index < type_count)
            o.class_id = f.types_[static_cast<std::size_t>(o.type_index)].class_id;
        if (f.version_ < 11)
            c.U16();
        if (f.version_ >= 11 && f.version_ < 17)
            c.S16();
        if (f.version_ == 15 || f.version_ == 16)
            c.U8();
        if (!c.ok) {
            SetError(error, "truncated object table");
            return std::nullopt;
        }
        f.objects_.push_back(o);
    }
    if (f.version_ >= 11) {
        const s32 scripts = c.S32();
        if (!c.ok || scripts < 0 || scripts > 1 << 20) {
            SetError(error, "bad script table");
            return std::nullopt;
        }
        for (s32 i = 0; i < scripts; ++i) {
            c.S32(); // local file index
            if (f.version_ >= 14) {
                c.Align(4);
                c.S64();
            } else {
                c.S32();
            }
        }
    }
    const s32 externals = c.S32();
    if (!c.ok || externals < 0 || externals > 1 << 16) {
        SetError(error, "bad externals table");
        return std::nullopt;
    }
    for (s32 i = 0; i < externals; ++i) {
        if (f.version_ >= 6)
            c.CStr(1024); // temp empty
        if (f.version_ >= 5) {
            c.Skip(16); // guid
            c.S32();    // type
        }
        f.externals_.push_back(c.CStr(1024));
    }
    if (f.version_ >= 20) {
        const s32 ref_count = c.S32();
        if (c.ok && ref_count >= 0 && ref_count <= 65536) {
            f.ref_types_.resize(static_cast<std::size_t>(ref_count));
            for (auto& t : f.ref_types_) {
                if (!ParseSerializedType(c, f.version_, enable_tree, true, t)) {
                    f.ref_types_.clear(); // registries then stop the object read, as before
                    break;
                }
            }
        }
    }
    if (!c.ok) {
        SetError(error, "truncated serialized metadata");
        return std::nullopt;
    }
    return f;
}

const ObjectInfo* SerializedFile::Find(std::int64_t path_id) const {
    if (index_.empty() && !objects_.empty()) {
        index_.reserve(objects_.size());
        for (std::size_t i = 0; i < objects_.size(); ++i)
            index_.emplace(objects_[i].path_id, i);
    }
    const auto it = index_.find(path_id);
    return it == index_.end() ? nullptr : &objects_[it->second];
}

bool SerializedFile::ReadObjectBytes(const ObjectInfo& obj, std::size_t limit, Bytes& out) const {
    const u64 start = data_offset_ + obj.byte_start;
    if (!read_ || start > file_size_ || obj.byte_size > file_size_ - start || obj.byte_size > kMaxAlloc)
        return false;
    out.resize(std::min<std::size_t>(obj.byte_size, limit));
    return read_(start, out.size(), out.data());
}

std::optional<Value> SerializedFile::ReadObject(const ObjectInfo& obj, bool* truncated) const {
    if (truncated)
        *truncated = false;
    if (obj.type_index < 0 || static_cast<std::size_t>(obj.type_index) >= types_.size())
        return std::nullopt;
    const auto& type = types_[static_cast<std::size_t>(obj.type_index)];
    if (type.nodes.empty())
        return std::nullopt;
    Bytes buf;
    if (!ReadObjectBytes(obj, obj.byte_size, buf))
        return std::nullopt;
    // UnityPy aligns relative to the start of the file: the cursor carries the absolute base.
    Cursor c{buf.data(), buf.size(), 0, little_endian_, true};
    c.base = static_cast<std::size_t>(data_offset_ + obj.byte_start);
    Walker w{c};
    w.ref_types = ref_types_.empty() ? nullptr : &ref_types_;
    Value v;
    w.Read(type.nodes, type.nodes[0], v);
    if (!c.ok)
        return std::nullopt;
    if (truncated)
        *truncated = w.stopped;
    return v;
}

const SerializedType* SerializedFile::TypeOf(const ObjectInfo& obj) const {
    if (obj.type_index < 0 || static_cast<std::size_t>(obj.type_index) >= types_.size())
        return nullptr;
    return &types_[static_cast<std::size_t>(obj.type_index)];
}

bool SerializedFile::HasField(const SerializedType& type, std::string_view name) {
    if (type.nodes.empty())
        return false;
    for (const u32 ci : type.nodes[0].children) {
        if (ci < type.nodes.size() && type.nodes[ci].name == name)
            return true;
    }
    return false;
}

std::string SerializedFile::PeekName(const ObjectInfo& obj) const {
    const SerializedType* type = TypeOf(obj);
    if (!type || type->nodes.empty())
        return {};
    Bytes buf;
    if (!ReadObjectBytes(obj, 1024, buf))
        return {};
    Cursor c{buf.data(), buf.size(), 0, little_endian_, true};
    c.base = static_cast<std::size_t>(data_offset_ + obj.byte_start);
    Walker w{c};
    w.budget = 4096;
    for (const u32 ci : type->nodes[0].children) {
        if (ci >= type->nodes.size())
            return {};
        const TypeTreeNode& child = type->nodes[ci];
        Value v;
        w.Read(type->nodes, child, v);
        if (!c.ok || w.stopped)
            return {};
        if (child.name == "m_Name")
            return v.kind == Value::Kind::String ? v.s : std::string{};
    }
    return {};
}

} // namespace bc_unity

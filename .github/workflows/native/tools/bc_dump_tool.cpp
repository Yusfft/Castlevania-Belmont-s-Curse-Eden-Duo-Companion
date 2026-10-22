// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: dumps every MonoBehaviour of a bundle as JSON lines ({"path_id":..,"value":..}),
// so the reader can be compared with UnityPy on the same file.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "bc_unity.h"

using namespace bc_unity;

static void Json(const Value& v, std::string& out) {
    char buf[64];
    switch (v.kind) {
    case Value::Kind::Null:
        out += "null";
        break;
    case Value::Kind::Int:
        out += std::to_string(v.i);
        break;
    case Value::Kind::Float:
        if (!std::isfinite(v.f)) {
            out += "null";
        } else {
            std::snprintf(buf, sizeof buf, "%.9g", v.f);
            out += buf;
        }
        break;
    case Value::Kind::String:
    case Value::Kind::Bytes:
        out += '"';
        for (unsigned char ch : v.s) {
            if (ch == '"' || ch == '\\') {
                out += '\\';
                out += static_cast<char>(ch);
            } else if (ch < 0x20) {
                std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
        out += '"';
        break;
    case Value::Kind::List:
        out += '[';
        for (std::size_t i = 0; i < v.list.size(); ++i) {
            if (i)
                out += ',';
            Json(v.list[i], out);
        }
        out += ']';
        break;
    case Value::Kind::Object:
        out += '{';
        for (std::size_t i = 0; i < v.keys.size() && i < v.list.size(); ++i) {
            if (i)
                out += ',';
            Value key;
            key.kind = Value::Kind::String;
            key.s = v.keys[i];
            Json(key, out);
            out += ':';
            Json(v.list[i], out);
        }
        out += '}';
        break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <bundle>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    std::string err;
    auto b = Bundle::Parse(file.data(), file.size(), &err);
    if (!b) {
        std::fprintf(stderr, "bundle: %s\n", err.c_str());
        return 1;
    }
    for (const auto& node : b->nodes) {
        if (node.path.find(".res") != std::string::npos)
            continue;
        auto bytes = b->NodeBytes(node);
        if (!bytes)
            continue;
        auto sf = SerializedFile::Parse(bytes->first, bytes->second, &err);
        if (!sf) {
            std::fprintf(stderr, "serialized %s: %s\n", node.path.c_str(), err.c_str());
            continue;
        }
        for (const auto& o : sf->Objects()) {
            if (o.class_id != 114)
                continue;
            bool truncated = false;
            auto v = sf->ReadObject(o, &truncated);
            std::string line = "{\"path_id\":" + std::to_string(o.path_id) +
                               ",\"truncated\":" + (truncated ? "true" : "false") + ",\"value\":";
            if (v)
                Json(*v, line);
            else
                line += "null";
            line += "}";
            std::puts(line.c_str());
        }
    }
    return 0;
}

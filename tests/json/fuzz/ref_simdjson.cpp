// ref_simdjson.cpp -- simdjson (DOM API) as a reference for difffuzz.py.
// Same protocol and canonical form as ref_yyjson.c. simdjson is fetched and
// built by run_difffuzz.sh (see bench/json/fetch.sh for the pinned version).
// usage: ref_simdjson <batch file>
#include "simdjson.h"
#include <cstdio>
#include <fstream>
#include <string>

static void put_hex(std::string &o, std::string_view s) {
    static const char hx[] = "0123456789abcdef";
    for (unsigned char c : s) { o += hx[c >> 4]; o += hx[c & 15]; }
}

static void canon(std::string &o, simdjson::dom::element v) {
    using simdjson::dom::element_type;
    char buf[64];
    switch (v.type()) {
    case element_type::NULL_VALUE: o += "null"; break;
    case element_type::BOOL: o += bool(v) ? "true" : "false"; break;
    case element_type::INT64: snprintf(buf, sizeof buf, "i:%lld", (long long)int64_t(v)); o += buf; break;
    case element_type::UINT64: {
        unsigned long long u = uint64_t(v);
        snprintf(buf, sizeof buf, u <= 9223372036854775807ULL ? "i:%llu" : "u:%llu", u);
        o += buf;
        break;
    }
    case element_type::DOUBLE: snprintf(buf, sizeof buf, "f:%.17g", double(v)); o += buf; break;
    case element_type::BIGINT: o += "bigint"; break;
    case element_type::STRING: o += "s:"; put_hex(o, std::string_view(v)); break;
    case element_type::ARRAY: {
        o += '[';
        bool first = true;
        for (simdjson::dom::element x : simdjson::dom::array(v)) {
            if (!first) o += ',';
            first = false;
            canon(o, x);
        }
        o += ']';
        break;
    }
    case element_type::OBJECT: {
        o += '{';
        bool first = true;
        for (auto field : simdjson::dom::object(v)) {
            if (!first) o += ',';
            first = false;
            o += "s:";
            put_hex(o, field.key);
            o += ':';
            canon(o, field.value);
        }
        o += '}';
        break;
    }
    }
}

static int unhex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ref_simdjson <batch>\n"); return 2; }
    std::ifstream in(argv[1]);
    std::string line;
    simdjson::dom::parser parser;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::string doc;
        if (line != "-")
            for (size_t i = 0; i + 1 < line.size(); i += 2) doc += char(unhex(line[i]) * 16 + unhex(line[i + 1]));
        simdjson::dom::element root;
        auto err = parser.parse(simdjson::padded_string(doc)).get(root);
        if (err) {
            printf("ERR %s\n", simdjson::error_message(err));
            continue;
        }
        std::string o = "OK ";
        canon(o, root);
        puts(o.c_str());
    }
    return 0;
}

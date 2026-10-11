// bench_ref.cpp -- the reference libraries' side of bench/json: simdjson
// (DOM and On-Demand), yyjson and RapidJSON, with the same modes, timing and
// output format as bench_iron.iron (see README.md).
//
// usage: bench_ref <lib> <mode> <file> <warmup> <samples> [cold]
//   lib: simdjson_dom | simdjson_od | yyjson | rapidjson
// A mode a library has no meaningful form of prints "SKIP <lib> <mode>".
//
// Each sample includes freeing what it built, as the Iron side does (a Json
// tree is dropped at the end of the timed block). Inputs are read and, for
// simdjson, padded before timing.
#include "simdjson.h"
#include "yyjson.h"
#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/reader.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace simdjson;
static const unsigned RJ_FLAGS = rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag;

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── traversals: visit every value, fold into a checksum ──────────────────

static uint64_t walk_dom(dom::element e) {
    switch (e.type()) {
    case dom::element_type::ARRAY: {
        uint64_t a = 0;
        for (dom::element x : dom::array(e)) a += walk_dom(x);
        return a;
    }
    case dom::element_type::OBJECT: {
        uint64_t a = 0;
        for (auto f : dom::object(e)) a += f.key.size() + walk_dom(f.value);
        return a;
    }
    case dom::element_type::INT64: return (uint64_t)int64_t(e) & 1023;
    case dom::element_type::UINT64: return uint64_t(e) & 1023;
    case dom::element_type::DOUBLE: return (uint64_t)(int64_t)double(e) & 1023;
    case dom::element_type::STRING: return std::string_view(e).size();
    case dom::element_type::BOOL: return bool(e) ? 3 : 2;
    case dom::element_type::NULL_VALUE: return 1;
    default: return 0;
    }
}

static uint64_t walk_od(ondemand::value v) {
    switch (v.type().value_unsafe()) {
    case ondemand::json_type::array: {
        uint64_t a = 0;
        for (auto x : v.get_array()) a += walk_od(x.value_unsafe());
        return a;
    }
    case ondemand::json_type::object: {
        uint64_t a = 0;
        for (auto f : v.get_object()) {
            a += f.unescaped_key().value_unsafe().size();
            a += walk_od(f.value().value_unsafe());
        }
        return a;
    }
    case ondemand::json_type::number: {
        ondemand::number n = v.get_number().value_unsafe();
        if (n.is_int64()) return (uint64_t)n.get_int64() & 1023;
        if (n.is_uint64()) return n.get_uint64() & 1023;
        return (uint64_t)(int64_t)n.get_double() & 1023;
    }
    case ondemand::json_type::string: return v.get_string().value_unsafe().size();
    case ondemand::json_type::boolean: return v.get_bool().value_unsafe() ? 3 : 2;
    case ondemand::json_type::null: return v.is_null().value_unsafe() ? 1 : 0;
    default: return 0;
    }
}

static uint64_t walk_yy(yyjson_val *v) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_ARR: {
        uint64_t a = 0;
        size_t i, n;
        yyjson_val *x;
        yyjson_arr_foreach(v, i, n, x) a += walk_yy(x);
        return a;
    }
    case YYJSON_TYPE_OBJ: {
        uint64_t a = 0;
        size_t i, n;
        yyjson_val *k, *x;
        yyjson_obj_foreach(v, i, n, k, x) a += yyjson_get_len(k) + walk_yy(x);
        return a;
    }
    case YYJSON_TYPE_NUM:
        if (yyjson_is_real(v)) return (uint64_t)(int64_t)yyjson_get_real(v) & 1023;
        return yyjson_get_uint(v) & 1023;
    case YYJSON_TYPE_STR: return yyjson_get_len(v);
    case YYJSON_TYPE_BOOL: return yyjson_get_bool(v) ? 3 : 2;
    case YYJSON_TYPE_NULL: return 1;
    default: return 0;
    }
}

static uint64_t walk_rj(const rapidjson::Value &v) {
    if (v.IsArray()) {
        uint64_t a = 0;
        for (auto &x : v.GetArray()) a += walk_rj(x);
        return a;
    }
    if (v.IsObject()) {
        uint64_t a = 0;
        for (auto &m : v.GetObject()) a += m.name.GetStringLength() + walk_rj(m.value);
        return a;
    }
    if (v.IsInt64()) return (uint64_t)v.GetInt64() & 1023;
    if (v.IsUint64()) return v.GetUint64() & 1023;
    if (v.IsDouble()) return (uint64_t)(int64_t)v.GetDouble() & 1023;
    if (v.IsString()) return v.GetStringLength();
    if (v.IsBool()) return v.GetBool() ? 3 : 2;
    return 1;
}

// ── the benchmark ─────────────────────────────────────────────────────────

struct Ctx {
    std::string lib, mode;
    std::string text;
    padded_string padded;
    std::vector<std::string> docs;
    std::vector<padded_string> pdocs;
    dom::parser dp;
    ondemand::parser op;
    yyjson_doc *yy = nullptr;
    std::vector<yyjson_doc *> yydocs;
    rapidjson::Document rj;
    std::vector<rapidjson::Document> rjdocs;
};

// One sample of `mode` for `lib`; returns a checksum, or ~0 when the
// combination is not meaningful.
static uint64_t run_once(Ctx &c) {
    const std::string &L = c.lib, &M = c.mode;
    uint64_t check = 0;
    if (L == "simdjson_dom") {
        if (M == "parse" || M == "validate") {
            dom::element e;
            if (c.dp.parse(c.padded).get(e)) abort();
            return e.is_object() ? 1 : 2;
        }
        if (M == "traverse") {
            dom::element e;
            if (c.dp.parse(c.padded).get(e)) abort();
            return walk_dom(e);
        }
        if (M == "small_parse" || M == "small_validate") {
            for (auto &d : c.pdocs) {
                dom::element e;
                if (c.dp.parse(d).get(e)) abort();
                check += e.is_object();
            }
            return check;
        }
        if (M == "small_select") {
            for (auto &d : c.pdocs) {
                dom::element e;
                if (c.dp.parse(d).get(e)) abort();
                int64_t id = 0;
                std::string_view email;
                bool active = false;
                (void)e["id"].get(id);
                (void)e["email"].get(email);
                (void)e["active"].get(active);
                check += (uint64_t)(id & 1023) + email.size() + active;
            }
            return check;
        }
        return ~0ULL;
    }
    if (L == "simdjson_od") {
        if (M == "traverse") {
            ondemand::document doc = c.op.iterate(c.padded).value_unsafe();
            ondemand::value root = doc.get_value().value_unsafe();
            return walk_od(root);
        }
        if (M == "small_select") {
            for (auto &d : c.pdocs) {
                ondemand::document doc = c.op.iterate(d).value_unsafe();
                int64_t id = 0;
                std::string_view email;
                bool active = false;
                (void)doc["id"].get(id);
                (void)doc["email"].get(email);
                (void)doc["active"].get(active);
                check += (uint64_t)(id & 1023) + email.size() + active;
            }
            return check;
        }
        return ~0ULL;
    }
    if (L == "yyjson") {
        if (M == "parse" || M == "validate" || M == "traverse") {
            yyjson_doc *d = yyjson_read(c.text.data(), c.text.size(), 0);
            if (!d) abort();
            check = M == "traverse" ? walk_yy(yyjson_doc_get_root(d)) : yyjson_doc_get_val_count(d);
            yyjson_doc_free(d);
            return check;
        }
        if (M == "serialize") {
            size_t n = 0;
            char *s = yyjson_write(c.yy, 0, &n);
            free(s);
            return n;
        }
        if (M == "small_parse" || M == "small_validate" || M == "small_select") {
            for (auto &s : c.docs) {
                yyjson_doc *d = yyjson_read(s.data(), s.size(), 0);
                if (!d) abort();
                if (M == "small_select") {
                    yyjson_val *r = yyjson_doc_get_root(d);
                    check += (uint64_t)(yyjson_get_sint(yyjson_obj_get(r, "id")) & 1023);
                    check += yyjson_get_len(yyjson_obj_get(r, "email"));
                    check += yyjson_get_bool(yyjson_obj_get(r, "active"));
                } else {
                    check += yyjson_doc_get_val_count(d);
                }
                yyjson_doc_free(d);
            }
            return check;
        }
        if (M == "small_serialize") {
            for (auto *d : c.yydocs) {
                size_t n = 0;
                char *s = yyjson_write(d, 0, &n);
                free(s);
                check += n;
            }
            return check;
        }
        return ~0ULL;
    }
    if (L == "rapidjson") {
        if (M == "parse" || M == "traverse") {
            rapidjson::Document d;
            d.Parse<RJ_FLAGS>(c.text.data(), c.text.size());
            if (d.HasParseError()) abort();
            return M == "traverse" ? walk_rj(d) : d.IsObject();
        }
        if (M == "validate") {
            rapidjson::Reader reader;
            rapidjson::BaseReaderHandler<> h;
            rapidjson::MemoryStream ms(c.text.data(), c.text.size());
            if (!reader.Parse<RJ_FLAGS>(ms, h)) abort();
            return 1;
        }
        if (M == "serialize") {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> w(sb);
            c.rj.Accept(w);
            return sb.GetSize();
        }
        if (M == "small_parse" || M == "small_select") {
            for (auto &s : c.docs) {
                rapidjson::Document d;
                d.Parse<RJ_FLAGS>(s.data(), s.size());
                if (d.HasParseError()) abort();
                if (M == "small_select") {
                    check += (uint64_t)(d["id"].GetInt64() & 1023);
                    check += d["email"].GetStringLength();
                    check += d["active"].GetBool();
                } else {
                    check += d.MemberCount();
                }
            }
            return check;
        }
        if (M == "small_validate") {
            for (auto &s : c.docs) {
                rapidjson::Reader reader;
                rapidjson::BaseReaderHandler<> h;
                rapidjson::MemoryStream ms(s.data(), s.size());
                if (!reader.Parse<RJ_FLAGS>(ms, h)) abort();
                check++;
            }
            return check;
        }
        if (M == "small_serialize") {
            for (auto &d : c.rjdocs) {
                rapidjson::StringBuffer sb;
                rapidjson::Writer<rapidjson::StringBuffer> w(sb);
                d.Accept(w);
                check += sb.GetSize();
            }
            return check;
        }
        return ~0ULL;
    }
    return ~0ULL;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: bench_ref <lib> <mode> <file> <warmup> <samples> [cold]\n");
        return 2;
    }
    Ctx c;
    c.lib = argv[1];
    c.mode = argv[2];
    const char *path = argv[3];
    int warmup = atoi(argv[4]), samples = atoi(argv[5]);
    bool cold = argc > 6 && strcmp(argv[6], "cold") == 0;
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    c.text = ss.str();
    size_t bytes = c.text.size();
    bool small = c.mode.rfind("small_", 0) == 0;
    if (small) {
        bytes = 0;
        size_t i = 0;
        while (i < c.text.size()) {
            size_t j = c.text.find('\n', i);
            if (j == std::string::npos) j = c.text.size();
            if (j > i) {
                c.docs.emplace_back(c.text, i, j - i);
                bytes += j - i;
            }
            i = j + 1;
        }
        for (auto &d : c.docs) c.pdocs.emplace_back(d);
        if (c.mode == "small_serialize") {
            for (auto &d : c.docs) c.yydocs.push_back(yyjson_read(d.data(), d.size(), 0));
            c.rjdocs.resize(c.docs.size());
            for (size_t k = 0; k < c.docs.size(); k++) c.rjdocs[k].Parse<RJ_FLAGS>(c.docs[k].data(), c.docs[k].size());
        }
    } else {
        c.padded = padded_string(c.text);
        c.yy = yyjson_read(c.text.data(), c.text.size(), 0);
        c.rj.Parse<RJ_FLAGS>(c.text.data(), c.text.size());
    }
    // simdjson DOM serialize: the document lives in the parser, so parse once
    // and time minify() alone.
    dom::element ser_root;
    bool dom_ser = c.lib == "simdjson_dom" && c.mode == "serialize";
    if (dom_ser && c.dp.parse(c.padded).get(ser_root)) abort();
    if (c.lib == "simdjson_dom" && c.mode == "small_serialize") {
        printf("SKIP %s %s\n", c.lib.c_str(), c.mode.c_str());
        return 0;
    }
    std::vector<uint64_t> flushbuf;
    if (cold) flushbuf.assign(16777216, 1);
    uint64_t check = 0;
    std::string times;
    for (int it = 0; it < warmup + samples; it++) {
        if (cold) {
            uint64_t s = 0;
            for (size_t k = 0; k < flushbuf.size(); k += 8) s += flushbuf[k];
            check += s & 1;
        }
        uint64_t t0 = now_ns();
        uint64_t r;
        if (dom_ser) r = simdjson::minify(ser_root).size();
        else r = run_once(c);
        uint64_t dt = now_ns() - t0;
        if (r == ~0ULL) {
            printf("SKIP %s %s\n", c.lib.c_str(), c.mode.c_str());
            return 0;
        }
        check += r;
        if (it >= warmup) {
            if (!times.empty()) times += ',';
            times += std::to_string(dt);
        }
    }
    const char *base = strrchr(path, '/');
    printf("RESULT %s %s %s %zu %s\n", c.lib.c_str(), c.mode.c_str(), base ? base + 1 : path, bytes, times.c_str());
    printf("CHECK %llu\n", (unsigned long long)check);
    return 0;
}

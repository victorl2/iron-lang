# JSON

`import json` gives Iron a JSON value type, a strict parser, a serializer
and a streaming writer. This guide covers the whole module; the
declarations are in `src/stdlib/json.iron` and the scanner and writer in
`src/stdlib/iron_json.c`. Every example below is compiled and run by
`scripts/test_doc_examples.sh`.

<!-- doctest-imports: json -->

```iron
import json

func main() {
    val r = Json.parse("""\{"user": \{"id": 42, "name": "Ann"}, "tags": ["a", "b"]}""")
    if r.error != 0 {
        println("bad JSON: {r.error_message}")
        return
    }
    val doc = r.value
    println("{doc.get("user").get_string("name")} {doc.get("user").get_int("id")}")
    doc.get("tags").push(Json.str("c"))
    doc.set("ok", Json.bool(true))
    println(doc.to_string())
}
```

```output
Ann 42
{"user":{"id":42,"name":"Ann"},"tags":["a","b","c"],"ok":true}
```

A `{` in an Iron string starts an interpolation, so JSON written as an Iron
literal escapes its opening braces: `"\{\"a\": 1}"`. Text read from a file
or an HTTP body needs nothing.

## Parsing

`Json.parse(text)` returns a `JsonResult`:

| Field | Meaning |
|---|---|
| `value: Json` | the document (a JSON null when parsing failed) |
| `error: Int` | 0 on success, otherwise one of the codes below |
| `error_message: String` | what is wrong and where: `trailing comma before closing bracket at line 1, column 6` |
| `line`, `column: Int` | 1-based position of the problem (the column counts characters) |
| `offset: Int` | 0-based byte offset of the problem |

`r.ok()` is `r.error == 0`. The parser implements RFC 8259 strictly: it
rejects trailing commas, leading zeros, comments, single quotes, `NaN` and
`Infinity`, unescaped control characters, invalid escapes, `\u` escapes
that do not pair surrogates, ill-formed UTF-8 (overlong forms, encoded
surrogates, code points above U+10FFFF), a byte order mark, numbers too
large for a 64-bit float (`1e400`) and anything after the top-level value.
Any value may be the top-level value.

`Json.parse_with(text, JsonLimits(max_depth, max_bytes))` bounds untrusted
input: nesting deeper than `max_depth` arrays and objects and input longer
than `max_bytes` (`-1` for no limit) are errors. `Json.parse` uses
`Json.default_limits()`, depth 512 and 64 MiB. A depth limit above 4096 is
treated as 4096: dropping a tree uses the call stack once per level, and
4096 levels stay well inside the smallest stack an Iron program runs on.
Everything else (serializing, `equals`, `clone`) walks a tree of any depth
without recursion, including trees built in code.

```iron
func main() {
    for text in ["[1, 2,]", "\{\"a\" 1}", "[01]", "\"\\ud800\"", "\{\"k\":\n  [1 2]}"] {
        val r = Json.parse(text)
        println("{r.error} {r.line}:{r.column} {r.error_message}")
    }
    val deep = Json.parse_with("[[[[1]]]]", JsonLimits(3, 1024))
    println("{deep.error} {deep.error_message}")
}
```

```output
4000 1:6 trailing comma before closing bracket at line 1, column 6
4000 1:6 expected ':' after object key, found '1' at line 1, column 6
4005 1:2 leading zeros are not allowed at line 1, column 2
4004 1:2 lone high surrogate (\uD800-\uDBFF must be followed by a low surrogate) at line 1, column 2
4000 2:6 expected ',' or ']', found '2' at line 2, column 6
4001 nesting is deeper than the depth limit at line 1, column 4
```

| Code | Name (`iron_errors.h`) | Cause |
|---|---|---|
| 4000 | `IRON_ERR_JSON_SYNTAX` | malformed JSON |
| 4001 | `IRON_ERR_JSON_DEPTH` | nesting deeper than the limit |
| 4002 | `IRON_ERR_JSON_TOO_LARGE` | input larger than the limit |
| 4003 | `IRON_ERR_JSON_BAD_UTF8` | ill-formed UTF-8 |
| 4004 | `IRON_ERR_JSON_BAD_STRING` | bad escape, control character, lone surrogate |
| 4005 | `IRON_ERR_JSON_BAD_NUMBER` | malformed or out-of-range number |

## Values

A `Json` holds one `JsonNode`:

```iron
-- (declared in json.iron)
-- enum JsonNode {
--     Null, Bool(Bool), Int(Int), Float(Float), Str(String),
--     Arr(rc [Json]), Obj(rc JsonObject),
-- }
func main() {
    val v = Json.parse("[1, 2.5, \"three\", null]").value
    for item in v.items() {
        match item.node {
            JsonNode.Int(i) -> println("int {i}")
            JsonNode.Float(f) -> println("float {f}")
            JsonNode.Str(s) -> println("string {s}")
            else -> println(item.type_name())
        }
    }
}
```

```output
int 1
float 2.5
string three
null
```

- A number without a fraction or exponent that fits in 64 bits is an `Int`
  and keeps its exact value (`9223372036854775807` survives a round trip;
  `-0` is the Int `0`). Every other number, including a larger integer, is
  the nearest `Float` (correctly rounded; `-0.0` keeps its sign, and a
  number too small for a double, such as `1e-400`, is `0.0` with its sign).
- Strings are stored decoded, as UTF-8 Iron strings.
- An object keeps its members in the order they were written or added.
  Parsed objects keep duplicate keys as written; lookups return the last
  one, as `JSON.parse` and Python's `json` do.
- Arrays and objects live behind `rc`: copying a `Json` is cheap and the
  copy refers to the same container, so a change made through one copy is
  visible through every other. `v.clone()` makes an independent deep copy.

`v.kind()` returns a `JsonKind` (`Null`, `Bool`, `Int`, `Float`, `String`,
`Array`, `Object`) and `v.type_name()` the same as text; `is_null()`,
`is_bool()`, `is_int()`, `is_float()`, `is_number()`, `is_string()`,
`is_array()` and `is_object()` test one kind.

## Reading values

| Method | Result |
|---|---|
| `as_bool()`, `as_int()`, `as_string()` | the value, or `null` when it has another type |
| `as_float()` | a Float, or an Int converted to Float, or `null` |
| `bool_or(d)`, `int_or(d)`, `float_or(d)`, `string_or(d)` | the value, or `d` |
| `get(key)` | member `key` of an object, or a JSON null (so lookups chain) |
| `find(key)` | member `key`, or `null` (`Json?`) |
| `has(key)` | whether this is an object with member `key` |
| `at(i)` | element `i` of an array (negative counts from the end), or a JSON null |
| `get_string(k)`, `get_int(k)`, `get_float(k)`, `get_bool(k)` | `get(k)` then `as_...()` |
| `len()` | elements of an array, members of an object, 0 otherwise |
| `keys()`, `items()` | the keys of an object; the elements of an array or the values of an object |
| `key_at(i)`, `value_at(i)` | member `i` of an object by position |
| `pointer(p)` | RFC 6901 JSON Pointer (`"/users/0/name"`, `~1` for `/`, `~0` for `~`), or `null` |
| `path(p)` | dotted path (`"users.0.name"`), or `null` |
| `equals(other)` | deep equality; members in any order (the last of duplicate keys counts), an Int equals a Float of exactly the same value |

```iron
func main() {
    val doc = Json.parse("""\{"users": [\{"name": "Ann", "age": 31}, \{"name": "Bo"}], "a/b": 1}""").value
    val first = doc.pointer("/users/0/name")
    val slash = doc.pointer("/a~1b")
    if first != null and slash != null {
        println("{first.as_string()} {slash.as_int()}")
    }
    val bo = doc.get("users").at(-1)
    println("{bo.get_string("name")} {bo.get_int("age")} {bo.get("age").int_or(0)} {doc.path("users.5") == null}")
}
```

```output
Ann 1
Bo null 0 true
```

## Building and changing values

`Json.null()`, `Json.bool(b)`, `Json.int(i)`, `Json.float(f)`,
`Json.str(s)`, `Json.array()`, `Json.obj()`, and `Json.array_of(items)`,
`Json.strings(items)`, `Json.ints(items)` from Iron lists build values.
`set(key, v)` replaces a member in place or appends it, `push(v)`,
`insert(i, v)` and `set_at(i, v)` change arrays, `remove(key)` and
`remove_at(i)` take things out and `clear()` empties a container. The
mutators work on a `val` because they change the shared container, not the
`Json` value; calling one on the wrong kind (`push` on an object) is a
programming error and panics.

```iron
func main() {
    val order = Json.obj()
    order.set("id", Json.int(7))
    order.set("items", Json.strings(["tea", "jam"]))
    val items = order.get("items")
    items.push(Json.str("bread"))
    val snapshot = order.clone()
    order.set("id", Json.int(8))
    println(order.to_string())
    println(snapshot.to_string())
}
```

```output
{"id":8,"items":["tea","jam","bread"]}
{"id":7,"items":["tea","jam","bread"]}
```

## Serializing

`to_string()` gives compact JSON (it is also what `"{v}"` interpolates),
`pretty()` indents by two spaces, and `format(indent, sort_keys)` chooses
the indentation (0 for compact) and whether object keys are sorted;
otherwise members keep their order, so output is deterministic either way.

Floats are written in their shortest round-trip form (the digits Python's
`repr` and JavaScript print, from the Ryu algorithm), with a `.0` on whole
values so they read back as floats and an exponent outside `1e-7 .. 1e21`.
JSON has no NaN or infinity: those are written as `null`. Strings are
escaped as JSON requires (`"`, `\`, control characters); bytes that are not
well-formed UTF-8 (an Iron string can hold any bytes) become U+FFFD so the
output is always valid JSON.

```iron
func main() {
    val v = Json.parse("""\{"b": [1.0, 0.1, 1e21, 2.5e-8], "a": "tab\\there"}""").value
    println(v.to_string())
    println(v.format(0, true))
    println(v.pretty())
}
```

```output
{"b":[1.0,0.1,1e+21,2.5e-8],"a":"tab\there"}
{"a":"tab\there","b":[1.0,0.1,1e+21,2.5e-8]}
{
  "b": [
    1.0,
    0.1,
    1e+21,
    2.5e-8
  ],
  "a": "tab\there"
}
```

## Streaming output: JsonWriter

`JsonWriter.compact()` and `JsonWriter.pretty(indent)` write JSON straight
into a native buffer: no tree and no intermediate strings, which makes it
the fast way to produce responses. Commas, escaping and indentation are
inserted for you; `finish()` returns the text and empties the writer for
reuse. Misuse (a value inside an object without a key, an unbalanced end,
`finish()` with something still open) panics.

| Method | Writes |
|---|---|
| `begin_object()`, `end_object()`, `begin_array()`, `end_array()` | containers |
| `key(k)` | the key of the next member |
| `string(s)`, `int(i)`, `float(f)`, `bool(b)`, `null()` | one value |
| `field_string(k, s)`, `field_int(k, i)`, `field_float(k, f)`, `field_bool(k, b)`, `field_null(k)` | `key` and value in one call |
| `value(v)`, `field_value(k, v)` | a whole `Json` tree |
| `raw(text)` | already-encoded JSON, inserted unchecked |
| `len()`, `finish()`, `reset()` | bytes so far; the text; discard |

```iron
object Product {
    val id: Int
    val name: String
    val price: Float
    val tags: [String]
}

func main() {
    var products: [Product] = []
    products.push(Product(1, "Tea", 4.5, ["drink"]))
    products.push(Product(2, "Jam \"extra\"", 3.0, []))
    val w = JsonWriter.compact()
    w.begin_object()
    w.field_int("count", products.len())
    w.key("products")
    w.begin_array()
    for p in products {
        w.begin_object()
        w.field_int("id", p.id)
        w.field_string("name", p.name)
        w.field_float("price", p.price)
        w.key("tags")
        w.begin_array()
        for t in p.tags {
            w.string(t)
        }
        w.end_array()
        w.end_object()
    }
    w.end_array()
    w.end_object()
    println(w.finish())
}
```

```output
{"count":2,"products":[{"id":1,"name":"Tea","price":4.5,"tags":["drink"]},{"id":2,"name":"Jam \"extra\"","price":3.0,"tags":[]}]}
```

With the `http` module, `Http.json_response(200, w.finish())` sends it.

## Streaming input: JsonReader

`JsonReader(text)` (or `JsonReader.with_limits(text, limits)`) parses the
text and lets you walk it value by value, copying what you need into your
own objects without building a tree. It has the same `error`,
`error_message`, `line` and `column` fields as a parse result. Reading
moves the reader, so it is a `var`.

| Method | Effect |
|---|---|
| `peek()` | the `JsonKind` of the next value |
| `begin_object()` / `next_key()` | enter an object; the next key, or `null` at its end (which leaves it) |
| `begin_array()` / `next_item()` | enter an array; whether another element follows (false at its end, which leaves it) |
| `int()`, `float()`, `string()`, `bool()` | the next value, or `null` (and the value is skipped) when it has another type |
| `int_or(d)`, `float_or(d)`, `string_or(d)`, `bool_or(d)` | the same with a fallback |
| `null()` | consume a null; false (consuming nothing) otherwise |
| `skip()` | skip the next value with everything inside it |
| `value()` | the next value as a `Json` tree |

```iron
object Line {
    val sku: String
    val qty: Int
}

func read_lines(text: String) -> [Line] {
    var out: [Line] = []
    var r = JsonReader(text)
    if r.error != 0 or not r.begin_object() {
        return out
    }
    var key = r.next_key()
    while key != null {
        if key == "lines" and r.begin_array() {
            while r.next_item() {
                var sku = ""
                var qty = 0
                if r.begin_object() {
                    var k = r.next_key()
                    while k != null {
                        if k == "sku" {
                            sku = r.string_or("")
                        } elif k == "qty" {
                            qty = r.int_or(0)
                        } else {
                            r.skip()
                        }
                        k = r.next_key()
                    }
                }
                out.push(Line(sku, qty))
            }
        } else {
            r.skip()
        }
        key = r.next_key()
    }
    return out
}

func main() {
    val lines = read_lines("""\{"id": 9, "lines": [\{"sku": "A", "qty": 2, "note": \{"x": [1]}}, \{"qty": 5, "sku": "B"}]}""")
    for l in lines {
        println("{l.sku} x{l.qty}")
    }
}
```

```output
A x2
B x5
```

## Your own types

Iron has no reflection and `comptime` cannot inspect types, so there is no
automatic mapping between JSON and objects
([#455](https://github.com/victorl2/iron-lang/issues/455)). What the module
offers instead:

- `interface JsonEncodable { readonly func write_json(w: JsonWriter) }`.
  Implement it once; `Json.encode(v)` and `Json.encode_list(xs)` (generic
  over `[T: JsonEncodable]`) produce text, and one type's `write_json` can
  call another's.
- `Json.decode_list(v, f) -> [T]?` decodes every element of an array with a
  function `func(Json) -> T` (null when `v` is not an array).
- The earlier forms `json_encode_list(xs)` and `json_decode_list(v, f, out)
  -> Bool` (appending to `out`) remain.
- For speed, decode with `JsonReader` as above and encode with
  `JsonWriter`: neither builds a tree.

```iron
object Point impl JsonEncodable {
    val x: Int
    val y: Int
    readonly func write_json(w: JsonWriter) {
        w.begin_object()
        w.field_int("x", self.x)
        w.field_int("y", self.y)
        w.end_object()
    }
}

func point_from(j: Json) -> Point {
    return Point(j.get("x").int_or(0), j.get("y").int_or(0))
}

func main() {
    val pts = [Point(1, 2), Point(3, 4)]
    val text = Json.encode_list(pts)
    println(text)
    val back = Json.decode_list(Json.parse(text).value, point_from)
    if back != null {
        println("{back.len()} {back[1].x} {Json.encode(back[0])}")
    }
}
```

```output
[{"x":1,"y":2},{"x":3,"y":4}]
2 3 {"x":1,"y":2}
```

## Conformance

`tests/json/conformance` checks the module against RFC 8259 and ECMA-404:
JSONTestSuite, the JSON_checker files, hard cases for parsing and printing
doubles (halfway points, subnormals, the int64 and double limits, huge
exponents), every escape and UTF-8 byte sequence class, structure, nesting
and size limits, against an independent model of the behaviour described
here. `tests/json/fuzz` compares it with Python's `json` and yyjson (and
simdjson when available) on random and mutated documents. Where the RFC
leaves a choice (JSONTestSuite's `i_` cases), the module:

| Input | Result |
|---|---|
| integer beyond 64 bits (`-123123123123123123123123123123`) | accepted as the nearest Float |
| number that rounds to infinity (`1e400`, `-1e9999`) | rejected (4005) |
| number that underflows (`123e-10000000`) | accepted as `0.0` (sign kept) |
| ill-formed UTF-8, overlong forms, encoded surrogates, above U+10FFFF | rejected (4003) |
| `\u` escape with an unpaired surrogate (`"\uD800"`, `"\uDC00\uD800"`) | rejected (4004) |
| UTF-8 byte order mark before the value | rejected (4000) |
| UTF-16 or UTF-32 text | rejected |
| 500 nested arrays | accepted (default depth limit 512) |
| noncharacters (U+FFFF, U+FDD0, U+10FFFE), raw or escaped | accepted |
| duplicate keys | accepted, kept in order; lookups use the last |

## Performance

The scanner validates and decodes in one pass in C (strings eight bytes at
a time, integers accumulated directly, floats through an exact fast path
before `strtod`) and hands Iron a flat tape; `Json.parse` builds the tree
from it, wrapping each array and object in its `rc` cell once. Strings of
up to 23 bytes are stored inline in the value, so most keys cost no
allocation. Serialization writes into one growing buffer. The numbers in
the pull request that added the module compare `Json.parse`,
`to_string()`, Python's `json` and yyjson on the same machine.

Lookups by key scan an object's members (from the end, so the last
duplicate wins): fine for the objects APIs return, linear for an object
with thousands of keys (`equals` hashes the keys, so comparing large
objects stays linear). For a large map, copy it into a `Map[String, Json]`
once.

"""Compile-time field reflection programs.

Random object shapes exercise interface-default expansion, declaration-order
metadata, documentation and `//fuzz:` directive metadata (fields and objects
that carry either are left out of the hash), generic fields(T), direct
reflected reads, Fields[T] writes and missing-field tracking. The model computes the same hashes and text without
using the compiler.
"""
from pf_probe import Probe, ProbeGen


PROLOGUE = '''interface FuzzReflect {
    readonly func reflected_hash() -> Int {
        var hash = 17
        comptime if typeinfo(Self).tag("fuzz") == null and typeinfo(Self).doc == null {
            comptime for f in fields(Self) {
                comptime if f.tag("fuzz") == null and f.doc == null {
                    comptime match f.type {
                        Int -> { hash = hash * 31 + self.(f) }
                        else -> comptime error("fuzz reflection expects Int")
                    }
                }
            }
        }
        return hash
    }

    static func seeded(seed: Int) -> Self {
        var b = Fields[Self]()
        comptime for f in fields(Self) {
            b.(f) = seed + f.index
        }
        return b.build()
    }

    static func first_missing(seed: Int) -> String? {
        var b = Fields[Self]()
        comptime for f in fields(Self) {
            comptime if f.index != 0 {
                b.(f) = seed + f.index
            }
        }
        return b.missing()
    }
}

func reflected_names[T]() -> String {
    var names = ""
    comptime for f in fields(T) {
        names = names + f.name + ";"
    }
    return names
}

func reflected_public_count[T]() -> Int {
    var count = 0
    comptime for f in fields(T) {
        comptime if f.is_pub {
            count = count + 1
        }
    }
    return count
}

func reflected_kind[T]() -> String {
    comptime match T {
        Int -> { return "int" }
        [E] -> { return "list:" + reflected_kind[E]() }
        Map[K, V] -> { return "map:" + reflected_kind[K]() + ":" + reflected_kind[V]() }
        E? -> { return "nullable:" + reflected_kind[E]() }
        FuzzReflect -> { return "reflected" }
        else -> { return "other" }
    }
}'''


def hash_values(values):
    result = 17
    for value in values:
        result = result * 31 + value
    return result


class ReflectGen(ProbeGen):
    NPROBES = (3, 7)
    panic_rate = 0.0
    error_rate = 0.0

    def reflection_object(self):
        r = self.r
        ident = self.fresh('Reflect')
        count = r.randint(1, 5)
        names = [f'f{i}' for i in range(count)]
        public = [r.choice((False, True)) for _ in names]
        values = [r.randint(-20, 20) for _ in names]
        seed = r.randint(-10, 10)
        kind_spelling, kind_result = r.choice((
            ('Int', 'int'),
            ('[Int]', 'list:int'),
            ('Map[String, Int]', 'map:other:int'),
            ('Int?', 'nullable:int'),
            (ident, 'reflected'),
        ))

        # metadata per field: none, a doc comment, an own-line directive,
        # a trailing directive, or a directive of another key (ignored)
        meta = [r.choice(('none', 'none', 'doc', 'tag', 'trailing', 'other'))
                for _ in names]
        lines = []
        for name, is_pub, m in zip(names, public, meta):
            if m == 'doc':
                lines.append(f'    // The {name} field.')
            elif m == 'tag':
                lines.append('    //fuzz:skip')
            elif m == 'other':
                lines.append('    //other:x')
            decl = f'    {"pub " if is_pub else ""}val {name}: Int'
            if m == 'trailing':
                decl += '  //fuzz:skip'
            lines.append(decl)
        fields = '\n'.join(lines)
        object_meta = r.choice(('', '', '', '// Documented.\n', '//fuzz:off\n'))
        self.need(ident, f'''{object_meta}object {ident} impl FuzzReflect {{
{fields}
}}''')

        def hashed(vals):
            if object_meta:
                return 17
            return hash_values([v for v, m in zip(vals, meta)
                                if m in ('none', 'other')])

        original_hash = hashed(values)
        seeded_hash = hashed([seed + i for i in range(count)])
        names_text = ''.join(name + ';' for name in names)
        public_count = sum(public)
        args = ', '.join(str(v) for v in values)
        out = (f'{original_hash}|{names_text}|{public_count}|{seeded_hash}|'
               f'{names[0]}|{kind_result}')
        code = [
            f'val value = {ident}({args})',
            f'val built = {ident}.seeded({seed})',
            f'val missing = {ident}.first_missing({seed})',
            f'println("{{value.reflected_hash()}}|{{reflected_names[{ident}]()}}|'
            f'{{reflected_public_count[{ident}]()}}|{{built.reflected_hash()}}|{{missing}}|'
            f'{{reflected_kind[{kind_spelling}]()}}")',
        ]
        return Probe(code, out=[out])

    def probe_kinds(self):
        return [(1, self.reflection_object)]

    def program(self):
        self.need('reflection_prologue', PROLOGUE)
        return super().program()


def generate(rng):
    gen = ReflectGen(rng)
    gen.program()
    return gen

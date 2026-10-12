"""Compile-time field reflection programs.

Random object shapes exercise interface-default expansion, declaration-order
metadata, generic fields(T), direct reflected reads, Fields[T] writes and
missing-field tracking. The model computes the same hashes and text without
using the compiler.
"""
from pf_probe import Probe, ProbeGen


PROLOGUE = '''interface FuzzReflect {
    readonly func reflected_hash() -> Int {
        var hash = 17
        comptime for f in fields(Self) {
            comptime if f.tag("fuzz") == null and f.doc == null {
                comptime match f.type {
                    Int -> { hash = hash * 31 + self.(f) }
                    else -> comptime error("fuzz reflection expects Int")
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

        fields = '\n'.join(
            f'    {"pub " if is_pub else ""}val {name}: Int'
            for name, is_pub in zip(names, public))
        self.need(ident, f'''object {ident} impl FuzzReflect {{
{fields}
}}''')

        original_hash = hash_values(values)
        seeded_hash = hash_values([seed + i for i in range(count)])
        names_text = ''.join(name + ';' for name in names)
        public_count = sum(public)
        args = ', '.join(str(v) for v in values)
        out = f'{original_hash}|{names_text}|{public_count}|{seeded_hash}|{names[0]}'
        code = [
            f'val value = {ident}({args})',
            f'val built = {ident}.seeded({seed})',
            f'val missing = {ident}.first_missing({seed})',
            f'println("{{value.reflected_hash()}}|{{reflected_names[{ident}]()}}|'
            f'{{reflected_public_count[{ident}]()}}|{{built.reflected_hash()}}|{{missing}}")',
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

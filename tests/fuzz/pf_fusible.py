"""Random list pipelines checked hinted, hint-free, and without optimization.

Each program mixes Int, String, and trivial object sources, pure named
callbacks with and without //iron:fusible, map and filter stages, and copy
boundaries. A small Python model predicts the result. The source marker tells
prog_fuzz how many fused loops the generated C must contain.
"""
from pf_lang import Block, Expected


class FusibleGen:
    def __init__(self, rng):
        self.r = rng
        self.funcs = []
        self.mainblock = Block([])
        self.decls = []
        self.lines = []
        self.out = []
        self.loops = 0

    def program(self):
        self.decls += [
            'object Elem {',
            '    val n: Int',
            '}',
            '',
        ]
        for case in range(self.r.randint(3, 7)):
            self.pipeline(case)

    def helper(self, name, params, ret, body, force_hint=False):
        if force_hint or self.r.random() < 0.55:
            self.decls.append('//iron:fusible')
        self.decls += [f'func {name}({params}) -> {ret} {{', f'    return {body}', '}', '']

    def pipeline(self, case):
        count = self.r.randint(1, 7)
        vals = [self.r.randint(-8, 12) for _ in range(count)]
        kind = self.r.choice(('int', 'string', 'object'))
        first = f'to_int_{case}'
        add = self.r.randint(-3, 6)
        if kind == 'int':
            source = '[' + ', '.join(str(x) for x in vals) + ']'
            self.helper(first, 'x: Int', 'Int', f'x + {add}', force_hint=case == 0)
            model = [x + add for x in vals]
            first_safe = True
        elif kind == 'object':
            source = '[' + ', '.join(f'Elem({x})' for x in vals) + ']'
            self.helper(first, 'x: Elem', 'Int', f'x.n + {add}', force_hint=case == 0)
            model = [x + add for x in vals]
            first_safe = True
        else:
            source = '[' + ', '.join(f'"s{x}"' for x in vals) + ']'
            self.helper(first, 'x: String', 'Int', f'x.len() + {add}', force_hint=case == 0)
            model = [len(f's{x}') + add for x in vals]
            first_safe = False

        expr = f'{source}.map({first})'
        candidates = [first_safe]
        for stage in range(self.r.randint(1, 4)):
            if self.r.random() < 0.55:
                amount = self.r.randint(-4, 5)
                name = f'map_{case}_{stage}'
                self.helper(name, 'x: Int', 'Int', f'x + {amount}')
                expr += f'.map({name})'
                model = [x + amount for x in model]
                candidates.append(True)
            else:
                limit = self.r.randint(-4, 10)
                name = f'keep_{case}_{stage}'
                self.helper(name, 'x: Int', 'Bool', f'x >= {limit}')
                expr += f'.filter({name})'
                model = [x for x in model if x >= limit]
                candidates.append(True)
            if self.r.random() < 0.22:
                expr += '.copy()'
                self.count_segments(candidates)
                candidates = []
        expr += '.sum()'
        candidates.append(True)
        self.count_segments(candidates)
        self.lines.append(f'    println("{{{expr}}}")')
        self.out.append(str(sum(model)))

    def count_segments(self, candidates):
        run = 0
        for candidate in candidates + [False]:
            if candidate:
                run += 1
            else:
                if run >= 2:
                    self.loops += 1
                run = 0

    def render(self):
        return '\n'.join([f'// @fusion-loops: {self.loops}', ''] + self.decls +
                         ['func main() {'] + self.lines + ['}', ''])

    def model(self):
        return Expected(self.out)


def generate(rng):
    g = FusibleGen(rng)
    g.program()
    return g

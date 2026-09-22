# Mutation through interface bindings

Date: 2026-09-22. Branch: `fix/method-receiver-by-reference`.

## Problem

A `var` binding of interface type did not behave like a `var` binding of the
concrete type. `var g: Game = MyGame(); g.update(dt)` mutated a copy, and
`func run(var game: Game)` called with a concrete `var my = MyGame()` passed
`&my` where an `Iron_Game *` was expected. Interface values are tagged unions
holding the implementor inline (or behind a malloc'd pointer for large
payloads); the generated dispatchers took `self` by value, so every write
landed in a stack copy.

The same receiver-copy bug existed for concrete receivers reached through a
`var` parameter, a lambda capture, an rc/heap binding, or a field of `self`
(`self.ball.move()` inside a method). Those are fixed on this branch too.

## Rule

A `var` binding of interface type behaves like a `var` binding of the concrete
type: mutating methods write through, no matter how the value arrived.

## Design

1. **Inline union stays.** The static-dispatch spec chose inline storage for
   cache locality and split collections; nothing here needs to give that up.
2. **Mutating dispatchers take a pointer.** For every interface method that is
   not `readonly`/`pure`, the generated dispatcher is
   `Iron_<iface>_<m>(Iron_<Iface> *self, ...)` and each case forwards
   `&self->data.<T>` (or `self->data.<T>` for indirect variants). Readonly
   dispatchers keep the by-value signature so `val` bindings, temporaries and
   rvalues are unaffected.
3. **Receivers pass their storage, never a copy.** For a pointer-receiver
   callee the HIR->LIR lowering passes the binding's alloca (local var,
   honored var param, var capture); emit_c renders the address of that slot,
   of a capture env pointer, of a module global, of an rc/heap pointee, or of
   a field chain rooted at any of those (`&(_v1->ball)`). Copy propagation and
   store-to-load forwarding treat the root slot of such a receiver as mutated
   by the call.
4. **Parameter boundary: wrap on entry, unwrap on exit.** When a concrete
   `var` binding is passed to a `var <Interface>` parameter the call site
   builds a temporary union, passes its address, and after the call copies
   the payload back into the source binding. This is the contract Iron `var`
   parameters already have (copy-in at entry, write-back at return), so
   aliasing behavior is identical to every other `var` param. The write-back
   checks the tag; a callee that rebound the parameter to another implementor
   panics rather than reading garbage. A `var <Interface>` source is passed by
   address directly.
5. **Default bodies are monomorphised.** An interface method with a body is
   cloned into every implementor that does not define it, before typecheck,
   keeping the interface's tier. Downstream passes see an ordinary method.

## Rejected

- A separate reference handle (`Iron_<Iface>_Ref { tag; void *ptr }`) for
  `var` interface params. True aliasing during the call, but it makes those
  params the only live-pointer `var` params in the language and doubles the
  representation. Revisit if `var` params ever become real pointers.
- Sharing one default-body function per interface that dispatches back
  through the union. More machinery, no benefit over cloning.

## Known gaps kept

- Passing a `var` field (`run(state.game)`) to a `var` parameter still lowers
  by value, as it already did for concrete types.
- Interface-typed arrays (`[Game]`) go through split collections and keep
  by-value element access.

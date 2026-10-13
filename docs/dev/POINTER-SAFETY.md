# Pointer safety: invariants

What the compiler and the runtime guarantee for checked pointers (`*T`,
`*var T`, `?*T`; manual 6.4), and why each part of the implementation
keeps it. `POINTER-LAYOUT.md` and `POINTER-CHECK-ELISION.md` describe the
older header-based checks and the elision pass in more detail; where they
disagree with this file, this file is right.

## The guarantee

1. **No dead storage is read or written through a checked pointer.** Every
   dereference of a pointer whose storage has ended aborts with the message
   of its kind before the storage is touched, in every build mode
   (default, `--release`, `--no-optimize`, `--debug`).
2. **A pointer whose storage is alive never panics.**
3. **The check reads only memory that is never freed**, so it cannot itself
   fault or race with the allocator.

Storage and its end:

| Storage | Ends when |
|---|---|
| a local | it is destroyed: at the end of its block, after the block's defers, in reverse order of declaration |
| a `var` captured by a closure (a cell) | the last share of the cell is released |
| a global | never |
| a heap value | `free` |
| an arena value | `reset`, a `restore` to a save point taken before it, the arena's end |
| an rc object | the last strong handle is released |
| a list element borrowed by a call | the call returns |

Data races are outside the guarantee: a program that ends storage on one
thread while another dereferences it is not guaranteed to be caught.

## I1. A pointer's generation alone decides validity

A checked pointer is `{addr, gen}`. `gen` names the storage the pointer was
taken from, and `iron_check_ptr(gen)` answers "is that storage alive" from
runtime tables only. Nothing depends on what produced the pointer (a local
`&`, a field, a list element, a parameter, a call result, a capture), so a
pointer can be copied anywhere without losing its check.

- Stack form (bit 63): `depth | thread number | id`. Live iff the scope
  table of that thread number holds exactly this key (thread number and id)
  at that depth.
- Slot form (bit 62): `arena bit | slot | sequence`. Live iff the
  generation table entry of the slot holds exactly this value.
- `gen == 0` (a null `?*T`) is never live.

## I2. A generation never becomes live again

- A slot's entry changes when its storage ends and the next owner of the
  slot gets the next sequence number; a slot whose sequence is used up is
  retired. Table chunks are never freed.
- A scope key is `(thread number, id)`. Ids of a thread number only grow,
  across the threads that reuse the number. A thread that uses up the 2^35
  ids of its number takes a fresh number and the old one is retired. So no
  key is written to a scope table twice in the life of the process, and
  leaving a scope clears its table entry.
- Arena segments (the allocations between two save points) own a slot each;
  `restore` ends every segment opened since the save point and later
  allocations open new ones.

Consequence: "the table holds this gen" is equivalent to "the storage this
pointer was taken from has not ended".

## I3. `&place` takes the generation of the storage that holds the place

`emit_place_gen` / `emit_container_gen` (emit_c.c):

| Place | Generation |
|---|---|
| a local | the scope that owns it (I4) |
| a captured `var` | the cell's (`iron_cell_ptr_gen`), in the declaring frame and in the lambda |
| a global | static (depth 0, always live) |
| a field or inline element of a value | that of the value's own place |
| reached through a checked pointer, a heap or arena handle | the pointer's or handle's own gen |
| a field of an rc object | the object's (`iron_rc_ptr_gen`) |
| `self` or a field of it in a pointer-receiver method | the receiver's, handed over by the caller in `iron_self_gen` (the method's own scope when no caller handed one over, which only ends the pointer early) |
| a growable list element (call argument only, E0330) | a generation made for the call and ended when it returns; the list is pinned meanwhile |

| the payload of a narrowed `T?` or interface `var` that is assigned again, closure-shared or global (call argument only, E0270) | a generation made for the call and ended when it returns |

`&` of a temporary or of a place inside one is rejected (E0270). The
variable of a `for` loop belongs to the scope of the iteration.

## I4. Scopes follow destruction order

hir_to_lir opens a scope for every inner block (entered at its start, left
on every exit edge after the block's defers and drops), and one more
after every local whose destruction can run a `drop` block
(`open_scope_after_droppable`): the locals declared after such a local are
left right before it is destroyed. So when a drop block runs, the scopes of
all locals declared after its value are closed and those of the locals
declared before it are open, which is exactly the set of destroyed and
alive locals. `defer` bodies run before any scope of the block is left.

## I5. A scope without a runtime identity cannot be observed

`iron_lir_prune_stack_scopes` removes the markers of a scope when no
pointer to one of its locals can be dereferenced after the scope ended.
A pointer taken in such a scope gets the generation live at function entry
(or the function's own scope). This is sound iff the escape analysis is:
a pointer **escapes** when it, or a value carrying its generation, reaches
anything but a dereference, a comparison or a private local slot:

- a field, element or pointer store, a store to a slot that is not private
  (global, captured, addressed, passed by address), a return, a capture, an
  object or list being built, any instruction not listed as harmless;
- a call argument, unless the callee is a known LIR function whose
  parameter (of type `*T` or `?*T`) does not escape by the same rules
  (least fixpoint over the module), or whose parameter is not a pointer
  (the callee receives the pointee);
- a pointer made from a place reached through it (`&p.f`, `&p.a[i]`) is
  followed like the pointer itself;
- a pointer-receiver method that takes `&self...` called on the pointee or
  on a place inside it (the method can keep a pointer with this
  generation anywhere).

For a local of an inner block, a store to any slot and a merge also count
(the slot may outlive the block). For a scope opened in the function's own
body after a droppable local they do not: nothing of the function runs
after that scope ends except destructors, which cannot see a private slot.

## I6. Every dereference is checked, or its check is provably redundant

`lower_genchecks` inserts a `GENCHECK` before every instruction that reads
or writes through a checked pointer or a heap/arena handle: field access,
load, store, a pointer handed to a non-pointer parameter (a method called
through it), and the receiver of a callee that is not a LIR function (an
interface dispatcher, a runtime method). This runs in every build mode.
The methods of collections cannot be called through a pointer (E0220), so
no runtime method receives an unchecked pointer.

The elision pass (off with `--no-optimize` and `--debug`) deletes a check
only when one of these holds:

- **Dominated.** An earlier check of the same root dominates it and no
  instruction between them (on any path, loop back edges included) can end
  storage: `free`, an rc release, an arena operation, a scope leave, an
  allocation, a store that can drop an owning value, a spawn, or a call
  that is not proven free of all of these. A pointer read from a field, a
  list element or a pointer variable is its own root, since what it holds
  can change between two reads.
- **Own frame.** The pointer is `&x` computed in this function for a local
  of the function's own scope (or of a scope that ends with it, I5) or a
  global. That storage outlives every instruction of the function.
- **Hoisted.** A check of a loop-invariant root moves to the preheader of
  a loop that is entered and executes the check on every iteration, with
  no storage-ending instruction in the loop.

A deleted check would have passed, so (1) and (2) are unaffected.

## I7. Storage a call is working on stays valid for the call

- An rc receiver read from a field, a list, a global or a cell is retained
  for the call.
- A receiver reached through a heap or arena handle or a pointer, when the
  callee may free, is pinned: ending its generation during the call panics.
- An interface value other code can reach is pinned while a method
  dispatched on it runs.
- A list with an element borrowed by a call is pinned: growing past
  capacity, removing, clearing or freeing it panics.
- A `var` parameter takes the caller's place by address. A variable
  outlives the call; for a field or an element the list it is in is
  pinned, the heap, arena or pointed-to value it is in has its generation
  pinned, and the rc object it is in is retained for the call.

## I8. List layouts and loops

A `layout: soa` list keeps a column per field in one block. The rules for
element pointers are the same in both layouts, and the fixtures
`ptr_list_aos_*` / `ptr_list_soa_*` run the same programs in each:

- `&ps[i].x` as a call argument points into the list's buffer (the element
  in the default layout, the column block in soa); `&ps[i]` in soa points at
  a gathered copy in the caller's frame that is stored back after the call.
  Either way the pointer carries the generation of the pin made for the
  call and is stale once the call returns, so a copy the callee keeps
  never reads the buffer, or the frame, later.
- The pin is on the list's buffer (`items`: the element array or the column
  block). Growth past the capacity, `pop`, `remove`, `insert`, `clear` and
  the free of a pinned list panic in both layouts (the list methods of
  emit_structs.c and of the IRON_LIST_IMPL macros check the pin).
- E0330 rejects the address of an element, or of anything stored inside
  one, anywhere but directly as a call argument, in both layouts.

A `for` loop over a growable list reads its elements through the list
value it took before the first iteration. The body may not change the
list it iterates (E0346 when the body does it directly), and for what the
compiler cannot see the loop pins the buffer like a call that borrows an
element (`$iter_pin` / `$iter_unpin`, the unpin on a scope around the loop
so that every exit runs it). `prune_iter_pins` removes the pin of a loop
whose body has no instruction that could change a list: no free, release,
spawn or await, and no call except to functions whose summary shows
neither (printing is the one runtime call treated as harmless). A
temporary list and a list kept as a stack array need no pin.

## Known limits

- 2^30 live slots, 4095 attached threads, 1,048,575 nested scopes: the
  runtime aborts with a message when one is exceeded.
- A drop block that reads a pointer into its own value's fields after
  those fields were destroyed by an inner destructor (an rc field whose
  object's drop block holds a pointer back into the owner) is not caught:
  a value's generation ends after its whole destruction.
- Unchecked pointers (`*unchecked T`, `Box`, `RawPtr`) are not covered.
- A loop over a `Map` or a `Set` is not pinned: the body must not add or
  remove entries (manual 9.10).

## Tests

- `tests/fuzz/pf_ptrlife.py`: programs with an exact model of every
  storage's lifetime; run plain, `--opts` (default, `--no-optimize`,
  `--debug` must agree), ASan+UBSan, valgrind and TSan. Fixed seeds run in
  ctest (`prog_fuzz_ptrlife`, `prog_fuzz_ptrlife_opts`).
- `tests/unit/test_runtime_gen_table.c`: I2 for slots, arena segments,
  scopes at any depth, across threads and across thread number reuse and
  exhaustion.
- `tests/lir/test_gencheck_elision.c`: I6.
- `tests/integration/v4/regressions/ptr_*`, `list_elem_*`, `arena_*`,
  `iface_*`: one fixture per bug.

# libpeg — fast incremental PEG parsing in C

A self-contained C11 implementation of the incremental PEG parser described
in *"Fast Incremental PEG Parsing"* (Zachary Yedidia and Stephen Chong,
SLE '21). After a full parse, applying a small edit and reparsing costs
time proportional to the edit, not the input: on the 187 KB Java
benchmark input, a single-character edit reparses ~100× faster than a
full reparse of the edited text (measured 15.0 ms -> 0.13 ms).

The library is C, with no dependencies beyond libc. C++ callers get
`include/peg/peg.hpp`, a header-only wrapper that turns the C API's
ownership rules into move-only RAII handles; it adds no runtime and no
second library to link.

This project is a C implementation based on the authors' original Go
implementation (github.com/zyedidia/gpeg, MIT). See *Credits* below.

## Layout

The tree is organised by module, with the public C API under
`include/peg/` and one directory per layer under `src/`.

| File                       | Purpose |
|----------------------------|---------|
| `include/peg/peg.h`        | End-to-end incremental parser: one object owning the compiled grammar, the memo table, and the subject buffer. |
| `include/peg/peg_pattern.h`| Pattern AST and grammar compiler (PEG → VM program), with optimization passes. |
| `include/peg/peg_vm.h`     | The parsing machine's public interface. |
| `include/peg/peg_memo.h`   | Memoization table: entries, relocatable captures, edit application. |
| `include/peg/peg_interval.h`| Interval tree with lazy shifting — the paper's core data structure (Sections 4.1/4.2). |
| `include/peg/peg_term.h`   | The term type: interned tags, half-open spans, and a tagged doubly-linked list with optional sub-lists. |
| `include/peg/peg_util.h`   | Allocation helpers (abort on OOM). |
| `include/peg/peg.hpp`      | The C++17 wrapper: move-only RAII handles over the C ABI. Header-only. |
| `src/alloc/util.c`         | Their implementation. |
| `src/interval/interval_tree.c` | Its implementation. |
| `src/memo/memo.c`          | Memo table: entries, relocatable captures, edit application. |
| `src/vm/`                  | `vm.c` (input wrapper, value stack, interpreter loop), `vm_code.c` (program builder, bytecode encoder, pre-decoder), `vm_internal.h` (types shared between the two). |
| `src/pattern/pattern.c`    | Pattern AST and grammar compiler. |
| `src/term/`                | `node.c` (the node type, its refcounting, and the iterative free), `sexpr.c` (the printer and the string form), `term_internal.h` (allocation). |
| `src/peg.c`               | The umbrella API implementation. |
| `tests/unit/test_interval_tree.c` | Unit, stress, and differential tests for the interval tree. |
| `tests/unit/test_memo.c`   | Tests for the memo table and capture relocation. |
| `tests/unit/test_vm.c`     | Tests for the parsing machine (matching, control flow, captures, memo/tree-memo, windows, checkers). |
| `tests/unit/test_pattern.c`| Tests for the compiler: literals/sets, predicates, repetition, recursive grammars, captures, search, checkers, memoization, tree-memo differential. |
| `tests/unit/test_term.c`   | Tests for the term type: ownership transfers, list and sub-list mutation, exact s-expression output, and deep chains/nests that would defeat a recursive free. |
| `tests/unit/test_peg.c`   | End-to-end tests: capture extents, incremental-vs-full-reparse differentials on random edits and on real testdata, and a full-vs-incremental benchmark. |
| `tests/unit/test_peg_cpp.cpp` | Tests for the C++ wrapper: move semantics, the same grammars and expected capture extents as the C suites, the term printer, and teardown of a 100 000-node chain. |
| `tests/oracle/interval_array.*` | Naive array-based interval map, used as the differential oracle. Test-only: not compiled into the library, not installed. |
| `tools/lpegc/bench.c`      | Benchmark harness (Bible search patterns, incremental Java parse). |
| `cmake/PegCompileOptions.cmake` | The warning policy, applied per target by `peg_set_compile_options()`. |
| `cmake/libpeg.pc.in`       | pkg-config template (installs `libpeg.pc`). |
| `cmake/libpegConfig.cmake.in` | `find_package(libpeg)` template, providing the `peg::peg` target. |
| `docs/nanopasses.org`      | The nanopass design document. |

## The parsing machine

A grammar compiles to a program for a small virtual machine in the style
of LPeg's parsing machine (Ierusalimschy, DLS '08): two registers — `ip`
(instruction) and `sp` (subject position) — and a heterogeneous stack of
backtrack points, call return addresses, and capture/memo bookkeeping.
Failure drives backtracking: instructions jump to the fail handler,
which pops until a backtrack point and restores its `(ip, sp)`.

The encoded byte format: opcode byte, one padding byte when the argument
length is even (so 16-bit arguments land on even offsets), then the
arguments; jump targets are 24-bit **mixed-endian** (`b[0]` holds bits
16–23, `b[1:3]` bits 0–15 little-endian). Before execution the byte
stream is pre-decoded into fixed-size instruction structs with jump
targets translated to instruction indices, so the interpreter does no
operand decoding.

The builder is streaming: label operands are written as placeholders and
backpatched by `vm_prog_finish` (labels are created with
`vm_prog_label`, positioned with `vm_prog_mark`), so forward references
work.

Memo instructions consult and fill the memo table (`MemoOpen`/`MemoClose`),
and tree-memoization instructions (`MemoTreeOpen`/`Insert`/`Tree`/`Close`)
maintain the merged parent entries of paper Section 4.3 with their stack
merge algorithm. The window optimization (Section 4.4) is the `window_*`
arguments of `vm_exec`: only captures overlapping the window are
constructed, and an edit clearing the window's memo entries forces their
regeneration.

### A note on capture ownership in the machine

Captures are shared freely between the stack, the memo table, and the
result tree, so they are reference-counted (see `peg_memo.h`); the rules are
documented at the top of `vm.c`. The subtle cases are `MemoTreeInsert`
and the `MemoTree` merge, where a live stack entry's captures are handed
to the memo table: the table stores a *referenced copy* of the pointer
array — the same capture objects, a second set of references — so both
sides stay alive exactly as long as they are needed.

## The grammar compiler

A grammar is described as a tree of *pattern* nodes (`peg_pattern.h`:
`pat_alt`, `pat_seq`, `pat_star`, ... and `pat_grammar`/`pat_nonterm`
for recursive definitions) and compiled by `pat_compile` into a VM
program. The pipeline:

1. **compile** — each node expands to a sequence of typed instructions
   with symbolic labels (an instruction-list IR);
2. **optimize** — pattern-level simplification (`Get`: alternations of
   single-byte matchers collapse into one charset, `!set .` becomes the
   complement set, ...) plus two IR passes: *head-fail* (Choice followed
   by a matcher becomes TestChar/TestSet/TestAny) and *jump replacement*
   (a jump to control flow becomes that instruction);
3. **encode** — the IR lowers through the streaming `vm_emit_*` builder
   to bytecode.

Recursive grammars compile each remaining definition to a Call/Return
pair; small leaf definitions (below the 100-node inline threshold) are
inlined first, and a call in tail position becomes a plain Jump
(tail-call optimization). `Star(Memo(p))` compiles to the tree
memoization instructions (Section 4.3 of the paper) for logarithmic
re-parses.

Two design notes:

- **Determinism**: grammar definitions are stored in an array in the
  given order, so a grammar always compiles to the same program.
- **Ownership**: pattern constructors take ownership of their children,
  so a pattern tree is a tree, not a DAG — one `pat *` must never be
  installed at two sites (the destructors would double-free).
  `pat_compile` mutates non-terminal nodes during grammar inlining, so
  one pattern tree must not be compiled twice. Its transient nodes
  (created by the `Get` simplifications and by `Search` compilation)
  reference children owned elsewhere and are shallow-freed when
  compilation ends.

## The end-to-end API

`peg/peg.h` bundles the whole incremental protocol into one object, making
the invariants structural: the buffer splice and the memo edit share one
code path, and each subject has exactly one memo table.

```c
pat *p = my_grammar();
peg *g = peg_new(p, 512);          /* 512: min examined-length to memoize */
pat_free(p);                         /* the pattern is compiled once */

vm_result r = peg_parse(g, data, len);   /* full parse, fills the table */
peg_edit(g, start, end, text, n);        /* splice + update the table */
vm_result r2 = peg_reparse(g);           /* incremental: reuses entries */
```

`peg_capture_interval(g, low, high)` re-runs with the window
optimization (paper Section 4.4) to rebuild only the captures
overlapping `[low, high)`.

`tests/unit/test_peg.c` verifies the paper's core claim end to end: after every
edit, `peg_reparse` must return exactly what a full reparse of the same
edited text returns (success and final position), checked over hundreds
of randomized edits on a tree-memoized list grammar and over the JSON
testdata. The benchmark prints the payoff on the Java testdata: a full
parse of ScriptRuntime.java in a few milliseconds, with incremental
reparses after single-character edits roughly 100× faster.

## The C++ wrapper

`peg/peg.hpp` is a header-only C++17 skin over the same ABI — no new
runtime, no separate library, no exceptions. Include it, link `libpeg`:

```cpp
#include "peg/peg.hpp"

libpeg::parser g(my_grammar(), 512);   // compiles the pattern; it may die here
libpeg::result r = g.parse(text);      // full parse, fills the memo table
g.edit(start, end, replacement);       // splice the buffer + update the table
libpeg::result r2 = g.reparse();       // incremental: reuses entries
```

Its job is to move the C API's ownership rules from prose into the type
system. Every owning handle (`pattern`, `program`, `memo`, `result`,
`capture`, `node`, `parser`, `input`) is **move-only**, so the rule that
one `pat *` must never appear at two sites is a compile error rather than
a double free, and the combinators take their children by value, so
handing a pattern to `pat_alt` visibly gives it away:

```cpp
libpeg::pattern l = libpeg::pattern::literal("a");
libpeg::pattern r = libpeg::pattern::literal("b");
libpeg::pattern alt = libpeg::pattern::alt(std::move(l), std::move(r));
// l and r are empty here; alt owns both trees.
```

Two things are deliberately absent: operator sugar for composing patterns
(`*p`, `p | q`) would hide that consumption, and exceptions would invent
a failure mode the library does not have — a failed parse is a `result`
with `success() == false`, and allocation failure aborts. The namespace
is `libpeg`, not `peg`: a C++ namespace cannot share a name with a type
declared in the same scope, and `typedef struct peg peg;` is part of the
frozen ABI.

`tests/unit/test_peg_cpp.cpp` holds the wrapper to the same expectations
as the C suites — the same capture grammar and extents, the same
incremental-equals-full differential — so the two spellings of the API
cannot drift apart.

## The term type

Parse results that carry structure — as opposed to a flat matched string —
are represented as *terms*: a chain of siblings, each node holding an
interned tag, a half-open `[lo, hi)` span into the subject, and an optional
nested sub-chain (`peg_term.h`). Spans are half-open to match the interval
tree, so `peg_span_overlap` *is* the memo-invalidation test: an entry
ending exactly where an edit starts never read the edited bytes.

The ownership rule is the load-bearing decision. A node's `next` and `sub`
each hold a reference; `prev` is **weak**. Counting `prev` too would make
every sibling pair own each other, and a two-node cycle could never reach
zero. Every list mutator is a pure ownership *transfer* — none of them
refs or unrefs — which is what keeps `splice` O(1) and makes "the caller's
reference moves here" a uniform contract.

Freeing is iterative and allocation-free: dead nodes are threaded onto a
worklist through their own `prev`, which is free precisely because the
node has already been unlinked. A 10 000-deep term is an ordinary result
for a nested document, and one stack frame per level does not fit in the
microcontroller budget this is aimed at; `peg_term_check` and the printer
use the same discipline with explicitly grown stacks.

The s-expression printout is the wire format, not a debugging aid: every
byte outside printable ASCII is escaped as `\xHH`, because a byte-oriented
parser cannot tell UTF-8 from text. `PEG_TERM_NO_STDIO` drops the two
`FILE*` entry points so the module builds on a target without stdio.
Allocation is confined to `term_internal.h`, the single swap point for a
freestanding allocator.

## The interval tree

The memoization table of an incremental packrat parser is keyed by
`(rule id, position)` and valued by parse results that cover an interval
of the input — the bytes the rule *examined*, which may be far more than
it matched, because PEGs have unbounded lookahead. Three operations
dominate:

- **overlap eviction** — an edit invalidates every entry that examined
  any changed byte;
- **shifting** — every entry starting at or after the edit moves;
- **lookup by (id, pos)** — during reparse, to skip already-parsed
  regions.

A hash map or array makes eviction and shifting linear. This tree is an
AVL tree keyed by `(pos, id)`, augmented with a subtree-maximum (classic
interval tree), so eviction is `O(m + log n)` and lookup `O(log n)`.

**Lazy shifts** (paper Section 4.2): shifts are appended to a global log
with increasing timestamps instead of being applied. Each node remembers
the newest shift it has absorbed; whenever a node is observed it first
replays newer shifts. A shift at index `i` cannot affect a subtree whose
maximum is below `i`, so far-away subtrees skip shifts cheaply.

**Multiple intervals per key**: tree memoization (paper Section 4.3)
needs several entries at the same `(id, pos)`; they share one node, and
`itree_find_largest` returns the longest so reparses skip as much input
as possible.

### A note on memory management

Eviction is explicit — `itree_remove_and_shift_cb` takes an `on_evict`
callback, and the memo table passes its entry destructor so invalidated
entries (and their captures) are freed at edit time. Everything the
table stores is owned by the table: `memo_table_put` takes ownership of
the capture array and its elements, and `memo_table_free` releases it
all.

### On AVL rebalancing

Two rebalancing subtleties are handled explicitly and covered by
regression tests:

1. **Rebalancing after overlap removals.** Removals rebalance on every
   unwind; without that, subtrees shrink without rotations and the tree
   slowly degrades (heights grow, and the `O(log n)` edit bound quietly
   erodes).

2. **Single rotation is not enough.** A single edit can delete a node's
   *entire* subtree (all its intervals overlapping the edit), changing a
   balance factor by more than 1 — e.g. +1 to +3. Textbook AVL code
   assumes at most ±2 and leaves such nodes unbalanced. This was found
   by delta-debugging a randomized failure to a 9-op reproducer and is
   covered by `test_regression_whole_subtree_removal`. `iv_rebalance`
   recursively repairs arbitrarily large imbalance.

Also, `update_max` deliberately never *shrinks* a node's stored subtree
maximum: children absorb shifts independently, and a lagging child still
holds pre-shift (too large) maxima. Over-estimates are always safe (they
only weaken pruning); under-estimates would be a correctness bug.

## Building and testing

The project builds with CMake (≥ 3.16), a C11 compiler, and — for the
C++ wrapper test — a C++17 compiler:

```sh
cmake -B build                # defaults to a Release build
cmake --build build
ctest --test-dir build        # run the test suite
```

Options:

| Option            | Default | Meaning |
|-------------------|---------|---------|
| `PEG_BUILD_TESTS`| `ON`    | Build the test suite (CTest targets). |
| `PEG_BUILD_BENCH`| `OFF`   | Build `bench_peg`, the benchmark harness. |
| `PEG_WERROR`     | `OFF`   | Treat compiler warnings as errors. |
| `PEG_SANITIZE`   | `OFF`   | Build with ASan and UBSan. |

Warning flags live in `cmake/PegCompileOptions.cmake` and are applied per
target by `peg_set_compile_options()`; new targets should call it rather
than spelling out flags.

Install with the usual:

```sh
cmake --install build          # or: cmake --install build --prefix /usr/local
```

This installs the static library `libpeg.a`, the public headers
(`peg.h`, `peg_pattern.h`, `peg_vm.h`, `peg_memo.h`, `peg_interval.h`,
`peg_term.h`, `peg_util.h`, `peg.hpp`) under `include/peg/`, and the
package metadata for two ways of consuming it:

```cmake
# CMake: the imported target is peg::peg, the same name as the in-tree
# alias, so a target_link_libraries line does not change.
find_package(libpeg REQUIRED)
target_link_libraries(myparser PRIVATE peg::peg)
```

```sh
# pkg-config, for everything else.
cc myparser.c $(pkg-config --cflags --libs libpeg)
```

Building the benchmarks requires the large test inputs (bible.txt,
ScriptRuntime.java, test.json) — they are not distributed with the
source. The build looks for them in a sibling `testdata/` directory by
default; point `PEG_TESTDATA_DIR` at wherever they actually live:

```sh
cmake -B build -DPEG_TESTDATA_DIR=/path/to/testdata
```

The end-to-end test skips itself gracefully when they are absent, so a
passing run does not by itself mean the differential tests ran — check
for a `SKIP:` line.

Sanitizer build:

```sh
cmake -B build-asan -DPEG_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan
```

### Guix

A package declaration is provided in `guix.scm`:

```sh
guix build -f guix.scm        # build the package
guix shell -f guix.scm        # environment with the library
```

The test suite layers:

- **Unit tests** — overlap semantics (half-open: touching intervals do
  not overlap, since a memo entry ending exactly at the edit start never
  read the edited bytes), find-largest tie-breaking, lazy shift
  application, location handles surviving deletion and rebalancing.
- **Stress test** — randomized add/remove/shift/verify sequences over
  several PRNG seeds, with full invariant checking via `itree_verify`.
- **Differential test** — the tree compared op-by-op against the naive
  array implementation.
- **Module tests** — the memo table (`test_memo`), the machine
  (`test_vm`), and the compiler (`test_pattern`) each get their own
  suite.
- **C++ wrapper test** — `test_peg_cpp` compiles every public header as
  C++ through `peg.hpp` and exercises the same grammars through the RAII
  handles, so a header that is only valid C, or a handle that forgets to
  transfer ownership, fails the build or the run rather than a
  downstream consumer's.
- **End-to-end differential** — `test_peg` checks that an incremental
  reparse after an edit always equals a fresh full parse of the edited
  text (the paper's correctness claim), over randomized edits and real
  testdata, and times full vs. incremental parses as a benchmark.

### Testing note: valid edit magnitudes

The differential/stress generators constrain every shift to `amt >= -len`
for an edit over `[low, low+len)`: an edit replaces `len` bytes with
`max(len+amt, 0)` bytes, so it can never shrink the text by more than
`len`. This is not an artificial restriction — it is a precondition the
memo table always satisfies (`memo_table_apply_edit` computes
`amt = nnew - (end - start)` with `nnew >= 0`) and the tree relies on
it: a larger negative shift could move a surviving interval's key past a
non-shifted interval to its left, breaking BST ordering.

## Credits

The parsing algorithm and data structures are from *"Fast Incremental
PEG Parsing"* by Zachary Yedidia and Stephen Chong (SLE 2021). This C
implementation is derived from the authors' Go implementation of the
paper, github.com/zyedidia/gpeg (MIT License, Copyright (c) 2020
Zachary Yedidia), whose AVL tree code in turn derives from an
MIT-licensed AVL implementation. This project keeps the same MIT terms —
see `LICENSE`.

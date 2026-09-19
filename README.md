# libpeg

Fast incremental PEG parsing in C.

A C implementation of the incremental PEG parser described in *Fast
Incremental PEG Parsing* (Yedidia & Chong, SLE 2021). A grammar compiles to
a program for an LPeg-style parsing machine (Ierusalimschy, *A Parsing
Machine for PEGs*, DLS '08), backed by a memoization table stored in a
lazily-shifted AVL interval tree. After a full parse, reparsing an edited
input costs time proportional to the *edit* rather than to the size of the
input.

## Status

Version 0.1.0. The parsing and memoization machinery is complete and
tested; see [Tests](#tests). Not yet released.

## Requirements

- A C11 compiler (GCC or Clang)
- CMake 3.16 or newer
- Guix, optionally, for a reproducible build (see [Guix](#guix))

## Building

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

| Option | Default | Meaning |
| --- | --- | --- |
| `PEG_BUILD_TESTS` | `ON` | Build the test suite |
| `PEG_BUILD_BENCH` | `OFF` | Build the benchmark harness |
| `PEG_WERROR` | `OFF` | Treat compiler warnings as errors |
| `PEG_SANITIZE` | `OFF` | Build with AddressSanitizer and UndefinedBehaviorSanitizer |
| `PEG_TESTDATA_DIR` | `<src>/testdata/` | Where the large test and benchmark inputs live |

The build defaults to `Release`: the benchmarks are meaningless at `-O0`,
so an unconfigured build still gets optimization.

The large inputs (`testdata/bible.txt`, `testdata/test.json`,
`testdata/ScriptRuntime.java`) are used only by `test_peg` and by the
benchmarks. The other four tests run without them, and `test_peg` skips its
data-driven cases gracefully when the directory is absent.

For a sanitizer run:

```sh
cmake -S . -B build-asan -DPEG_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan
```

## Installing

```sh
cmake --install build --prefix /usr/local
```

This installs `libpeg.a` and the five public headers under `include/peg/`.
Everything in `src/` — including the internal headers `util.h` and
`vm_internal.h` — is private and is not installed.

Consuming the installed library:

```sh
cc -I/usr/local/include myprog.c -L/usr/local/lib -lpeg
```

## Quick start

A grammar is a tree of patterns. Build it, compile it, and parse:

```c
#include <stdio.h>
#include <string.h>
#include <peg/peg.h>

/* Expr   <- Term ([+-] Term)*
 * Term   <- Factor (muldiv Factor)*
 * Factor <- [0-9]+ / '(' Expr ')'
 *
 * muldiv is the two-byte set of '*' and '/'; it is built with
 * vm_charset_fill because there is no way to spell a set literal of
 * those two characters inside a C comment. */
static pat *arith_grammar(void)
{
	vm_charset digits, plus_minus, mul_div;
	vm_charset_range(&digits, '0', '9');
	vm_charset_fill(&plus_minus, (const uint8_t *)"+-", 2);
	vm_charset_fill(&mul_div, (const uint8_t *)"*/", 2);

	pat *expr = pat_concat((pat *[]){
		pat_nonterm("Term"),
		pat_star(pat_concat((pat *[]){
			pat_set(&plus_minus),
			pat_nonterm("Term") }, 2)) }, 2);
	pat *term = pat_concat((pat *[]){
		pat_nonterm("Factor"),
		pat_star(pat_concat((pat *[]){
			pat_set(&mul_div),
			pat_nonterm("Factor") }, 2)) }, 2);
	pat *factor = pat_alt(
		pat_plus(pat_set(&digits)),
		pat_concat((pat *[]){ pat_literal("(", 1),
		                      pat_nonterm("Expr"),
		                      pat_literal(")", 1) }, 3));

	const char *names[] = { "Expr", "Term", "Factor" };
	pat *defs[] = { expr, term, factor };
	return pat_grammar("Expr", names, defs, 3);
}

int main(void)
{
	pat *p = arith_grammar();
	peg *g = peg_new(p, 512);	/* 512: memo threshold */
	pat_free(p);			/* the parser owns the compiled program */

	const char *text = "12*(3+4)";
	size_t len = strlen(text);

	vm_result r = peg_parse(g, (const uint8_t *)text, len);
	if (r.success)
		printf("parsed %d of %zu bytes\n", r.pos, len);
	else
		printf("no match\n");
	vm_result_free(&r);

	peg_free(g);
	return 0;
}
```

Compile with `-Iinclude` and link `libpeg.a`, or with the installed flags
above.

### Incremental reparsing

The point of the library is that a second parse after an edit reuses the
first one's work. The incremental protocol is three calls on the same `peg`
object:

```c
vm_result r = peg_parse(g, (const uint8_t *)"12*(3+4)", 8);	/* full parse */
vm_result_free(&r);

/* Replace [0,2) with "7": the subject is now "7*(3+4)" */
peg_edit(g, 0, 2, (const uint8_t *)"7", 1);

r = peg_reparse(g);	/* incremental: reuses the surviving entries */
vm_result_free(&r);
```

`peg_edit` splices the buffer and updates the memo table — entries that
examined a changed byte are evicted, the rest shift lazily. The next
`peg_reparse` picks up from the survivors. The whole protocol is bundled
into one type so the invariants (the edit's coordinates matching the buffer
splice, one table per subject, the furthest-read reset between parses)
cannot be violated by the caller.

`peg_capture_interval(g, low, high)` is the window optimization (paper
Section 4.4): it extracts only the captures overlapping `[low, high)`, so
an editor can refresh just the region it drew.

## API

Five headers, all installed under `include/peg/`:

| Header | Contents |
| --- | --- |
| `peg.h` | The end-to-end incremental parser: `peg_new`, `peg_parse`, `peg_edit`, `peg_reparse`, `peg_capture_interval` |
| `pattern.h` | The pattern AST and the grammar compiler: `pat_*` constructors, `pat_grammar`, `pat_compile`, `pat_prettify` |
| `vm.h` | The parsing machine: opcodes, `vm_prog`/`vm_code`, `vm_exec`, charsets, checkers |
| `memo.h` | The memoization table and the reference-counted capture trees |
| `interval_tree.h` | The AVL interval tree the memo table is built on |

Most users need only `peg.h` and `pattern.h`. `vm.h` is needed for
`vm_charset` (building byte sets) and for `vm_result` / `vm_result_free`.
`memo.h` and `interval_tree.h` are exposed because the public types refer
to them, not because they are meant to be driven directly.

Ownership rules are documented in `pattern.h`: a pattern owns its
sub-patterns, a grammar owns its definitions (they are *transferred* to
`pat_grammar`), and a compiled program is independent of the pattern it
came from.

## Layout

```
include/peg/    public headers — the only thing installed
src/            implementation + internal headers (util.h, vm_internal.h)
tests/          the test suite
tests/interval_array.[ch]
                naive array oracle, compiled into test_interval_tree only
bench/          benchmark harness
testdata/       large real-world inputs for test_peg and the benchmarks
```

## Tests

```sh
ctest --test-dir build
```

| Test | Checks | What it covers |
| --- | --- | --- |
| `test_interval_tree` | ~1 089 000 | The AVL interval tree, differentially against a naive array oracle over randomized operation sequences |
| `test_memo` | 41 | Memo table insertion, eviction, and edit shifting |
| `test_vm` | 267 | The parsing machine: instruction semantics, backtracking, captures, checkers |
| `test_pattern` | 103 | The compiler: PEG constructs, optimization, prettify |
| `test_peg` | 265 | End-to-end: the paper's grammars, a JSON grammar, the PEG language parsing itself, and the incremental differential test |

The incremental differential test is the important one: it checks that an
incremental reparse after an edit produces exactly what a full reparse of
the edited text produces.

All five run clean under `-DPEG_SANITIZE=ON` (ASan + UBSan, including
LeakSanitizer).

## Benchmarks

```sh
cmake -S . -B build -DPEG_BUILD_BENCH=ON
cmake --build build -j
./build/bench_peg
```

`bench_peg` parses `testdata/bible.txt` (4.4 MB, no memoization) with
several search patterns, then runs the paper's headline case: a full parse
of `testdata/ScriptRuntime.java` (187 KB) with a Java grammar, followed by
incremental reparses after single-character edits.

Representative numbers (one machine, Release, median of batch):

| Benchmark | ns/op |
| --- | --- |
| SearchFirstAbram | 690 000 |
| SearchLastTubalcain | 3 000 000 |
| SearchFirstEartt | 14 100 000 |
| OmegaGrammar | 38 700 000 |
| OmegaPattern | 39 300 000 |
| SearchLastAbram | 82 300 000 |

| Incremental (Java, 187 KB) | ns/op |
| --- | --- |
| Full parse | 14 960 000 (14.96 ms) |
| Full parse of the edited text | 14 940 000 |
| **Incremental reparse** | **133 000 (0.133 ms)** |

The incremental reparse is ~112× faster than the full parse it replaces.
Treat the absolute numbers as machine-specific; the ratio is the point.

## Guix

```sh
guix build -f guix.scm     # build the package (runs the test suite)
guix shell -f guix.scm     # development shell
guix package -f guix.scm   # install into the user profile
```

## License

MIT — see [LICENSE](LICENSE).

## References

- Roberto Ierusalimschy, *A Parsing Machine for PEGs*, DLS '08.
- Aaron Yedidia and Michael Chong, *Fast Incremental PEG Parsing*, SLE 2021.

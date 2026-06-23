---
name: deprecate-rpc
description: Deprecate or remove SPDK RPC parameters or entire RPC methods following the deprecation policy (at least one full release)
argument-hint: "[param_name or rpc_name]"
triggers:

- user
- model

---

# Deprecate SPDK RPC Parameters or Entire RPCs

Use this skill when asked to deprecate an RPC parameter, rename/replace a parameter, or deprecate an entire RPC method in SPDK.

## Overview

SPDK requires at least one full release of deprecation notice before removal: announce in release N, remove no earlier than release N+1.
Deprecating a parameter or RPC touches **eight areas** that must all stay in sync.

---

## Phase 1 -- Deprecate (announce release)

Work through each file group in order. Every step is mandatory unless marked otherwise.

### 1. `schema/schema.json` -- mark the parameter deprecated

Add `"deprecated": true` to the parameter object in the schema.

```json
{
  "name": "old_param_name",
  "type": "uint32",
  "deprecated": true,
  "description": "Old description. Default: 0"
}
```

If the parameter is being **replaced** by a new one, add the new parameter entry
in the same `params` array (keep it near the deprecated one for readability).

### 2. `lib/*/*.c` -- register the deprecation and emit warnings

Use the `SPDK_LOG_DEPRECATION_REGISTER` / `SPDK_LOG_DEPRECATED` macros.

```c
SPDK_LOG_DEPRECATION_REGISTER(tag_name,
                  "use new_param instead", "vXX.YY",
                  SPDK_LOG_DEPRECATION_EVERY_24H);
```

Pick the rate limit that fits:

| Macro constant                       | When to use                              |
|--------------------------------------|------------------------------------------|
| `SPDK_LOG_DEPRECATION_ALWAYS`        | Param is NOP or behaviour-breaking       |
| `SPDK_LOG_DEPRECATION_EVERY_24H`     | Normal deprecation -- warn once per day  |

If the old parameter still needs to work during the deprecation window, write a
**custom JSON decoder** wrapper that (a) logs the deprecation and (b) delegates
to the real decoder:

```c
static int
decode_old_param(const struct spdk_json_val *val, void *out)
{
    SPDK_LOG_DEPRECATED(tag_name);
    return spdk_json_decode_uint32(val, out);
}
```

Then swap the decoder in the `spdk_json_object_decoder` table:

```c
- {"old_param", offsetof(...), spdk_json_decode_uint32, true},
+ {"old_param", offsetof(...), decode_old_param, true},
```

If there is **conversion logic** (old units -> new units), keep it in the RPC
handler with a sentinel check (e.g. `UINT64_MAX` means "not supplied") so the
old parameter still works during the deprecation period.

### 3. Config writer / dump function -- emit the new parameter name

If the subsystem has a config-dump or state-save function (typically in
`*_tgt.c` or similar, using `spdk_json_write_*` APIs), update it to emit the
**new** parameter name. This ensures that any saved configuration written by a
running instance already uses the replacement name, so reloading the config
will not trigger deprecation warnings.

```c
/* Before */
spdk_json_write_named_string(w, "old_param", value);

/* After */
spdk_json_write_named_string(w, "new_param", value);
```

If the type changed (e.g. comma-separated string to JSON array), rewrite the
serialization logic to match the new schema type.

### 4. `python/spdk/cli/*.py` -- update argparse help text

The `genrpc.py` lint (`lint_py_cli`) **requires** the word `Deprecated` (capital D)
in the `help=` string of any parameter whose schema has `"deprecated": true`.

```python
# Before
p.add_argument('--old-param', help='Old description', type=int)

# After -- add deprecation notice pointing to the replacement
p.add_argument('--old-param',
               help='Old description. Deprecated, use --new-param instead.',
               type=int)
```

If the old param and new param are mutually exclusive, wrap them in a
`p.add_mutually_exclusive_group()`:

```python
group = p.add_mutually_exclusive_group()
group.add_argument('--old-param', dest='old_param',
                   help='Old description. Deprecated, use --new-param instead.',
                   type=int)
group.add_argument('--new-param', help='New description', type=int)
```

### 5. Tests -- migrate all usages to the new parameter/RPC

Search the entire tree for every test that passes the deprecated parameter or
calls the deprecated RPC and **switch it to the replacement immediately**.
Do not leave tests using the old name -- the deprecation window is for external
consumers, not for the project's own test suite.

```bash
# Find all references in tests (adjust the pattern to match your case)
grep -rn 'old_param\|old_rpc_name' test/
```

For each hit:

- Replace the old parameter/RPC with the new one in the test JSON payloads,
  Python RPC calls, and bash `rpc_cmd` invocations.
- If a test **specifically validates** that the deprecated path still works
  (compat test), keep it but add a comment noting the target removal release.
- Run the affected test suite to confirm nothing breaks.

### 6. `deprecation.md` -- add a notice section

Under the appropriate `###` subsystem heading, add a `####` entry whose title
is the **tag name** used in `SPDK_LOG_DEPRECATION_REGISTER`:

```markdown
#### `tag_name`

The `old_param` parameter of `rpc_method_name` RPC is deprecated and will be
removed in vXX.YY. Use `new_param` instead.
```

### 7. Verify

Run the genrpc lint to confirm schema, CLI, and deprecation annotations are
consistent:

```bash
python3 scripts/genrpc.py --lint
```

This checks:

- Every schema param with `"deprecated": true` has `Deprecated` in CLI help
- Schema params and CLI args are in sync
- Type mappings are correct

---

## Phase 2 -- Remove (target release)

Once the target release arrives, remove **all** compat/shim code.

### 1. `schema/schema.json` -- delete the deprecated parameter entry

Remove the entire parameter object from the `params` array. Clean up any
descriptions on the replacement param that reference the old name.

### 2. `lib/*/*.c` -- remove deprecation machinery

Delete in this order:

1. `SPDK_LOG_DEPRECATION_REGISTER(...)` block
2. Custom decoder wrapper function (e.g. `decode_old_param`)
3. Decoder table entry `{"old_param", ...}`
4. Sentinel initialization (e.g. `req.old_param = UINT64_MAX;`)
5. Conversion / compat logic in the RPC handler
6. Any field in the RPC context struct that only existed for the old param

### 3. `python/spdk/cli/*.py` -- simplify CLI

- Remove the deprecated `add_argument` call
- If a mutually-exclusive group was used, dissolve it and keep only the
  replacement argument as a standalone `add_argument`
- Remove any `dest=` aliasing that was only needed for the old name

### 4. Tests -- remove any remaining compat test references

If any compat tests were kept during Phase 1, delete them now. Grep the test
tree one more time to confirm zero references to the old parameter or RPC name
remain:

```bash
grep -rn 'old_param\|old_rpc_name' test/
```

### 5. `deprecation.md` -- remove the notice section

Delete the `####` block for this tag.

### 6. `CHANGELOG.md` -- document the removal

Under the upcoming release heading, add a removal note:

```markdown
### subsystem_name

Removed the deprecated `old_param` parameter from the `rpc_method_name` RPC.
Use `new_param` instead.
```

### 7. Verify

```bash
python3 scripts/genrpc.py --lint
```

---

## Deprecating an Entire RPC (not just a parameter)

The same two-phase approach applies, but with broader scope:

**Phase 1 (deprecate):**

1. `SPDK_LOG_DEPRECATION_REGISTER` at the top of the RPC handler.
2. Call `SPDK_LOG_DEPRECATED(tag)` at the start of the handler function.
3. Add `"deprecated": true` to the **method** object in `schema/schema.json`
   (at the method level, not param level).
4. Update the config writer/dump function to call the replacement RPC.
5. Add `[Deprecated]` to the CLI `help=` and/or subparser description.
6. Migrate all tests to the replacement RPC (same rules as parameter Phase 1
   step 5).
7. Add a `deprecation.md` section.

**Phase 2 (remove):**

1. Delete the entire RPC handler, decoder table, and context struct.
2. Remove the `SPDK_RPC_REGISTER(...)` call.
3. Delete the CLI subparser (the `add_parser` block).
4. Remove from `schema/schema.json`.
5. Remove any remaining test references to the old RPC.
6. Remove `deprecation.md` entry; add `CHANGELOG.md` entry.

---

## Quick Checklist

```text
Deprecate:
  [ ] schema/schema.json    -- "deprecated": true (+ new param if replacing)
  [ ] lib/*/*.c              -- SPDK_LOG_DEPRECATION_REGISTER + custom decoder
  [ ] config writer          -- emit new param name in dump/save function
  [ ] python/spdk/cli/*.py   -- "Deprecated" in help text
  [ ] test/                  -- migrate all test usages to new param/RPC
  [ ] deprecation.md         -- #### tag_name section
  [ ] genrpc lint passes     -- python3 scripts/genrpc.py --lint

Remove:
  [ ] schema/schema.json    -- delete deprecated param entry
  [ ] lib/*/*.c              -- delete register, decoder, compat logic
  [ ] python/spdk/cli/*.py   -- delete old arg, dissolve exclusive groups
  [ ] test/                  -- remove remaining compat test references
  [ ] deprecation.md         -- delete section
  [ ] CHANGELOG.md           -- add removal note
  [ ] genrpc lint passes     -- python3 scripts/genrpc.py --lint
```

## Reference Commits

| Commit | What it demonstrates |
|--------|---------------------|
| `982dc0c77f5` | Deprecate a param: `num_shared_bufs` -- custom decoder wrapper, deprecation.md notice |
| `8cebc8a8c1f` | Lint enforcement: require `Deprecated` in CLI help for `"deprecated": true` schema params |
| `31e152b049b` | Deprecate a param + a behaviour: `hide_metadata` param and mixed `dif_insert_or_strip` configs |
| `60dd38eea45` | Remove deprecated params: `max_discard_size_kib` / `max_write_zeroes_size_kib` -- full Phase 2 cleanup |

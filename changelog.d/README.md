# Changelog fragments

Document API, ABI, JSON RPC and major functional changes in a fragment in the
same commit as the change.

Create `changelog.d/<category>/<filename>.md`, for example
`changelog.d/accel/software_interrupt_mode.md`. Choose a descriptive filename without
an identifier or required prefix. The category directory supplies the section
heading. Do not add headings or nested directories.

Write one logical change per file. Related paragraphs, lists and code can stay
together:

```markdown
Added interrupt mode support to the software accel module.
```

Preview the complete changelog with `scripts/changelog.py --dry-run`.
Before release, run `scripts/changelog.py` and commit `CHANGELOG.md` with the
fragment deletions. It merges the fragments into the release in `VERSION`.
Use `--release v26.09` to select another existing release section.

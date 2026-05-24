# `pak_content_types.tsv` — reference metadata

A small table describing well-known file extensions that appear inside
Quake-engine archives (PAK / PK3 / PK4 / Sin / Daikatana). Provided as
reference data for consumers of libpakka; **libpakka itself does not
read this file at runtime.**

## Schema

Tab-separated values with one header row. Fields contain no tabs or
newlines, so the format parses with a plain `split('\t')` in any
language — no CSV-style quoting rules.

| column        | shape                                                                          |
| ------------- | ------------------------------------------------------------------------------ |
| `extension`   | lowercase ASCII, no leading dot, unique within the file                        |
| `description` | short English label; printable ASCII, no tab or newline                        |
| `kind`        | one of `map`, `model`, `sprite`, `texture`, `palette`, `sound`, `music`, `video`, `script`, `code`, `document`, `demo`, `archive`, `data` |

`kind` is intended as a coarse classification — primarily a hook for
consumers to pick an icon or group entries. New kinds may be added over
time; consumers should treat unknown values defensively.

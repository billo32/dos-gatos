# Source manifest, version 2: values, rules, templates, color scales

A source (an entry of apps.json, or `source` in an extension manifest) turns an HTTP response
into what the clock shows: a text, a color and an icon. Version 1 has one value (`path`,
`find`/`keep`, `re`) formatted with `scale`, `dec` and `fmt`, a color and an icon, plus icon
rules (`icons`). Version 2 adds, all optional and all backwards compatible:

| field    | what it does |
|----------|--------------|
| `values` | more named values from the same response: `{"code": "current.weather_code"}` |
| `text`   | a template for the text: `"{t|round}° {wind|round}"` |
| `color`  | still `"#RRGGBB"`, or a scale: `{"by": "t", "scale": [[-10, "#4DA3FF"], [30, "#FF4B4B"]]}` |
| `rules`  | conditions that change the text, color or icon, or hide the screen |
| `frames` | several screens in turn within the app: `[{"text": "{sym|upper}"}, {"text": "{chg|fixed:1|sign}%"}]` |
| `each`, `item` | repeat the frames for every element of a list in the response (coins, pipelines…) |

A source is **version 2** when it has `values`, `text`, `rules` or `frames`, or a `color` that is an object.
Only then is the result worked out by the host (the Mac app, the web app) and sent to the clock
finished (`{"t":"resp","id","status","text","color"?,"icon"?}`, firmware ≥ 0.9.1). A clock on
its own over Wi‑Fi still shows the version-1 result (`path` + `fmt`), so a version-2 source should
keep a sensible `path` and `fmt`.

## Values

Every value is a string, or missing.

- `v` — the version-1 value: `path` / `find`+`keep` / `re` applied to the response, as before.
- each entry of `values` — a path in the same syntax as `path` (`a.b.0.c`, `items.length`)
  applied to the whole response parsed as JSON. A value that isn't there is missing; so are all
  of them when the response isn't JSON. Names are `[A-Za-z_][A-Za-z0-9_]*` except `all`, `any` and `not`; `v` can't be
  redefined. At most 8 values.

A value is **numeric** when, trimmed, it is a finite decimal number (`-3`, `21.5`, `1e3`).

## Text templates

`{name}` is replaced with the value, `{name|filter|filter:arg}` passes it through filters left to
right. A missing value is an empty string. Anything that isn't a well-formed `{…}` is kept as is.

Without `text` the text is the version-1 one: `fmt` with `{v}` replaced by `v` after `scale` and
`dec` (`v` missing → the source shows nothing).

| filter       | result |
|--------------|--------|
| `round`      | nearest integer (halves away from zero) |
| `fixed:n`    | `n` decimals (0–6) |
| `mul:x`      | times `x` |
| `abs`        | absolute value |
| `k`          | short form: below 1000 unchanged; then `1.2K`, `12K`, `1.2M`, `3.4B` (one decimal below 10, else whole; `.0` dropped) |
| `upper`, `lower` | case |
| `trunc:n`    | first `n` characters |
| `default:x`  | `x` when the value is missing or empty |
| `map:a=b,c=d`| `b` when the value equals `a` (case-insensitive), and so on; otherwise unchanged |
| `sign`       | a `+` in front of a positive number (`2.4` → `+2.4`); use it after `fixed` |

Number filters leave a non-numeric value unchanged. Numbers are written the shortest way that
reads back the same (`3`, `21.5`, `0.1`), with `fixed` as the exception. An unknown filter
leaves the value unchanged.

## Conditions (`when`)

An object; every key must hold:

- a value name → a test: `{"gt": 30}`, `{"in": ["failed", "canceled"]}`; several operators in
  one test must all hold;
- `"all": [when, …]`, `"any": [when, …]`, `"not": when`.

| operator | holds when |
|----------|------------|
| `eq`, `ne` | equal / not equal: numerically when both sides are numeric, else as text, case-insensitive |
| `lt`, `lte`, `gt`, `gte` | numeric comparison; false when the value isn't numeric |
| `in`     | `eq` to one of the list |
| `contains` | the text contains it, case-insensitive |
| `matches` | the regular expression finds a match (use the common subset of JavaScript and Rust syntax) |
| `exists` | `true`: the value is present and not empty; `false`: the opposite |

A test on a missing value: only `exists: false` and `ne` hold.

## Rules

`"rules": [{"when": …, "text"?, "color"?, "icon"?, "hide"?, "stop"?}, …]` — in order, every rule
whose `when` holds (a rule without `when` always does) overrides the fields it has. `hide: true`
skips the screen (the result text is empty); `stop: true` ends the list after this rule. A
rule's `text` is a template and its `color` is a color or a scale.

## Colors

`"#RRGGBB"`, or `{"by": value, "scale": [[number, "#RRGGBB"], …]}` with ascending numbers: below
the first stop the first color, above the last the last, in between each channel interpolated
linearly and rounded (halves up). A scale on a non-numeric or missing value gives the source's
default, `#FFFFFF`. The result is written `#RRGGBB` in capitals.

## Icon

A rule's `icon` wins; else the version-1 icon rules (`icons`); else `icon`.

## Frames

`"frames": [frame, …]` makes the app show several screens in turn, each for `fdur` seconds
(1–30, default 3). A frame is `{"text"?, "color"?, "icon"?, "rules"?}`: it starts from what the
source alone gives (its `text`/`fmt`, color, icon and `rules`), then its own fields and rules
override that. A frame hidden by a rule is skipped.

`"each": path` repeats the frames for every element of the array or object at `path` in the
response (`""` is the whole response), at most 8 elements. For each element:

- `key` is the object key, or the array index;
- `"item": {name: path}` are values taken from the element itself (paths relative to it).

Values are looked up in the element's names first (`key`, `item`), then the source's. At most 16
frames in all; with no frame left, the clock skips the app. The app stays on screen for the longer
of its `dur` and all its frames.

## Result

`{text, color, icon}` — `text` empty means the clock skips the screen. With `frames`, a list of
those (`{"t":"resp","id","status","frames":[…],"fdur"}`, firmware ≥ 0.9.2; older firmware gets
the first frame). A set of conformance vectors (manifest, sample response, expected result) checks that the Mac app
and the web app work these out identically.

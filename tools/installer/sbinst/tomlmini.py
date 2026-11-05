"""TOML for the installer manifest: tomllib (Python 3.11+), else a small parser for the subset the
manifest uses (tables, arrays of tables, strings, integers with _, booleans, flat arrays, inline
tables of scalars), so the installer also runs on an older Linux Python."""
import re

try:
    import tomllib as _tomllib
except ImportError:          # pragma: no cover - exercised by the fallback test
    _tomllib = None


class TOMLError(ValueError):
    pass


def loads(text):
    if _tomllib is not None:
        try:
            return _tomllib.loads(text)
        except _tomllib.TOMLDecodeError as e:
            raise TOMLError(str(e)) from None
    return loads_fallback(text)


_KEY = re.compile(r'^([A-Za-z0-9_-]+)\s*=\s*(.*)$')


def _strip_comment(s):
    out, q = [], None
    for ch in s:
        if q:
            out.append(ch)
            if ch == q and (len(out) < 2 or out[-2] != '\\'):
                q = None
        elif ch in '"\'':
            q = ch
            out.append(ch)
        elif ch == '#':
            break
        else:
            out.append(ch)
    return ''.join(out).strip()


def _split_top(s, sep=','):
    parts, depth, q, cur = [], 0, None, []
    for ch in s:
        if q:
            cur.append(ch)
            if ch == q and cur[-2:-1] != ['\\']:
                q = None
            continue
        if ch in '"\'':
            q = ch
        elif ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        parts.append(''.join(cur).strip())
    return parts


def _value(v):
    v = v.strip()
    if v.startswith('"') and v.endswith('"') and len(v) >= 2:
        return bytes(v[1:-1], 'utf-8').decode('unicode_escape')
    if v.startswith("'") and v.endswith("'") and len(v) >= 2:
        return v[1:-1]
    if v in ('true', 'false'):
        return v == 'true'
    if v.startswith('[') and v.endswith(']'):
        return [_value(x) for x in _split_top(v[1:-1])]
    if v.startswith('{') and v.endswith('}'):
        d = {}
        for part in _split_top(v[1:-1]):
            m = _KEY.match(part)
            if not m:
                raise TOMLError(f'bad inline table item {part!r}')
            d[m.group(1)] = _value(m.group(2))
        return d
    try:
        return int(v.replace('_', ''), 0)
    except ValueError:
        raise TOMLError(f'unsupported value {v!r}') from None


def loads_fallback(text):
    root, cur = {}, None
    cur = root
    pending = ''
    for raw in text.splitlines():
        line = _strip_comment(raw)
        if pending:
            line = pending + ' ' + line
            pending = ''
        if not line:
            continue
        if line.count('[') > line.count(']') and '=' in line:
            pending = line           # a multi-line array
            continue
        if line.startswith('[['):
            name = line.strip('[] ')
            root.setdefault(name, []).append({})
            cur = root[name][-1]
        elif line.startswith('['):
            name = line.strip('[] ')
            cur = root.setdefault(name, {})
        else:
            m = _KEY.match(line)
            if not m:
                raise TOMLError(f'cannot parse {raw!r}')
            cur[m.group(1)] = _value(m.group(2))
    return root

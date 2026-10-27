# Copied from grail-compat src/grail/ by tools/sync_to_cpython_grail.py; do not edit here.
"""grail.mutate: edit an immutable value through a thread-confined draft.

The same file serves the native implementation (cpython-grail's
Lib/grail/_mutate.py, copied by tools/sync_to_cpython_grail.py) and
grail-compat.

``mutate(value)`` returns a draft of a record, ``PVector`` or ``PMap``.
Reading a field or item that holds another record, ``PVector`` or ``PMap``
returns a draft of it, created on first use, so nested state can be edited
in place: ``s.tasks[i].queue += (p,)``.  ``finish(draft)`` builds the new
value bottom-up with ``grail.replace``, ``PVector.set`` and ``PMap``
transients, sharing everything that was not assigned; a draft with no
assignments finishes to its original object.  The original is never
changed.  Record drafts have only their fields, no methods, so the draft
is finished by the ``finish`` function rather than a method.

Like a transient, a draft belongs to the thread that made it.  Leaving a
``with`` block seals it (only ``finish`` remains); an exception aborts it.
Drafts are not records: ``match``, ``==`` and ``isinstance`` see the draft,
not the value, so read through ``original`` or finish first.
"""

import _thread
from _thread import get_ident as _get_ident

from . import PMap, PVector, record_fields, replace

try:
    from ._core import _is_record_type           # grail-compat
except ImportError:
    from _grail import _is_record_type           # cpython-grail

__all__ = ["mutate", "finish", "original"]

_OPEN, _SEALED, _DONE = 0, 1, 2
_MISSING = object()
_DELETED = object()
_field_names = {}                # record type -> frozenset of field names


def _names(cls):
    names = _field_names.get(cls)
    if names is None:
        names = _field_names[cls] = frozenset(f.name for f in record_fields(cls))
    return names


class _Root:
    __slots__ = ("thread", "state", "draft", "result")

    def __init__(self):
        self.thread = _thread.get_ident()
        self.state = _OPEN
        self.draft = None
        self.result = _MISSING


def _fail(root):
    if root.state != _OPEN:
        raise RuntimeError("draft is no longer open")
    raise RuntimeError("draft used from another thread")


def _check(root):
    if root.state or root.thread != _get_ident():
        _fail(root)


# Never drafted; skips the record-type test for the common field values.
_PLAIN = frozenset((int, float, str, bytes, bool, tuple, type(None)))


def _draft(value, root):
    """A child draft for a draftable value, else the value itself."""
    cls = type(value)
    if cls in _PLAIN:
        return value
    if cls is PVector:
        return _VectorDraft(value, root)
    if cls is PMap:
        return _MapDraft(value, root)
    if _is_record_type(cls):
        return _RecordDraft(value, root)
    return value


def _store(value, root):
    if isinstance(value, _Draft) and _root(value) is not root:
        raise ValueError("draft belongs to another mutate()")
    return value


def _build(value, memo):
    """The finished value for a draft (once per draft), else value."""
    if not isinstance(value, _Draft):
        return value
    key = id(value)
    built = memo.get(key)
    if built is None:
        built = memo[key] = value._build(memo)
    return built


def _root(draft):
    return draft._d_root


class _Draft:
    __slots__ = ("_d_base", "_d_root", "_d_changes")

    def __init__(self, base, root):
        _set_base(self, base)
        _set_root(self, root)
        _set_changes(self, {})

    # Context management applies to the root draft.
    def __enter__(self):
        root = _root(self)
        _check(root)
        if root.draft is not self:
            raise RuntimeError("only the root draft is a context manager")
        return self

    def __exit__(self, exc, value, tb):
        root = _root(self)
        if root.state == _OPEN:
            root.state = _SEALED if exc is None else _DONE
        return False

    def __repr__(self):
        return f"<draft of {self._d_base!r}>"

    __hash__ = None

    def __reduce_ex__(self, protocol):
        raise TypeError("cannot pickle a draft")

    def __copy__(self):
        raise TypeError("cannot copy a draft")

    __deepcopy__ = __copy__


_set_base = _Draft._d_base.__set__
_set_root = _Draft._d_root.__set__
_set_changes = _Draft._d_changes.__set__


class _RecordDraft(_Draft):
    __slots__ = ()

    # Slot reads find the slot descriptors; only missing names (fields)
    # reach __getattr__.
    def __getattr__(self, name):
        root = self._d_root
        if root.state or root.thread != _get_ident():
            _fail(root)
        changes = self._d_changes
        value = changes.get(name, _MISSING)
        if value is not _MISSING:
            return value
        base = self._d_base
        if name not in _names(type(base)):
            raise AttributeError(
                f"{type(base).__name__!r} record has no field {name!r}")
        value = _draft(getattr(base, name), root)
        if isinstance(value, _Draft):
            changes[name] = value
        return value

    def __setattr__(self, name, value):
        root = self._d_root
        if root.state or root.thread != _get_ident():
            _fail(root)
        base = self._d_base
        if name not in _names(type(base)):
            raise AttributeError(
                f"{type(base).__name__!r} record has no field {name!r}")
        if isinstance(value, _Draft) and value._d_root is not root:
            raise ValueError("draft belongs to another mutate()")
        self._d_changes[name] = value

    def __delattr__(self, name):
        raise AttributeError("cannot delete a record field")

    def _build(self, memo):
        base = self._d_base
        changed = {}
        for name, value in self._d_changes.items():
            if isinstance(value, _Draft):
                value = _build(value, memo)
            if value is not getattr(base, name):
                changed[name] = value
        return replace(base, **changed) if changed else base


class _VectorDraft(_Draft):
    __slots__ = ("_d_tail",)     # appended items, after the base's length

    def __init__(self, base, root):
        _Draft.__init__(self, base, root)
        self._d_tail = []

    def _index(self, key):
        n = len(self._d_base) + len(self._d_tail)
        if key.__class__ is not int:
            try:
                key = key.__index__()
            except AttributeError:
                raise TypeError("draft indices must be integers") from None
        if key < 0:
            key += n
        if not 0 <= key < n:
            raise IndexError("draft index out of range")
        return key

    def __len__(self):
        _check(self._d_root)
        return len(self._d_base) + len(self._d_tail)

    def __getitem__(self, key):
        root = self._d_root
        _check(root)
        i = self._index(key)
        base = self._d_base
        if i >= len(base):
            tail = self._d_tail
            value = tail[i - len(base)]
            if not isinstance(value, _Draft):
                value = _draft(value, root)
                tail[i - len(base)] = value
            return value
        changes = self._d_changes
        value = changes.get(i, _MISSING)
        if value is _MISSING:
            value = _draft(base[i], root)
            if isinstance(value, _Draft):
                changes[i] = value
        return value

    def __setitem__(self, key, value):
        root = self._d_root
        _check(root)
        i = self._index(key)
        value = _store(value, root)
        base = self._d_base
        if i >= len(base):
            self._d_tail[i - len(base)] = value
        else:
            self._d_changes[i] = value

    def append(self, value):
        root = self._d_root
        _check(root)
        self._d_tail.append(_store(value, root))

    def extend(self, iterable):
        root = self._d_root
        _check(root)
        self._d_tail.extend(_store(v, root) for v in iterable)

    def _build(self, memo):
        base = self._d_base
        result = base
        for i, value in self._d_changes.items():
            value = _build(value, memo)
            if value is not base[i]:
                result = result.set(i, value)
        tail = self._d_tail
        if tail:
            if len(tail) == 1:
                result = result.append(_build(tail[0], memo))
            else:
                result = result + PVector([_build(v, memo) for v in tail])
        return result


class _MapDraft(_Draft):
    __slots__ = ()

    def _get(self, key):
        root = self._d_root
        _check(root)
        changes = self._d_changes
        value = changes.get(key, _MISSING)
        if value is _MISSING:
            value = self._d_base.get(key, _MISSING)
            if value is _MISSING:
                return value
            value = _draft(value, root)
            if isinstance(value, _Draft):
                changes[key] = value
        elif value is _DELETED:
            return _MISSING
        return value

    def __getitem__(self, key):
        value = self._get(key)
        if value is _MISSING:
            raise KeyError(key)
        return value

    def get(self, key, default=None):
        value = self._get(key)
        return default if value is _MISSING else value

    def __contains__(self, key):
        return self._get(key) is not _MISSING

    def __setitem__(self, key, value):
        root = self._d_root
        _check(root)
        self._d_changes[key] = _store(value, root)

    def __delitem__(self, key):
        if self._get(key) is _MISSING:
            raise KeyError(key)
        self._d_changes[key] = _DELETED

    def __len__(self):
        _check(self._d_root)
        base = self._d_base
        n = len(base)
        for key, value in self._d_changes.items():
            if value is _DELETED:
                n -= key in base
            elif key not in base:
                n += 1
        return n

    def _build(self, memo):
        base = self._d_base
        edits = []
        for key, value in self._d_changes.items():
            old = base.get(key, _MISSING)
            if value is _DELETED:
                if old is not _MISSING:
                    edits.append((key, _DELETED))
            else:
                value = _build(value, memo)
                if value is not old:
                    edits.append((key, value))
        if not edits:
            return base
        with base.mutate() as m:
            for key, value in edits:
                if value is _DELETED:
                    del m[key]
                else:
                    m[key] = value
        return m.finish()


def mutate(value, /):
    """Return a draft of a record, PVector or PMap; see finish()."""
    if isinstance(value, _Draft):
        raise TypeError("mutate() requires a value, not a draft")
    root = _Root()
    draft = _draft(value, root)
    if not isinstance(draft, _Draft):
        raise TypeError("mutate() requires a Grail record, PVector or PMap")
    root.draft = draft
    return draft


def finish(draft, /):
    """Return the value a root draft describes and close the draft.

    Calling it again returns the same value.  An aborted draft has no value.
    """
    if not isinstance(draft, _Draft):
        raise TypeError("finish() requires a draft from mutate()")
    root = _root(draft)
    if root.draft is not draft:
        raise RuntimeError("finish() requires the root draft")
    if root.thread != _thread.get_ident():
        raise RuntimeError("draft used from another thread")
    if root.result is _MISSING:
        if root.state == _DONE:
            raise RuntimeError("draft was aborted")
        root.state = _DONE
        root.result = _build(draft, {})
    return root.result


def original(draft, /):
    """The value a draft was made from (its edits are not applied)."""
    if not isinstance(draft, _Draft):
        raise TypeError("original() requires a draft from mutate()")
    return draft._d_base

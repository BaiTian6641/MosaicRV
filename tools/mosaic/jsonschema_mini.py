"""Minimal JSON Schema (draft-07 subset) validator.

Deliberately dependency-free: the project plan fixes "Python 标准库编排", so the
config layer must validate with nothing but the standard library. Only the
keywords actually used by config/schema/*.json are implemented; anything else in
a schema is reported as an unsupported keyword instead of being silently ignored,
so a schema can never appear to be enforced when it is not.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional, Tuple

_ANNOTATIONS = frozenset(
    {"$schema", "$id", "title", "description", "$comment", "examples", "default"}
)

_SUPPORTED = frozenset(
    {
        "type",
        "enum",
        "const",
        "properties",
        "required",
        "additionalProperties",
        "items",
        "minItems",
        "maxItems",
        "uniqueItems",
        "minLength",
        "maxLength",
        "minimum",
        "maximum",
        "pattern",
        "oneOf",
        "anyOf",
        "allOf",
        "not",
        "$ref",
        "propertyNames",
    }
)

Error = Tuple[str, str]


class SchemaError(Exception):
    """Raised when the schema itself is unusable (not the document)."""


def _type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "null":
        return value is None
    raise SchemaError("unsupported type keyword: %r" % expected)


def _resolve(root: Dict[str, Any], ref: str) -> Dict[str, Any]:
    if not ref.startswith("#/"):
        raise SchemaError("only local refs are supported, got %r" % ref)
    node: Any = root
    for token in ref[2:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if not isinstance(node, dict) or token not in node:
            raise SchemaError("unresolvable ref %r" % ref)
        node = node[token]
    if not isinstance(node, dict):
        raise SchemaError("ref %r does not point at a schema object" % ref)
    return node


def _check(schema: Any, value: Any, root: Dict[str, Any], path: str) -> List[Error]:
    if schema is True or schema == {}:
        return []
    if schema is False:
        return [(path, "schema forbids any value here")]
    if not isinstance(schema, dict):
        raise SchemaError("schema at %s is not an object" % (path or "/"))

    unknown = set(schema) - _SUPPORTED - _ANNOTATIONS
    if unknown:
        raise SchemaError(
            "unsupported schema keyword(s) at %s: %s"
            % (path or "/", ", ".join(sorted(unknown)))
        )

    errors: List[Error] = []

    if "$ref" in schema:
        errors.extend(_check(_resolve(root, schema["$ref"]), value, root, path))

    if "type" in schema:
        expected = schema["type"]
        options = expected if isinstance(expected, list) else [expected]
        if not any(_type_matches(value, opt) for opt in options):
            errors.append(
                (path, "expected type %s, got %s" % ("|".join(options), type(value).__name__))
            )
            return errors

    if "enum" in schema and value not in schema["enum"]:
        errors.append((path, "value %r not in enum %r" % (value, schema["enum"])))
    if "const" in schema and value != schema["const"]:
        errors.append((path, "value %r must equal %r" % (value, schema["const"])))

    for keyword in ("allOf",):
        if keyword in schema:
            for i, sub in enumerate(schema[keyword]):
                errors.extend(_check(sub, value, root, "%s/allOf[%d]" % (path, i)))

    for keyword in ("oneOf", "anyOf"):
        if keyword in schema:
            branches = [_check(sub, value, root, "%s/%s[%d]" % (path, keyword, i))
                        for i, sub in enumerate(schema[keyword])]
            passed = [i for i, errs in enumerate(branches) if not errs]
            if keyword == "oneOf" and len(passed) != 1:
                errors.append((path, "oneOf matched %d branches, expected exactly 1" % len(passed)))
            if keyword == "anyOf" and not passed:
                errors.append((path, "anyOf matched no branch"))

    if "not" in schema and not _check(schema["not"], value, root, path):
        errors.append((path, "value matches a forbidden schema"))

    if isinstance(value, str):
        if "pattern" in schema and re.search(schema["pattern"], value) is None:
            errors.append((path, "value %r does not match pattern %r" % (value, schema["pattern"])))
        if "minLength" in schema and len(value) < schema["minLength"]:
            errors.append((path, "string is shorter than minLength %d" % schema["minLength"]))
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            errors.append((path, "string is longer than maxLength %d" % schema["maxLength"]))

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append((path, "value %r < minimum %r" % (value, schema["minimum"])))
        if "maximum" in schema and value > schema["maximum"]:
            errors.append((path, "value %r > maximum %r" % (value, schema["maximum"])))

    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append((path, "array has %d items, minItems is %d" % (len(value), schema["minItems"])))
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append((path, "array has %d items, maxItems is %d" % (len(value), schema["maxItems"])))
        if schema.get("uniqueItems"):
            seen = []
            for item in value:
                if item in seen:
                    errors.append((path, "array items are not unique: %r" % (item,)))
                    break
                seen.append(item)
        if "items" in schema:
            for i, item in enumerate(value):
                errors.extend(_check(schema["items"], item, root, "%s[%d]" % (path, i)))

    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append((path, "missing required property %r" % key))
        props = schema.get("properties", {})
        if "propertyNames" in schema:
            for key in value:
                errors.extend(_check(schema["propertyNames"], key, root, "%s/<key %s>" % (path, key)))
        for key, sub in props.items():
            if key in value:
                errors.extend(_check(sub, value[key], root, "%s.%s" % (path, key)))
        if "additionalProperties" in schema:
            extra = sorted(set(value) - set(props))
            addl = schema["additionalProperties"]
            for key in extra:
                if addl is False:
                    errors.append((path, "additional property %r is not allowed" % key))
                else:
                    errors.extend(_check(addl, value[key], root, "%s.%s" % (path, key)))

    return errors


def validate(document: Any, schema: Dict[str, Any]) -> List[Error]:
    """Validate `document` against `schema`; returns a list of (json_path, message)."""
    return _check(schema, document, schema, "$")


def require_supported(schema: Any, path: str = "$") -> Optional[str]:
    """Return an error string if the schema tree uses an unimplemented keyword."""
    try:
        _check(schema, None, schema, path)
    except SchemaError as exc:
        return str(exc)
    return None
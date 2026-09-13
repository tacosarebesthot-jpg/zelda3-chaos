#!/usr/bin/env python3
"""Phase 0 data-extraction tool: run ALttPDoorRandomizer's full generation
pipeline and dump the separable LOGIC layer as JSON for the zelda3 C engine.

The dump is read-only with respect to the reference repo: nothing inside the
ALttPDoorRandomizer clone is modified.  The only import shim lives in
./stubs/bps (inert stub for the pip `bps` patch library, which Rom.py insists
on importing even when --suppress_rom is passed).

Usage (from anywhere):
    python dump_logic.py [--repo PATH] [--out PATH] [--seed N [N ...]]
                         [--players 1] [--logic noglitches] [--mode open]
                         [--goal ganon] [--door_shuffle vanilla]
                         [--shuffle vanilla] [--loglevel error]
                         [--setting key=value ...] [--with-fill-data]

Defaults: repo = ../ALttPDoorRandomizer (sibling of this script's parent),
out = ./out, seeds = 1234 5678.

Outputs per seed (out/seed_<N>/):
    regions.json edges.json locations.json items.json rules.json meta.json
    bosses.json  (the finished boss placement, see dump_bosses)
    drops.json   (the enemy behind every "... Key Drop", see dump_drops)
    pots.json    (the pot behind every "... Pot Key", see dump_pots)
    entrances.json (the door / exit / hole wiring, see dump_entrances)
    bonks.json   (the 42 bonk / tree-pull prize slots, see dump_bonks)
With --with-fill-data, additionally (phase-1 schema-gap closures):
    keydoors.json shops.json barriers.json vanilla_locations.json groups.json

Rule IR
-------
ALttPDoorRandomizer attaches most access rules as plain lambdas
(`spot.access_rule`), so this tool decompiles them into a typed node tree:

    {"op": "and"|"or", "nodes": [...]}       boolean composition
    {"op": "not",       "nodes": [one]}      negation
    {"op": "item",      "item", "count"}     state.has(item, p, count)
    {"op": "item_count_sum", "items", "count"}  sum of item_count() >= n
    {"op": "bottles",   "count"}             bottle_count() >= count
    {"op": "crystals"|"pendants"|"bosses"|"hearts", "count", ...}
    {"op": "reachable", "spot", "spot_type"} state.can_reach(...)
    {"op": "unlimited", "item"}              state.can_buy_unlimited(item)
    {"op": "extend_magic", "magic", "fullrefill"}
    {"op": "barrier",   "region", "barrier"} crystal-switch barrier state
    {"op": "not_bunny", "region"}            state.is_not_bunny(...)
    {"op": "reach_light_world"|"reach_dark_world"|"everything"}
    {"op": "door_open", "door"}              state.is_door_open(...)
    {"op": "location_check", "item", "location"}
    {"op": "small_key_door", "door", "dungeon", "variants"}
    {"op": "static",    "value"}             constant-folded (per this profile)
    {"op": "opaque",    "reason", "python"}  UNTRANSLATABLE fallback marker

Lambdas are translated by AST decompilation with:
  * inlining of CollectionState/Region/Boss helper methods (recursive, with
    `self` bound to the real object),
  * constant folding of world data (settings, tile swaps, dungeon limits),
    which is sound because the dump is for one fixed settings profile,
  * direct nodes for the fork's own typed Rule objects (source/logic/Rule.py)
    found in `spot.verbose_rule`, including their DNF via get_requirements().
Anything that resists flattening is kept as an "opaque" marker carrying the
original python source; counts are reported in rules.json stats.
"""

from __future__ import annotations

import functools
import argparse
import ast
import hashlib
import inspect
import json
import logging
import os
import subprocess
import sys
import textwrap
import types

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_REPO = os.path.abspath(
    os.path.join(HERE, "..", "..", "ALttPDoorRandomizer"))
DEFAULT_OUT = os.path.join(HERE, "out")

log = logging.getLogger("dump_logic")


# ---------------------------------------------------------------------------
# Environment setup
# ---------------------------------------------------------------------------

def setup_repo(repo: str) -> None:
    """Put the stubs dir and the reference repo on sys.path and chdir into the
    repo (parse_cli reads resources/app/cli/args.json relative to cwd)."""
    stubs = os.path.join(HERE, "stubs")
    if not os.path.isdir(repo):
        raise SystemExit(f"reference repo not found: {repo}")
    for p in (stubs, repo):
        if p not in sys.path:
            sys.path.insert(0, p)
    os.chdir(repo)


def git_hash(repo: str) -> str:
    try:
        out = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=15)
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception as e:
        log.warning("git rev-parse failed: %s", e)
    return "unknown"


# ---------------------------------------------------------------------------
# Folding helpers
# ---------------------------------------------------------------------------

class _Unsupported:
    def __repr__(self):
        return "<unsupported>"


UNSUPPORTED = _Unsupported()
SKIP_ARG = object()      # marks the `state` positional argument


class Obj:
    """Wraps a live python object seen during translation (world, region,
    rule, ...).  Attribute access on it is real getattr."""
    __slots__ = ("obj",)

    def __init__(self, obj):
        self.obj = obj

    def __repr__(self):
        return f"Obj({type(self.obj).__name__})"


class StateRef:
    __slots__ = ()


STATE = StateRef()

# attributes on CollectionState that must never be folded (run-time state)
STATE_DENYLIST = {"prog_items", "forced_keys", "reachable_regions",
                  "blocked_connections", "locations_checked", "events",
                  "path", "stale", "door_counter", "reached_doors",
                  "opened_doors", "dungeons_to_check"}


def is_prim(v) -> bool:
    if isinstance(v, (bool, int, float, str)):
        return True
    if isinstance(v, (tuple, list)):
        return len(v) <= 8 and all(is_prim(x) for x in v)
    return False


def foldable_value(v) -> bool:
    """Final values that are safe to bake in.  Containers of objects or dicts
    are refused (they would fold to meaningless truthiness)."""
    if isinstance(v, (dict, set, frozenset)):
        return False
    if isinstance(v, (list, tuple)):
        return all(is_prim(x) for x in v) and len(v) <= 8
    if isinstance(v, (bool, int, float, str)) or v is None:
        return True
    return False


def static_node(value):
    return {"op": "static", "value": bool(value)}


def canonical(node) -> str:
    return json.dumps(node, sort_keys=True, separators=(",", ":"))


def lambda_source(fn) -> str:
    try:
        return textwrap.shorten(
            textwrap.dedent(inspect.getsource(fn)).strip(), 400)
    except Exception:
        code = getattr(fn, "__code__", None)
        return getattr(code, "co_name", repr(fn))


def count_opaque(node) -> int:
    if not isinstance(node, dict):
        return 0
    n = 1 if node.get("op") == "opaque" else 0
    for v in node.values():
        if isinstance(v, dict):
            n += count_opaque(v)
        elif isinstance(v, list):
            for x in v:
                n += count_opaque(x)
    return n


def keyrule_str(k):
    name = getattr(k, "name", None)
    if name:
        return name
    if isinstance(k, tuple):
        return [str(x) for x in k]
    return str(k)


def simplify(node) -> dict:
    """Recursively fold static constants through and/or/not trees and drop
    duplicate siblings."""
    if not isinstance(node, dict):
        return node
    op = node.get("op")
    if op in ("and", "or"):
        return RuleDumper._bool_combine(
            op, [simplify(x) for x in node["nodes"]])
    if op == "not":
        kid = simplify(node["nodes"][0])
        if kid.get("op") == "static":
            return static_node(not kid["value"])
        return {"op": "not", "nodes": [kid]}
    if op in ("typed", "boss") and isinstance(node.get("nodes"), list):
        out = dict(node)
        out["nodes"] = [simplify(x) for x in node["nodes"]]
        return out
    return node


def _freeze_args(binds):
    if not binds:
        return None
    out = []
    for k, v in sorted(binds.items()):
        out.append((k, v if is_prim(v) else id(v)))
    return tuple(out)


def _callable_name(target, func):
    if target is not None:
        return getattr(target, "__qualname__", None) or \
            getattr(target, "__name__", "?")
    return ast.unparse(func) if isinstance(func, ast.AST) else "?"


def func_of_call(node: ast.Call):
    return node.func


# ---------------------------------------------------------------------------
# Rule IR: lambda decompiler -> typed node tree
# ---------------------------------------------------------------------------

class RuleStats:
    def __init__(self):
        self.total_spots = 0
        self.trivial_true = 0
        self.flattened = 0
        self.fallback = 0
        self.opaque_nodes = 0
        self.typed_ir_spots = 0
        self.untranslated = []

    def as_dict(self):
        flat_total = self.flattened + self.fallback
        return {
            "rule_spots": self.total_spots,
            "trivial_always_true": self.trivial_true,
            "nontrivial_rules": flat_total,
            "flattened": self.flattened,
            "fallback_markers": self.fallback,
            "flatten_pct": round(100.0 * self.flattened / flat_total, 2) if flat_total else 100.0,
            "opaque_node_instances": self.opaque_nodes,
            "spots_with_typed_ir": self.typed_ir_spots,
        }


class RuleDumper:
    """Translates a world's access_rule lambdas (and the fork's typed Rule
    objects) into the JSON rule IR consumed by the C engine."""

    MAX_EXPR_DEPTH = 48
    MAX_INLINE_DEPTH = 48

    def __init__(self, world):
        self.world = world
        from BaseClasses import CollectionState
        self._CollectionState = CollectionState
        self._state_probe = CollectionState(world)
        self.registry = {}
        self.sources = {}
        self.stats = RuleStats()
        self._cache = {}
        import source.logic.Rule as _RuleMod
        import Rules as _RulesMod
        self._special_globals = {
            id(_RuleMod.eval_location_main): self._loc_check_from_call,
            id(_RuleMod.eval_small_key_door_main): self._skd_from_call,
            # Rules.py has its own variants for partial/strict key-door logic
            id(_RulesMod.eval_small_key_door_main): self._skd_from_call,
            id(_RulesMod.eval_small_key_door_partial_main): self._skd_from_call,
            id(_RulesMod.eval_small_key_door_strict_main): self._skd_from_call,
            id(_RulesMod.eval_alternative_crystal_main): self._skd_from_call,
        }

    # -- registry -----------------------------------------------------------

    def register(self, node, src=None) -> str:
        key = hashlib.sha1(canonical(node).encode()).hexdigest()[:10]
        rid = "r" + key
        self.registry.setdefault(rid, node)
        if src and rid not in self.sources:
            self.sources[rid] = src
        return rid

    def rule_ref(self, fn, spot_desc: str) -> str:
        self.stats.total_spots += 1
        node = simplify(self.translate_callable(fn))
        opaque = count_opaque(node)
        trivial = node.get("op") == "static" and node.get("value") is True
        if trivial:
            self.stats.trivial_true += 1
        elif opaque == 0:
            self.stats.flattened += 1
        else:
            self.stats.fallback += 1
            self.stats.untranslated.append(
                (spot_desc, f"{opaque} opaque node(s)", lambda_source(fn)))
        self.stats.opaque_nodes += opaque
        return self.register(node, src=lambda_source(fn))

    def typed_ir_ref(self, rule) -> str:
        node = simplify(self.serialize_typed_rule(rule))
        self.stats.typed_ir_spots += 1
        return self.register(node)

    # -- callable entry -----------------------------------------------------

    def translate_callable(self, fn, depth=0, bind_self=None, bind_args=None) -> dict:
        # functools.partial has no source: translate the wrapped function with
        # the partial's arguments bound (underworld_glitches_rules bomb_clip).
        if isinstance(fn, functools.partial):
            ba = dict(fn.keywords or {})
            f = fn.func
            code = getattr(f, "__code__", None)
            if code is not None and fn.args:
                params = list(code.co_varnames[:code.co_argcount])
                if params and params[0] == "self":
                    params = params[1:]
                for name, v in zip(params, fn.args):
                    ba.setdefault(name, v)
            if bind_args:
                ba.update(bind_args)
            fn, bind_args = f, ba
        stack = self.__dict__.setdefault("_fn_stack", [])
        stack.append(_callable_name(fn, None))
        try:
            return self._translate_callable_inner(fn, depth, bind_self, bind_args)
        finally:
            stack.pop()

    def _translate_callable_inner(self, fn, depth=0, bind_self=None, bind_args=None) -> dict:
        if not callable(fn):
            return static_node(bool(fn))
        if depth > self.MAX_INLINE_DEPTH:
            return self._opaque_any(fn, "inlining depth exceeded")
        cache_key = (id(fn), depth, id(bind_self), _freeze_args(bind_args))
        if cache_key in self._cache:
            return self._cache[cache_key]
        self._cache[cache_key] = {"op": "opaque", "reason": "recursion",
                                  "python": lambda_source(fn)}  # cycle guard

        try:
            src = textwrap.dedent(inspect.getsource(fn))
            tree = ast.parse(src)
        except (OSError, TypeError, SyntaxError):
            node = self._opaque_any(fn, "no source available")
            self._cache[cache_key] = node
            return node

        ctx = {}
        if fn.__closure__:
            for name, cell in zip(fn.__code__.co_freevars, fn.__closure__):
                try:
                    ctx[name] = cell.cell_contents
                except ValueError:
                    node = self._opaque_any(fn, "empty closure cell")
                    self._cache[cache_key] = node
                    return node
        if bind_self is not None:
            ctx["self"] = Obj(bind_self)
        if bind_args:
            ctx.update(bind_args)

        fdefs = [n for n in ast.walk(tree)
                 if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
        lambdas = [n for n in ast.walk(tree) if isinstance(n, ast.Lambda)]

        if fdefs and not lambdas:
            node = self._translate_body(fdefs[0].body, ctx, fn, depth)
        elif lambdas:
            first = fn.__code__.co_firstlineno
            cands = [n for n in lambdas if n.lineno == first] or lambdas
            lam = cands[0] if len(cands) == 1 else self._pick_lambda(cands, fn)
            node = self._opaque_any(fn, "lambda selection failed") if lam is None \
                else self._translate_expr(lam.body, ctx, fn, depth)
        else:
            node = self._opaque_any(fn, "no function body found")

        self._cache[cache_key] = node
        return node

    @staticmethod
    def _pick_lambda(cands, fn):
        consts = {c for c in fn.__code__.co_consts if isinstance(c, str)}
        best, best_score = None, -1
        for lam in cands:
            strs = {n.value for n in ast.walk(lam)
                    if isinstance(n, ast.Constant) and isinstance(n.value, str)}
            score = len(strs & consts) + (1 if strs <= consts else 0)
            if score > best_score:
                best, best_score = lam, score
        return best

    # -- statements -----------------------------------------------------------

    def _opaque_any(self, thing, reason):
        if os.environ.get("DUMP_DEBUG_OPAQUE"):
            print("OPAQUE[%s] in %s" % (reason, " > ".join(self.__dict__.get("_fn_stack", []))[-300:]), flush=True)
        try:
            if isinstance(thing, ast.AST):
                py = ast.unparse(thing)
            else:
                py = lambda_source(thing) or repr(thing)
        except Exception:
            py = repr(thing)
        return {"op": "opaque", "reason": reason, "python": py}

    def _translate_body(self, stmts, ctx, fn, depth):
        """Translate simple bodies: return / assign / if-returns chains.
        Multiple returns combine as or()."""
        local = dict(ctx)
        result = None
        for st in stmts:
            if isinstance(st, ast.Return):
                node = self._translate_expr(st.value, local, fn, depth) \
                    if st.value is not None else static_node(True)
                result = node if result is None else \
                    {"op": "or", "nodes": [result, node]}
            elif isinstance(st, ast.Assign) and len(st.targets) == 1 and \
                    isinstance(st.targets[0], ast.Name):
                val = self._eval(st.value, local, fn, depth)
                if val is UNSUPPORTED:
                    return self._opaque_any(st, "unsupported assignment")
                local[st.targets[0].id] = val
            elif isinstance(st, ast.If):
                test = self._translate_expr(st.test, local, fn, depth)
                then_node = self._translate_body(st.body, local, fn, depth)
                else_node = self._translate_body(st.orelse, local, fn, depth) \
                    if st.orelse else static_node(False)
                node = {"op": "or", "nodes": [
                    {"op": "and", "nodes": [test, then_node]},
                    {"op": "and", "nodes":
                        [{"op": "not", "nodes": [test]}, else_node]},
                ]}
                result = node if result is None else \
                    {"op": "or", "nodes": [result, node]}
            elif isinstance(st, ast.Pass):
                continue
            else:
                return self._opaque_any(
                    st, f"unsupported statement: {type(st).__name__}")
        return result if result is not None else static_node(True)

    # -- raw value evaluation (for argument positions) --------------------------

    def _eval(self, node, ctx, fn, depth):
        """Evaluate an expression to a raw python value, or UNSUPPORTED."""
        if depth > self.MAX_EXPR_DEPTH:
            return UNSUPPORTED
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Name):
            nid = node.id
            if nid == "state":
                return STATE
            if nid in ctx:
                return ctx[nid]
            g = getattr(fn, "__globals__", {})
            if nid in g:
                return g[nid]
            return UNSUPPORTED
        if isinstance(node, ast.Attribute):
            base = self._resolve_object(node.value, ctx, fn, depth + 1)
            if base is UNSUPPORTED or base is None:
                return UNSUPPORTED
            b = base.obj if isinstance(base, Obj) else base
            if isinstance(base, StateRef):
                if node.attr == "world":
                    return Obj(self.world)
                if node.attr in STATE_DENYLIST:
                    return UNSUPPORTED
                b = self._state_probe
            if b is None:
                return UNSUPPORTED
            # a CollectionState reached as a plain object (self inside an
            # inlined state method): .world is the dump's world, same as the
            # StateRef branch above.  The probe state has no usable .world,
            # which left every self.world.<setting>[player] lookup opaque.
            if node.attr == "world" and (b is self._state_probe or
                                         isinstance(b, self._CollectionState)):
                return Obj(self.world)
            v = getattr(b, node.attr, UNSUPPORTED)
            if v is UNSUPPORTED:
                return UNSUPPORTED
            if isinstance(b, type(self._state_probe)) and node.attr in STATE_DENYLIST:
                return UNSUPPORTED
            return v
        if isinstance(node, ast.Subscript):
            base = self._resolve_object(node.value, ctx, fn, depth + 1)
            if base is UNSUPPORTED or base is None:
                return UNSUPPORTED
            b = base.obj if isinstance(base, Obj) else base
            if isinstance(base, StateRef):
                return UNSUPPORTED
            key = self._eval(node.slice, ctx, fn, depth + 1)
            if key is UNSUPPORTED or key is STATE or not is_prim(key):
                return UNSUPPORTED
            try:
                return b[key]
            except Exception:
                return UNSUPPORTED
        if isinstance(node, ast.BinOp):
            import operator as op_mod
            fns = {ast.Add: op_mod.add, ast.Sub: op_mod.sub,
                   ast.Mult: op_mod.mul, ast.FloorDiv: op_mod.floordiv,
                   ast.Mod: op_mod.mod}
            f = fns.get(type(node.op))
            if f is None:
                return UNSUPPORTED
            a = self._eval(node.left, ctx, fn, depth + 1)
            b = self._eval(node.right, ctx, fn, depth + 1)
            if a is UNSUPPORTED or b is UNSUPPORTED or a is STATE or b is STATE:
                return UNSUPPORTED
            try:
                return f(a, b)
            except Exception:
                return UNSUPPORTED
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            v = self._eval(node.operand, ctx, fn, depth + 1)
            if isinstance(v, (int, float)):
                return -v
            return UNSUPPORTED
        if isinstance(node, (ast.Tuple, ast.List)):
            vals = []
            for el in node.elts:
                v = self._eval(el, ctx, fn, depth + 1)
                if v is UNSUPPORTED or v is STATE:
                    return UNSUPPORTED
                vals.append(v)
            return tuple(vals)
        if isinstance(node, ast.Call):
            return self._eval_call(node, ctx, fn, depth)
        return UNSUPPORTED

    def _eval_call(self, node: ast.Call, ctx, fn, depth):
        """Pure calls: int()/bool(), world methods, global helper functions."""
        args, kwargs = [], {}
        for a in node.args:
            v = self._eval(a, ctx, fn, depth + 1)
            if v is UNSUPPORTED:
                return UNSUPPORTED
            args.append(v)
        for kw in node.keywords:
            v = self._eval(kw.value, ctx, fn, depth + 1)
            if v is UNSUPPORTED:
                return UNSUPPORTED
            kwargs[kw.arg] = v
        target = self._resolve_function(node.func, ctx, fn)
        if target is None:
            return UNSUPPORTED
        if target in (int, bool, str, float, len):
            try:
                return target(*args, **kwargs)
            except Exception:
                return UNSUPPORTED
        # method call on a folded object (e.g. world.is_tile_swapped(..))
        if isinstance(node.func, ast.Attribute):
            base = self._resolve_object(node.func.value, ctx, fn, depth + 1)
            if base is UNSUPPORTED or base is None or isinstance(base, StateRef):
                return UNSUPPORTED
            b = base.obj if isinstance(base, Obj) else base
            if isinstance(b, type(self._state_probe)):
                return UNSUPPORTED  # never call state methods for values
            try:
                v = target(*args, **kwargs)
            except Exception:
                return UNSUPPORTED
            return v
        if isinstance(target, types.FunctionType):
            # pure global helper taking primitives
            try:
                return target(*args, **kwargs)
            except Exception:
                return UNSUPPORTED
        return UNSUPPORTED

    def _resolve_object(self, node, ctx, fn, depth):
        """Resolve an expression to the underlying python value (or Obj wrapper,
        or STATE sentinel). UNSUPPORTED when it cannot be resolved."""
        if isinstance(node, ast.Name):
            return self._eval(node, ctx, fn, depth)
        if isinstance(node, (ast.Attribute, ast.Subscript, ast.Call)):
            return self._eval(node, ctx, fn, depth)
        if isinstance(node, ast.Constant):
            return node.value
        return UNSUPPORTED

    def _resolve_function(self, func, ctx, fn):
        if isinstance(func, ast.Name):
            nid = func.id
            v = ctx.get(nid)
            if v is None:
                g = getattr(fn, "__globals__", {})
                v = g.get(nid)
            return v if callable(v) else None
        if isinstance(func, ast.Attribute):
            base = self._resolve_object(func.value, ctx, fn, 1)
            if base is UNSUPPORTED or base is None:
                return None
            b = base.obj if isinstance(base, Obj) else base
            if isinstance(base, StateRef):
                b = self._state_probe
            m = getattr(b, func.attr, None)
            return m if callable(m) else None
        return None

    # -- expression translation (boolean context) --------------------------------

    def _translate_expr(self, node, ctx, fn, depth) -> dict:
        if depth > self.MAX_EXPR_DEPTH:
            return self._opaque_any(node, "expression depth exceeded")

        if isinstance(node, ast.Constant):
            if node.value is None or isinstance(node.value, (bool, int, float, str)):
                return static_node(bool(node.value))
            return self._opaque_any(node, "constant type")

        if isinstance(node, ast.Name):
            nid = node.id
            if nid == "state":
                return static_node(True)
            if nid in ("True", "False"):
                return static_node(nid == "True")
            if nid == "None":
                return static_node(False)
            if nid in ctx:
                return self._value_node(ctx[nid])
            g = getattr(fn, "__globals__", {})
            if nid in g:
                return self._value_node(g[nid])
            return self._opaque_any(node, f"unbound name {nid}")

        if isinstance(node, ast.BoolOp):
            op = "and" if isinstance(node.op, ast.And) else "or"
            parts = [self._translate_expr(v, ctx, fn, depth + 1)
                     for v in node.values]
            return self._bool_combine(op, parts)

        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            inner = simplify(self._translate_expr(node.operand, ctx, fn,
                                                  depth + 1))
            if inner.get("op") == "static":
                return static_node(not inner["value"])
            return {"op": "not", "nodes": [inner]}

        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            v = self._eval(node.operand, ctx, fn, depth + 1)
            if isinstance(v, (int, float)):
                return static_node(-v)
            return self._opaque_any(node, "unary")

        if isinstance(node, ast.IfExp):
            test = self._translate_expr(node.test, ctx, fn, depth + 1)
            then_node = self._translate_expr(node.body, ctx, fn, depth + 1)
            else_node = self._translate_expr(node.orelse, ctx, fn, depth + 1)
            return self._bool_combine("or", [
                self._bool_combine("and", [test, then_node]),
                self._bool_combine("and",
                    [{"op": "not", "nodes": [test]}, else_node]),
            ])

        if isinstance(node, ast.Compare):
            return self._translate_compare(node, ctx, fn, depth)

        if isinstance(node, ast.BinOp):
            v = self._eval(node, ctx, fn, depth + 1)
            if v is UNSUPPORTED or v is STATE:
                return self._opaque_any(node, "binop")
            return static_node(bool(v))

        if isinstance(node, ast.Attribute):
            v = self._eval(node, ctx, fn, depth + 1)
            if v is UNSUPPORTED or v is STATE:
                return self._opaque_any(node, "attribute access")
            return static_node(bool(v))

        if isinstance(node, ast.Subscript):
            v = self._eval(node, ctx, fn, depth + 1)
            if v is UNSUPPORTED or v is STATE:
                if os.environ.get("DUMP_DEBUG_OPAQUE"):
                    try:
                        src = ast.unparse(node)
                    except Exception:
                        src = "?"
                    base_v = self._resolve_object(node.value, ctx, fn, depth + 1)
                    key_v = self._eval(node.slice, ctx, fn, depth + 1)
                    print("  subscript detail: %s base=%s key=%s ctx=%s" % (
                        src, type(base_v).__name__ if base_v is not UNSUPPORTED else "UNSUPPORTED",
                        "UNSUPPORTED" if key_v is UNSUPPORTED else repr(key_v)[:40],
                        sorted(k for k in ctx.keys())[:12]), flush=True)
                return self._opaque_any(node, "subscript")
            return static_node(bool(v))

        if isinstance(node, ast.Call):
            return self._translate_call(node, ctx, fn, depth)

        if isinstance(node, (ast.Tuple, ast.List)):
            v = self._eval(node, ctx, fn, depth + 1)
            if v is UNSUPPORTED:
                return self._opaque_any(node, "collection")
            return static_node(bool(v))

        if isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp,
                             ast.DictComp, ast.NamedExpr, ast.Lambda,
                             ast.JoinedStr)):
            return self._opaque_any(node, f"{type(node).__name__}")

        return self._opaque_any(node, f"expr {type(node).__name__}")

    def _value_node(self, val) -> dict:
        if callable(val) and not isinstance(val, types.FunctionType):
            return static_node(True)
        if isinstance(val, types.FunctionType):
            return self.translate_callable(val)
        if val is STATE:
            return static_node(True)
        if foldable_value(val):
            return static_node(bool(val))
        return static_node(True)  # object presence -> truthy

    @staticmethod
    def _bool_combine(op, parts):
        flat = []
        for p in parts:
            if p.get("op") == op and op in ("and", "or"):
                flat.extend(p["nodes"])
            else:
                flat.append(p)
        # dedupe identical siblings
        seen, uniq = set(), []
        for p in flat:
            k = canonical(p)
            if k not in seen:
                seen.add(k)
                uniq.append(p)
        flat = uniq
        if op == "and":
            if any(p.get("op") == "static" and not p["value"] for p in flat):
                return static_node(False)
            flat = [p for p in flat
                    if not (p.get("op") == "static" and p["value"])]
        else:
            if any(p.get("op") == "static" and p["value"] for p in flat):
                return static_node(True)
            flat = [p for p in flat
                    if not (p.get("op") == "static" and not p["value"])]
        if not flat:
            return static_node(op == "and")
        if len(flat) == 1:
            return flat[0]
        return {"op": op, "nodes": flat}

    # -- calls -----------------------------------------------------------------

    def _translate_call(self, node: ast.Call, ctx, fn, depth) -> dict:
        func = node.func

        # 0. any(f(state) for f in options) / all(...) over a CONCRETE list of
        #    callables (Rules.py set_bunny_rules: options_to_access_rule and
        #    path_to_access_rule).  The generator is expanded into an OR/AND
        #    of the inlined callables.  Without this every bunny rule in the
        #    owglitches/hybridglitches profiles was an opaque node and the
        #    engine refused the whole world.
        if isinstance(func, ast.Name) and func.id in ("any", "all") \
                and len(node.args) == 1 and not node.keywords \
                and isinstance(node.args[0], (ast.GeneratorExp, ast.ListComp)):
            gen = node.args[0]
            if len(gen.generators) == 1 and not gen.generators[0].ifs \
                    and isinstance(gen.generators[0].target, ast.Name):
                loopvar = gen.generators[0].target.id
                elt = gen.elt
                seq = self._eval(gen.generators[0].iter, ctx, fn, depth + 1)
                call_ok = isinstance(elt, ast.Call) and isinstance(elt.func, ast.Name) \
                    and elt.func.id == loopvar and len(elt.args) == 1 \
                    and isinstance(elt.args[0], ast.Name) and elt.args[0].id == "state"
                if call_ok and seq is not UNSUPPORTED and seq is not STATE \
                        and isinstance(seq, (list, tuple)) and all(callable(c) for c in seq):
                    # Each option is a self-contained rule: translate it ONCE
                    # at a shallow depth and memoize by identity.  Option lists
                    # nest (bunny options hold path rules holding item rules),
                    # so inheriting the caller's depth blew past MAX_*_DEPTH and
                    # the depth-keyed cache made the dump exponential.
                    parts = [self._shared_rule_ref(c) for c in seq]
                    if not parts:
                        return static_node(func.id == "all")
                    return self._bool_combine("or" if func.id == "any" else "and", parts)

        # 0.5 <entrance or location>.can_reach(state): BaseClasses.Entrance.can_reach
        #     is "parent region reachable and my access rule passes" - the same
        #     shape _can_reach_node emits for state.can_reach(<entrance>).
        if isinstance(func, ast.Attribute) and func.attr == "can_reach"                 and not (isinstance(func.value, ast.Name) and func.value.id in ("state", "self")):
            base = self._resolve_object(func.value, ctx, fn, depth + 1)
            raw = base.obj if isinstance(base, Obj) else base
            if raw is not None and raw is not UNSUPPORTED and raw is not STATE                     and hasattr(raw, "parent_region") and hasattr(raw, "access_rule")                     and getattr(raw.parent_region, "name", None):
                parts = [{"op": "reachable", "spot": raw.parent_region.name, "spot_type": "Region"}]
                if callable(raw.access_rule):
                    parts.append(self._shared_rule_ref(raw.access_rule))
                return self._bool_combine("and", parts)

        # 1. state.<method>(...) -- 'state' always denotes the state argument;
        #    or self.<method>(...) when self is bound to the probe state
        if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) \
                and (func.value.id == "state"
                     or (func.value.id == "self" and self._self_is_state(ctx))):
            return self._state_method(func.attr, node, ctx, fn, depth)

        # 2. known global functions (eval_location_main / eval_small_key_door_main)
        target = self._resolve_function(func, ctx, fn)
        if target is not None:
            if id(target) in self._special_globals:
                args = self._fold_args_skip_state(node, ctx, fn, depth)
                if args is not None:
                    return self._special_globals[id(target)](args)
                return self._opaque_any(node, "special call args")
            if target in (int, bool, str, float, len):
                v = self._eval(node, ctx, fn, depth + 1)
                if v is not UNSUPPORTED and v is not STATE:
                    return static_node(bool(v))
                return self._opaque_any(node, "builtin call")
            if callable(target):
                sub = self._inline_target(target, func, node, ctx, fn, depth)
                if sub is not None:
                    return sub
        return self._opaque_any(node, "unresolved call to "
                                + _callable_name(target, func))

    def _shared_rule_ref(self, c):
        """Translate callable `c` once (shallow depth, memoized by identity),
        REGISTER it under its content hash, and return a {"op":"ref"} node.
        Shared sub-rules (bunny options, path rules, entrance access rules)
        are referenced thousands of times; embedding them inflated the
        owglitches rules.json to 150 MB.  The engine resolves refs against
        the same "nodes" registry (rando_rules.c)."""
        memo = self.__dict__.setdefault("_option_memo", {})
        k = id(c)
        if k not in memo:
            # functools.partial (underworld_glitches_rules: bomb_clip with
            # region=/player= pre-bound) has no source of its own: translate
            # the wrapped function with the partial's arguments bound.  A
            # bound method binds its instance as self.
            f, bind_self, bind_args = c, None, {}
            if isinstance(f, functools.partial):
                bind_args = dict(f.keywords or {})
                pos = list(f.args or ())
                f = f.func
                code = getattr(f, "__code__", None)
                if code is not None and pos:
                    params = list(code.co_varnames[:code.co_argcount])
                    if params and params[0] == "self":
                        params = params[1:]
                    for name, v in zip(params, pos):
                        bind_args.setdefault(name, v)
            if isinstance(f, types.MethodType):
                bind_self, f = f.__self__, f.__func__
            node = simplify(self.translate_callable(f, 1, bind_self=bind_self,
                                                    bind_args=bind_args or None))
            if node.get("op") in ("static", "opaque"):
                memo[k] = node                       # trivial: keep inline
            else:
                memo[k] = {"op": "ref", "id": self.register(node, src=lambda_source(c))}
        return memo[k]

    def _inline_target(self, target, func, node: ast.Call, ctx, fn, depth):
        """Inline a function/method call with its parameters bound to the
        (folded) call-site arguments.  Returns None when binding fails."""
        f, owner = target, None
        if isinstance(target, functools.partial):
            # closure-held partial (underworld_glitches_rules: bomb_clip with
            # region=/player= pre-bound): bind the partial's own arguments,
            # then map the call-site arguments onto the REMAINING parameters
            pf = target.func
            if isinstance(pf, types.MethodType):
                pf, owner = pf.__func__, pf.__self__
            code = getattr(pf, "__code__", None)
            if code is None:
                return None
            params = list(code.co_varnames[:code.co_argcount])
            if params and params[0] == "self":
                params = params[1:]
            binds = dict(target.keywords or {})
            pos = list(target.args or ())
            for name, v in zip(params, pos):
                binds.setdefault(name, v)
            remaining = [q for q in params[len(pos):] if q not in binds]
            for i, a in enumerate(node.args):
                if i >= len(remaining):
                    return None
                if isinstance(a, ast.Name) and a.id == "state":
                    binds[remaining[i]] = STATE
                    continue
                v = self._eval(a, ctx, fn, depth + 1)
                if v is UNSUPPORTED or v is STATE:
                    return None
                binds[remaining[i]] = v
            sub = self.translate_callable(pf, depth + 1, bind_self=owner, bind_args=binds)
            return sub if sub.get("op") != "opaque" or count_opaque(sub) else None
        if isinstance(target, types.MethodType):
            f, owner = target.__func__, target.__self__
        binds = self._fold_params(f, node, ctx, fn, depth)
        if binds is not None:
            # bind an owner as 'self' only for unbound method-style functions
            code = getattr(f, "__code__", None)
            takes_self = bool(code and code.co_argcount
                              and code.co_varnames[0] == "self")
            if owner is None and takes_self and isinstance(func, ast.Attribute):
                base = self._resolve_object(func.value, ctx, fn, depth + 1)
                if isinstance(base, Obj):
                    owner = base.obj
                elif isinstance(base, StateRef):
                    owner = self._state_probe
            sub = self.translate_callable(f, depth + 1, bind_self=owner,
                                          bind_args=binds)
            if sub.get("op") != "opaque":
                return sub
            if binds:
                # the bound translation is the truthful one; retrying with the
                # parameters stripped only manufactures a second, worse opaque
                # node and hides the real leaf failure
                return sub if count_opaque(sub) else None
        # fall back: translate without bound params
        if isinstance(target, (types.FunctionType, types.MethodType)):
            sub = self.translate_callable(f, depth + 1, bind_self=owner)
            if sub.get("op") != "opaque":
                return sub
        return None

    def _fold_params(self, f, node: ast.Call, ctx, fn, depth):
        code = getattr(f, "__code__", None)
        if code is None:
            return None
        params = list(code.co_varnames[:code.co_argcount])
        if params and params[0] == "self":
            params = params[1:]
        binds = {}
        args = node.args
        if len(args) > len(params):
            return None
        for i, a in enumerate(args):
            if isinstance(a, ast.Name) and a.id == "state":
                binds[params[i]] = STATE
                continue
            v = self._eval(a, ctx, fn, depth + 1)
            if v is UNSUPPORTED or v is STATE:
                return None
            binds[params[i]] = v
        for kw in node.keywords:
            if kw.arg is None:
                return None
            v = self._eval(kw.value, ctx, fn, depth + 1)
            if v is UNSUPPORTED or v is STATE:
                return None
            binds[kw.arg] = v
        defaults = getattr(f, "__defaults__", None) or ()
        if defaults:
            for name, dv in zip(params[len(params) - len(defaults):],
                                defaults):
                binds.setdefault(name, dv)
        return binds

    def _self_is_state(self, ctx) -> bool:
        s = ctx.get("self")
        if s is None:
            return True  # top-level lambda: self refers to nothing else
        if isinstance(s, StateRef):
            return True
        if isinstance(s, Obj):
            return isinstance(s.obj, self._CollectionState)
        return False

    def _fold_args_skip_state(self, node: ast.Call, ctx, fn, depth):
        vals = []
        for a in node.args:
            if isinstance(a, ast.Name) and a.id == "state":
                vals.append(SKIP_ARG)
                continue
            v = self._eval(a, ctx, fn, depth + 1)
            if v is UNSUPPORTED:
                return None
            vals.append(v)
        return [v for v in vals if v is not SKIP_ARG]

    # -- state method dispatch ---------------------------------------------------

    def _state_method(self, attr, node: ast.Call, ctx, fn, depth) -> dict:
        a = node.args

        def ev(i):
            if len(a) <= i:
                return UNSUPPORTED
            return self._eval(a[i], ctx, fn, depth + 1)

        def kwval(name):
            for kw in node.keywords:
                if kw.arg == name:
                    return self._eval(kw.value, ctx, fn, depth + 1)
            return UNSUPPORTED

        if attr == "has":
            item, count = ev(0), (ev(2) if len(a) > 2 else 1)
            if isinstance(item, str) and isinstance(count, int):
                return {"op": "item", "item": item, "count": count}
            return self._opaque_any(node, "has()")
        if attr == "item_count":
            item = ev(0)
            if isinstance(item, str):
                return {"op": "item", "item": item, "count": 1}
            return self._opaque_any(node, "item_count()")
        if attr == "has_bottle":
            return {"op": "bottles", "count": 1}
        if attr == "bottle_count":
            # state-dependent arithmetic helper: not expressible statically
            return self._opaque_any(node, "bottle_count() (state arithmetic)")
        if attr == "has_crystals":
            n = ev(0)
            if isinstance(n, int):
                return {"op": "crystals", "count": n}
            return self._opaque_any(node, "has_crystals()")
        if attr == "has_pendants":
            n = ev(0)
            if isinstance(n, int):
                return {"op": "pendants", "count": n}
            return self._opaque_any(node, "has_pendants()")
        if attr in ("has_bosses", "has_crystal_bosses", "has_pendant_bosses"):
            n = ev(0)
            prize = None
            if attr == "has_crystal_bosses":
                prize = "Crystal"
            elif attr == "has_pendant_bosses":
                prize = "Pendant"
            elif len(a) > 2:
                prize = ev(2)
            if isinstance(n, int):
                return {"op": "bosses", "count": n,
                        "prize": prize if isinstance(prize, str) or prize is None
                        else str(prize)}
            return self._opaque_any(node, attr)
        if attr == "has_hearts":  # signature: (player, count)
            n = ev(1)
            if isinstance(n, int):
                return {"op": "hearts", "count": n}
            return self._opaque_any(node, "has_hearts()")
        if attr == "heart_count":
            return {"op": "hearts", "count": 1}
        if attr == "can_reach":
            return self._can_reach_node(node, ctx, fn, depth)
        if attr in ("can_reach_blue", "can_reach_orange"):
            spot = ev(0)
            name = self._spot_name(spot)
            if name is not None:
                return {"op": "barrier", "region": name,
                        "barrier": "blue" if attr == "can_reach_blue"
                        else "orange"}
            return self._opaque_any(node, attr)
        if attr == "can_buy_unlimited":
            item = ev(0)
            if isinstance(item, str):
                return {"op": "unlimited", "item": item}
            return self._opaque_any(node, "can_buy_unlimited()")
        if attr == "can_extend_magic":
            magic = ev(1) if len(a) > 1 else kwval("smallmagic")
            if magic is UNSUPPORTED:
                magic = 16
            refill = kwval("fullrefill")
            if refill is UNSUPPORTED:
                refill = ev(2) if len(a) > 2 else False
            if isinstance(magic, (int, float)):
                return {"op": "extend_magic", "magic": int(magic),
                        "fullrefill": bool(refill)}
            return self._opaque_any(node, "can_extend_magic()")
        if attr == "is_not_bunny":
            spot = ev(0)
            name = self._spot_name(spot)
            if name is not None:
                return {"op": "not_bunny", "region": name}
            return self._opaque_any(node, "is_not_bunny()")
        if attr == "can_reach_light_world":
            return {"op": "reach_light_world"}
        if attr == "can_reach_dark_world":
            return {"op": "reach_dark_world"}
        if attr == "everything":
            return {"op": "everything"}
        if attr == "is_door_open":
            door = ev(0)
            if isinstance(door, str):
                return {"op": "door_open", "door": door}
            return self._opaque_any(node, "is_door_open()")
        if attr in ("has_sm_key", "has_sm_key_strict"):
            if self.world.keyshuffle[1] == "universal":
                return self._opaque_any(
                    node, "has_sm_key() universal-key branch not supported")
            item, count = ev(0), (ev(2) if len(a) > 2 else 1)
            if isinstance(item, str) and isinstance(count, int):
                return {"op": "item", "item": item, "count": count, "key": True}
            return self._opaque_any(node, attr)
        if attr in ("can_bomb_clip", "can_dash_clip"):
            name = self._spot_name(ev(0))
            if name is not None:
                nb = {"op": "not_bunny", "region": name}
                boots = {"op": "item", "item": "Pegasus Boots", "count": 1}
                if attr == "can_dash_clip":
                    return self._bool_combine("and", [nb, boots])
                # pass the caller's player expression through: with no args
                # can_use_bombs inlined with `player` unbound and its whole
                # can_farm_bombs/can_stun_enemies chain went opaque (hybrid
                # profile, every underworld clip rule)
                bombs = self._state_method(
                    "can_use_bombs",
                    ast.Call(func=ast.Attribute(
                        value=ast.Name(id="state", ctx=ast.Load()),
                        attr="can_use_bombs", ctx=ast.Load()),
                        args=[a[1]] if len(a) > 1 else [], keywords=[]),
                    ctx, fn, depth)
                return self._bool_combine("and", [nb, boots, bombs])
            return self._opaque_any(node, attr)
        if attr in ("can_farm_rupees",):
            return {"op": "item", "item": "Farmable Rupees", "count": 1}

        # generic: inline the method source with self bound to the probe state
        meth = getattr(type(self._state_probe), attr, None)
        if meth is None:
            meth = getattr(self._state_probe, attr, None)
        if callable(meth):
            sub = self._inline_target(meth, func_of_call(node), node, ctx, fn,
                                      depth)
            if sub is not None:
                return sub
        return self._opaque_any(node, f"unknown state method {attr}")

    @staticmethod
    def _spot_name(spot):
        if spot is UNSUPPORTED or spot is STATE or spot is None:
            return None
        if isinstance(spot, Obj):
            spot = spot.obj
        if isinstance(spot, str):
            return spot
        n = getattr(spot, "name", None)
        return n if isinstance(n, str) else None

    def _can_reach_node(self, node, ctx, fn, depth):
        a = node.args
        spot = ev0 = None
        hint = None
        if a:
            spot = self._eval(a[0], ctx, fn, depth + 1)
        for kw in node.keywords:
            if kw.arg == "resolution_hint":
                hint = self._eval(kw.value, ctx, fn, depth + 1)
        if len(a) > 1:
            hint = self._eval(a[1], ctx, fn, depth + 1)
        # can_reach(<Entrance or Location object>): the engine only knows
        # REGIONS.  BaseClasses says such a spot is reachable when its parent
        # region is reachable and its own access rule passes, so emit
        # exactly that (the access rule becomes a shared ref).  Path rules
        # in set_bunny_rules do this for every bunny-impassable cave.
        raw = spot.obj if isinstance(spot, Obj) else spot
        if raw is not None and raw is not UNSUPPORTED and raw is not STATE \
                and not isinstance(raw, str) \
                and hasattr(raw, "parent_region") and hasattr(raw, "access_rule") \
                and getattr(raw.parent_region, "name", None):
            parts = [{"op": "reachable", "spot": raw.parent_region.name, "spot_type": "Region"}]
            ar = raw.access_rule
            if callable(ar):
                parts.append(self._shared_rule_ref(ar))
            return self._bool_combine("and", parts)
        name = self._spot_name(spot)
        if name is None:
            return self._opaque_any(node, "can_reach()")
        stype = hint if isinstance(hint, str) else None
        if stype is None:
            sp = spot.obj if isinstance(spot, Obj) else None
            stype = getattr(sp, "spot_type", None) or "Region"
        return {"op": "reachable", "spot": name, "spot_type": stype}

    # -- compare ------------------------------------------------------------------

    def _translate_compare(self, node: ast.Compare, ctx, fn, depth) -> dict:
        special = self._count_compare(node, ctx, fn, depth)
        if special is not None:
            return special
        cur = self._eval(node.left, ctx, fn, depth + 1)
        if cur is UNSUPPORTED or cur is STATE:
            lhs = self._translate_expr(node.left, ctx, fn, depth + 1)
            return self._opaque_any(node, "comparison on non-constant "
                                         + json.dumps(lhs.get("op")))
        for op, comp in zip(node.ops, node.comparators):
            right = self._eval(comp, ctx, fn, depth + 1)
            if right is UNSUPPORTED or right is STATE:
                return self._opaque_any(node, "comparison on non-constant")
            if isinstance(op, (ast.In, ast.NotIn)):
                try:
                    r = cur in right
                except Exception:
                    return self._opaque_any(node, "in-comparison")
                if isinstance(op, ast.NotIn):
                    r = not r
                cur = r
                continue
            cmpf = {ast.Eq: lambda x, y: x == y,
                    ast.NotEq: lambda x, y: x != y,
                    ast.Lt: lambda x, y: x < y,
                    ast.LtE: lambda x, y: x <= y,
                    ast.Gt: lambda x, y: x > y,
                    ast.GtE: lambda x, y: x >= y}.get(type(op))
            if cmpf is None:
                return self._opaque_any(node, "comparison op")
            try:
                cur = cmpf(cur, right)
            except Exception:
                return self._opaque_any(node, "comparison")
        return static_node(bool(cur))

    def _count_compare(self, node: ast.Compare, ctx, fn, depth):
        """item_count('X', p) [+ item_count('Y', p)] >= n  ->  item nodes."""
        def items_of(expr):
            if isinstance(expr, ast.BinOp) and isinstance(expr.op, ast.Add):
                l = items_of(expr.left)
                r = items_of(expr.right)
                return None if (l is None or r is None) else l + r
            if isinstance(expr, ast.Call) and \
                    isinstance(expr.func, ast.Attribute) and \
                    expr.func.attr == "item_count" and len(expr.args) >= 1:
                v = self._eval(expr.args[0], ctx, fn, depth + 1)
                if isinstance(v, str):
                    return [v]
            return None

        items = items_of(node.left)
        if not items:
            return None
        if len(node.ops) != 1 or not isinstance(node.ops[0], (ast.Gt, ast.GtE)):
            return None
        rhs = self._eval(node.comparators[0], ctx, fn, depth + 1)
        if not isinstance(rhs, int):
            return None
        n = rhs + (0 if isinstance(node.ops[0], ast.GtE) else 1)
        if len(items) == 1:
            return {"op": "item", "item": items[0], "count": n}
        return {"op": "item_count_sum", "items": items, "count": n}

    # -- special global handlers ----------------------------------------------------

    def _loc_check_from_call(self, args):
        item, location = args[0], args[1]
        return {"op": "location_check", "item": item, "location": location}

    def _skd_from_call(self, args):
        door_name, dungeon = args[0], args[1]
        return {"op": "small_key_door", "door": door_name, "dungeon": dungeon}

    # -- the fork's typed Rule IR -----------------------------------------------------

    def serialize_typed_rule(self, rule, depth=0) -> dict:
        if depth > self.MAX_INLINE_DEPTH:
            return self._opaque_any(rule, "typed rule depth")
        rt = getattr(rule, "rule_type", None)
        name = getattr(rt, "name", None) or str(rt)
        sub = [self.serialize_typed_rule(s, depth + 1)
               for s in getattr(rule, "sub_rules", [])]
        if name == "Conjunction":
            return self._bool_combine("and", sub) if sub else static_node(True)
        if name == "Disjunction":
            return self._bool_combine("or", sub) if sub else static_node(False)
        if name == "Negate":
            return {"op": "not", "nodes": sub}
        if name == "Item":
            return {"op": "item", "item": rule.principal, "count": rule.count}
        if name == "Bottle":
            return {"op": "bottles", "count": 1}
        if name == "Crystal":
            return {"op": "crystals", "count": rule.principal}
        if name == "Hearts":
            return {"op": "hearts", "count": rule.principal}
        if name == "Static":
            return static_node(bool(rule.principal))
        if name == "Reachability":
            p = rule.principal
            return {"op": "reachable", "spot": getattr(p, "name", str(p)),
                    "spot_type": getattr(p, "spot_type", "Region")}
        if name == "Barrier":
            p = rule.principal
            bar = getattr(rule.barrier, "name", str(rule.barrier))
            return {"op": "barrier", "region": getattr(p, "name", str(p)),
                    "barrier": bar}
        if name == "Unlimited":
            return {"op": "unlimited", "item": rule.principal,
                    "shop_regions": [getattr(r, "name", str(r))
                                     for r in rule.locations]}
        if name == "ExtendMagic":
            return {"op": "extend_magic", "magic": rule.principal,
                    "fullrefill": bool(rule.flag),
                    "shop_regions": [getattr(r, "name", str(r))
                                     for r in rule.locations]}
        if name == "Boss":
            boss = rule.principal
            dr = getattr(boss, "defeat_rule", None)
            inner = self.serialize_typed_rule(dr, depth + 1) if dr is not None \
                else static_node(True)
            return {"op": "boss", "boss": getattr(boss, "name", str(boss)),
                    "nodes": [inner]}
        if name == "LocationCheck":
            return {"op": "location_check", "item": rule.principal,
                    "location": getattr(rule, "location", None)}
        if name == "SmallKeyDoor":
            door, dungeon = rule.principal
            hint = getattr(rule, "resolution_hint", None)
            variants = []
            for k, num in getattr(hint, "new_rules", {}).items():
                variants.append({"rule_type": keyrule_str(k), "keys": num})
            return {"op": "small_key_door", "door": door, "dungeon": dungeon,
                    "variants": variants}
        node = {"op": "typed", "rule_type": name, "nodes": sub}
        try:
            node["print"] = textwrap.shorten(rule.__unicode__(), 200)
        except Exception:
            pass
        return node

    def serialize_dnf(self, rule):
        """DNF item requirements via get_requirements() -- the form the fork
        itself uses for key flooding."""
        try:
            reqs = rule.get_requirements()
        except Exception as e:
            return {"error": str(e)}
        out = []
        for rs in reqs:
            terms = []
            for r in rs.get_values():
                terms.append({
                    "req": r.req_type.name,
                    "item": r.item,
                    "amount": r.amount,
                    "player": r.player,
                    "crystal": getattr(r.crystal, "name", str(r.crystal)),
                    "locations": list(r.locations),
                })
            out.append(terms)
        return out


# ---------------------------------------------------------------------------
# World -> JSON dumps
# ---------------------------------------------------------------------------

def dump_world(world, outdir: str, repo: str, versions: dict) -> dict:
    global _last_rule_registry
    os.makedirs(outdir, exist_ok=True)
    dumper = RuleDumper(world)
    _last_rule_registry = dumper.registry

    # -- regions ------------------------------------------------------------
    regions = sorted(world.regions, key=lambda r: r.name)
    names = [r.name for r in regions]
    dupes = {n for n in names if names.count(n) > 1}
    if dupes:
        raise SystemExit(f"duplicate region names: {sorted(dupes)[:5]}")
    rid = {r.name: i for i, r in enumerate(regions)}
    regions_json = []
    for i, r in enumerate(regions):
        regions_json.append({
            "id": i,
            "name": r.name,
            "type": r.type.name,
            "dungeon": r.dungeon.name if r.dungeon else None,
            "hint": r.hint_text,
            "light_world": bool(r.is_light_world),
            "dark_world": bool(r.is_dark_world),
            "indoors": bool(r.type.is_indoors),
        })

    # -- edges -----------------------------------------------------------------
    edges = []
    for r in world.regions:
        for e in r.exits:
            if e.connected_region is not None:
                edges.append(e)
    edges.sort(key=lambda e: (e.parent_region.name, e.name))
    edges_json = []
    for i, e in enumerate(edges):
        rule_id = dumper.rule_ref(e.access_rule, f"entrance:{e.name}")
        rec = {
            "id": i,
            "from": rid[e.parent_region.name],
            "to": rid[e.connected_region.name],
            "entrance": e.name,
            "spot_type": e.spot_type,
            "rule": rule_id,
        }
        if getattr(e, "door", None) is not None:
            d = e.door
            rec["door"] = getattr(d, "name", None)
            rec["door_type"] = getattr(getattr(d, "type", None), "name", None)
        if getattr(e, "vanilla", None):
            rec["vanilla_target"] = e.vanilla
        edges_json.append(rec)

    # -- locations ---------------------------------------------------------------
    locs = sorted(world.get_locations(), key=lambda l: l.name)
    seen = {}
    locations_json = []
    for l in locs:
        base = l.name
        seen[base] = seen.get(base, 0) + 1
        name = base if seen[base] == 1 else f"{base} #{seen[base]}"
        rule_id = dumper.rule_ref(l.access_rule, f"location:{l.name}")
        rec = {
            "id": len(locations_json),
            "name": name,
            "region": rid[l.parent_region.name],
            "type": l.type.name,
            "event": bool(l.event),
            "real": bool(l.real),
            "locked": bool(l.locked),
            "address": hex(l.address) if isinstance(l.address, int) else None,
            "rule": rule_id,
        }
        if getattr(l, "verbose_rule", None) is not None:
            rec["typed_ir"] = dumper.typed_ir_ref(l.verbose_rule)
            dnf = dumper.serialize_dnf(l.verbose_rule)
            if "error" not in dnf:
                rec["dnf"] = dnf
        if getattr(l, "forced_item", None) is not None:
            rec["forced_item"] = l.forced_item.name
        if l.item is not None:
            rec["placed_item"] = {
                "name": l.item.name,
                "player": l.item.player,
                "progression": bool(l.item.advancement),
            }
        locations_json.append(rec)

    # -- items --------------------------------------------------------------------
    from collections import Counter
    pool = Counter()
    prog = {}
    itype = {}
    for it in world.itempool:
        pool[it.name] += 1
        prog[it.name] = bool(it.advancement)
        itype[it.name] = it.type
    pre = Counter(it.name for it in world.precollected_items)
    items_json = []
    for name in sorted(pool):
        rec = {"item": name, "count": pool[name],
               "progression": prog[name], "type": itype[name]}
        if pre.get(name):
            rec["precollected"] = pre[name]
        items_json.append(rec)
    for name in sorted(pre):
        if name not in pool:
            items_json.append({"item": name, "count": 0, "progression": None,
                               "type": None, "precollected": pre[name]})

    # -- rules registry --------------------------------------------------------------
    rules_json = {
        "version": 1,
        "profile": {
            "logic": world.logic[1],
            "mode": world.mode[1],
            "door_shuffle": world.doorShuffle[1],
            "shuffle": world.shuffle[1],
            "keyshuffle": world.keyshuffle[1],
            "goal": world.goal[1],
        },
        "nodes": dict(sorted(dumper.registry.items())),
        "sources": dict(sorted(dumper.sources.items())),
        "stats": dumper.stats.as_dict(),
        "untranslated": [{"spot": s, "reason": r, "python": p}
                         for (s, r, p) in dumper.stats.untranslated],
    }

    # -- meta ---------------------------------------------------------------------------
    placed = sum(1 for l in world.get_locations()
                 if l.item is not None and not l.event)
    meta = {
        "dump_version": 1,
        "tool": "dump_logic.py",
        "seed": int(world.seed),
        "generator_version": versions["dr"],
        "overworld_randomizer_version": versions["or"],
        "repo": {"path": repo, "git_hash": versions["git"]},
        "settings": {
            "players": world.players,
            "logic": world.logic[1],
            "mode": world.mode[1],
            "goal": world.goal[1],
            "weapons": world.swords[1],
            "door_shuffle": world.doorShuffle[1],
            "shuffle": world.shuffle[1],
            "keyshuffle": world.keyshuffle[1],
            "mapshuffle": world.mapshuffle[1],
            "compassshuffle": world.compassshuffle[1],
            "bigkeyshuffle": world.bigkeyshuffle[1],
            "pottery": world.pottery[1],
            "prizeshuffle": world.prizeshuffle[1],
            "shopsanity": bool(world.shopsanity[1]),
            "crystals_ganon": world.crystals_needed_for_ganon[1],
            "crystals_gt": world.crystals_needed_for_gt[1],
        },
        "counts": {
            "regions": len(regions_json),
            "edges": len(edges_json),
            "locations": len(locations_json),
            "items_in_pool": int(sum(pool.values())),
            "item_names": len(pool),
            "placed_non_event": placed,
            "event_locations": sum(1 for l in locations_json if l["event"]),
        },
        "rules": dumper.stats.as_dict(),
    }

    for fname, data in [("regions.json", regions_json),
                        ("edges.json", edges_json),
                        ("locations.json", locations_json),
                        ("items.json", items_json),
                        ("rules.json", rules_json),
                        ("meta.json", meta)]:
        with open(os.path.join(outdir, fname), "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1, default=str)
    return meta


# ---------------------------------------------------------------------------
# Phase-1 schema-gap dumps (--with-fill-data)
# ---------------------------------------------------------------------------
#
# The five files below close the gaps the C scaffold found in the core dump:
#
#   keydoors.json        world.key_logic door_rules: the REAL per-door small-key
#                        data (WorstCase / AllowSmall / Lock / CrystalAlternative
#                        variants) the `small_key_door` rule nodes only point at.
#   shops.json           world.shops stock + the shop->location table that
#                        `unlimited` / `extend_magic` rules consult.
#   barriers.json        per-door crystal-barrier state (door.crystal et al) that
#                        drives the fork's orange/blue region flow, plus the
#                        exact propagation semantics.
#   vanilla_locations.json
#                        the fork's static vanilla item->location tables
#                        (source/item/FillUtil.py) inverted to location->item.
#   groups.json          the semantic item groups + profile limits consumed by
#                        CollectionState.collect() (progressive chains, bottles).
#
# Everything here is read out of the finished World (post-fill) or imported
# verbatim from the fork's own modules; no fork state is modified.

_CRYSTAL_NAMES = {0: "Null", 1: "Blue", 2: "Orange", 3: "Either"}

# registry (rule id -> node) of the most recently dumped world's rules.json;
# reused by the barriers dump to list the regions probed by barrier rules
_last_rule_registry = {}


def _dump_keydoors(world) -> dict:
    from BaseClasses import KeyRuleType
    dungeons, doors = [], []
    for d_name, d_logic in sorted(world.key_logic[1].items()):
        layout = world.key_layout[1].get(d_name)
        dungeons.append({
            "dungeon": d_name,
            "small_key_item": d_logic.small_key_name,
            "big_key_item": d_logic.bk_name,
            "max_chests": getattr(layout, "max_chests", None),
        })
        for door_name, rule in sorted(d_logic.door_rules.items()):
            nr = {}
            worstcase = None
            for rt, num in rule.new_rules.items():
                if rt == KeyRuleType.WorstCase:
                    nr["worstcase"] = num
                    worstcase = num
                elif rt == KeyRuleType.AllowSmall:
                    nr["allow_small"] = num
                elif rt == KeyRuleType.CrystalAlternative:
                    nr["crystal_alternative"] = num
                elif isinstance(rt, tuple):
                    nr[f"lock:{rt[1]}"] = num
                else:
                    nr[str(rt)] = num
            small_loc = getattr(rule, "small_location", None)
            doors.append({
                "door": door_name,
                "dungeon": d_name,
                "small_key_num": rule.small_key_num,
                "worstcase": worstcase,
                "partial_threshold": (min(worstcase, rule.small_key_num)
                                      if worstcase is not None else None),
                "new_rules": nr,
                "allow_small": bool(rule.allow_small),
                "small_location": small_loc.name if small_loc else None,
                "alternate_small_key": rule.alternate_small_key,
                "alternate_big_key_loc": sorted(
                    getattr(l, "name", str(l))
                    for l in rule.alternate_big_key_loc),
                "is_valid": bool(rule.is_valid),
            })
    doors.sort(key=lambda d: (d["dungeon"], d["door"]))
    return {
        "version": 1,
        "algorithm": world.key_logic_algorithm[1],
        "keyshuffle": world.keyshuffle[1],
        "partial_eval": (
            "open iff ANY new_rules entry fires (eval_small_key_door_partial): "
            "worstcase n -> has_sm_key >= min(n, small_key_num); "
            "allow_small n -> small_location currently holds the dungeon small "
            "key AND has_sm_key >= n; lock:<BigKey> n -> some location in "
            "alternate_big_key_loc holds that big key AND has_sm_key >= "
            "min(n, alternate_small_key); "
            "crystal_alternative n -> has_sm_key >= n (blue-barrier doors)"),
        "dungeons": dungeons,
        "doors": doors,
    }


def _dump_shops(world) -> dict:
    from Regions import shop_to_location_table, retro_shops
    shops, unlimited = [], {}
    for shop in sorted(world.shops[1], key=lambda s: s.region.name):
        region = shop.region
        inv = []
        for i, slot in enumerate(shop.inventory or []):
            if slot is None:
                continue
            inv.append({
                "slot": i,
                "item": slot["item"],
                "price": slot["price"],
                "max": slot["max"],
                "replacement": slot["replacement"],
                "replacement_price": slot["replacement_price"],
                "unlimited": slot["max"] == 0 or slot["replacement"] is not None,
            })
            # can_buy_unlimited(item) = any reachable shop with has_unlimited(item)
            for name in {slot["item"], slot["replacement"]}:
                if name:
                    unlimited.setdefault(name, [])
                    if region.name not in unlimited[name]:
                        unlimited[name].append(region.name)
        loc_names = (shop_to_location_table.get(region.name)
                     or retro_shops.get(region.name) or [])
        stock = []
        for ln in loc_names:
            loc = world.get_location_unsafe(ln, 1)
            stock.append({"location": ln,
                          "forced_item": loc.item.name if loc and loc.item else None})
        shops.append({
            "region": region.name,
            "region_type": region.type.name,
            "shop_type": shop.type.name,
            "room_id": shop.room_id,
            "locked": bool(shop.locked),
            "custom": bool(shop.custom),
            "inventory": inv,
            "locations": loc_names,
            "stock": stock,
        })
    return {
        "version": 1,
        "shopsanity": bool(world.shopsanity[1]),
        "note": ("shop region = a Region (type Cave) whose name is the shop "
                 "name; it contains the listed locations. With shopsanity off "
                 "each location's forced_item is the slot stock. "
                 "unlimited = items sold by >=1 shop, as tested by "
                 "can_buy_unlimited(item): region reachable AND shop has the "
                 "item as a slot item or as the after-limit replacement."),
        "shops": shops,
        "unlimited": dict(sorted(unlimited.items())),
    }


def _dump_barriers(world) -> dict:
    from BaseClasses import CrystalBarrier
    doors, seen = [], set()
    barrier_rules = {}
    for r in world.regions:
        for e in r.exits:
            d = getattr(e, "door", None)
            if d is None or id(d) in seen:
                continue
            seen.add(id(d))
            doors.append({
                "door": d.name,
                "type": getattr(getattr(d, "type", None), "name", None),
                "from_region": e.parent_region.name,
                "to_region": e.connected_region.name if e.connected_region else None,
                "crystal": _CRYSTAL_NAMES.get(int(d.crystal), str(d.crystal)),
                "c_switch": d.crystal == CrystalBarrier.Either,
                "blocked": bool(d.blocked),
                "trapped": bool(d.trapped),
                "stonewall": bool(d.stonewall),
                "small_key_door": bool(d.smallKey),
                "big_key_door": bool(d.bigKey),
                "alternative_crystal_rule": bool(d.alternative_crystal_rule),
                "req_event": d.req_event,
            })
    doors.sort(key=lambda d: d["door"])
    # regions probed by barrier rules (can_reach_blue/orange) in rules.json
    for node in _last_rule_registry.values():
        if isinstance(node, dict) and node.get("op") == "barrier":
            key = (node["region"], node["barrier"])
            barrier_rules[f"{node['region']}|{node['barrier']}"] = {
                "region": node["region"], "barrier": node["barrier"]}
    return {
        "version": 1,
        "semantics": {
            "states": _CRYSTAL_NAMES,
            "overworld_state": "Orange",
            "dungeon_flow": (
                "CollectionState.traverse_world carries a crystal state "
                "(bitmask Blue|Orange, Either = both). Non-dungeon regions are "
                "always entered with state Orange. Inside a dungeon the state "
                "OR-accumulates over paths (a region already reached can be "
                "re-reached to add a missing bit). Passing a door requires "
                "valid_crystal: door.crystal is Null/Either, or the carried "
                "state is Either, or state == door.crystal, or the door has an "
                "alternative_crystal_rule (Mire blue barriers: also openable "
                "with 2 small keys, see keydoors.json crystal_alternative). "
                "After the door the carried state becomes door.crystal (or "
                "stays, if Null). c_switch doors (crystal == Either) are "
                "crystal-switch touch points: passing through lets you set "
                "either state."),
            "rule_probe": (
                "can_reach_blue(region) / can_reach_orange(region) = region "
                "reachable AND (carried state & bit); emitted as the "
                "`barrier` rule nodes of the core dump"),
            "blocked_doors": (
                "door.blocked (trap/sandbox doors) never propagate the crystal "
                "flow; trapped doors are seed-dependent"),
        },
        "doors": doors,
        "rule_probes": sorted(barrier_rules.values(),
                              key=lambda x: (x["region"], x["barrier"])),
    }


# vanilla item families whose basic/progressive variants occupy the same
# vanilla slots; used to resolve `picked` from the profile's progressive mode
_VANILLA_FAMILIES = [
    ("Fighter Sword", "Progressive Sword"),
    ("Tempered Sword", "Progressive Sword"),
    ("Master Sword", "Progressive Sword"),
    ("Golden Sword", "Progressive Sword"),
    ("Bow", "Progressive Bow"),
    ("Silver Arrows", "Progressive Bow"),
    ("Blue Shield", "Progressive Shield"),
    ("Red Shield", "Progressive Shield"),
    ("Mirror Shield", "Progressive Shield"),
    ("Blue Mail", "Progressive Armor"),
    ("Red Mail", "Progressive Armor"),
    ("Power Glove", "Progressive Glove"),
    ("Titans Mitts", "Progressive Glove"),
]


def _dump_vanilla_locations(world) -> dict:
    import source.item.FillUtil as FU
    dropshuffle = world.dropshuffle[1]
    pottery = world.pottery[1]
    shopsanity = bool(world.shopsanity[1])
    take_any = world.take_any[1]
    # NB: world.progressive is a plain string ('on'/'off'/'random'), not a
    # per-player dict like most settings -- indexing it would grab a character
    progressive = world.progressive if isinstance(world.progressive, str) \
        else world.progressive[1]
    flute_active = (world.flute_mode[1] == "active"
                    or world.is_tile_swapped(0x18, 1))

    inv = {}

    def add(locs, item, src):
        for ln in locs:
            e = inv.setdefault(ln, {"location": ln, "vanilla": [],
                                    "sources": []})
            if item not in e["vanilla"]:
                e["vanilla"].append(item)
            if src not in e["sources"]:
                e["sources"].append(src)

    for item, locs in FU.vanilla_mapping.items():
        add(locs, item, "vanilla_mapping")
    if dropshuffle != "none":
        for item, locs in FU.keydrop_vanilla_mapping.items():
            add(locs, item, "keydrop_vanilla_mapping")
    if pottery not in ("none", "cave"):
        for item, locs in FU.potkeys_vanilla_mapping.items():
            add(locs, item, "potkeys_vanilla_mapping")
    if shopsanity:
        for item, locs in FU.shop_vanilla_mapping.items():
            add(locs, item, "shop_vanilla_mapping")
    if take_any != "none":
        for item, locs in FU.retro_vanilla_mapping.items():
            add(locs, item, "retro_vanilla_mapping")

    # resolve basic <-> progressive variants against the profile: when a
    # location's candidate set is exactly one (basic, progressive) pair from
    # _VANILLA_FAMILIES, keep the member the profile's progressive mode wants
    prog_on = {"on": True, "off": False}.get(progressive)
    family_pairs = set(map(tuple, _VANILLA_FAMILIES))
    locs_out = []
    for name in sorted(inv):
        e = inv[name]
        cand = list(e["vanilla"])
        if "Ocarina" in cand and "Ocarina (Activated)" in cand:
            cand = ["Ocarina (Activated)"] if flute_active else ["Ocarina"]
        elif prog_on is not None:
            pairs = [(a, b) for a in cand for b in cand
                     if a != b and (a, b) in family_pairs]
            if pairs:
                basic, prog = pairs[0]
                cand = [prog] if prog_on else [basic]
        picked = cand[0] if len(cand) == 1 else None
        rec = {"location": name, "picked": picked, "vanilla": cand,
               "sources": e["sources"]}
        locs_out.append(rec)
    world_locs = {l.name for l in world.get_locations()}
    missing = [r["location"] for r in locs_out
               if r["location"] not in world_locs]
    return {
        "version": 1,
        "method": (
            "inverted from the fork's own static tables in "
            "source/item/FillUtil.py: vanilla_mapping (+ keydrop/potkey/shop/"
            "retro variants when the profile activates them). No generation "
            "involved; seed-independent. `vanilla` lists every fork-table "
            "candidate; `picked` is the single profile-resolved item (basic "
            "vs progressive variants resolved via world.progressive, Ocarina "
            "via flute mode) or null when the fork itself leaves the slot "
            "ambiguous (pendant/crystal prize sets, progressive=random, or "
            "inactive variant tables). Prize locations are ambiguous by "
            "design: prizeshuffle=none still shuffles prize items in this "
            "fork. With dropshuffle=none the key-drop locations' vanilla item "
            "is their forced_item in locations.json; with shopsanity off the "
            "shop slots' vanilla item is the shop stock (see shops.json)."),
        "profile": {
            "progressive": progressive,
            "dropshuffle": dropshuffle,
            "pottery": pottery,
            "shopsanity": shopsanity,
            "take_any": take_any,
            "bow_mode": world.bow_mode[1],
            "swords": world.swords[1],
        },
        "counts": {
            "locations": len(locs_out),
            "picked": sum(1 for r in locs_out if r["picked"]),
            "ambiguous": sum(1 for r in locs_out if not r["picked"]),
            "not_in_this_world": len(missing),
        },
        "locations_not_in_dump": sorted(missing),
        "locations": locs_out,
    }


def _dump_groups(world) -> dict:
    import ItemList
    diff = getattr(world, "difficulty_requirements", None)
    diff = diff[1] if diff else None
    lim = {k: getattr(diff, k, None) for k in
           ("progressive_sword_limit", "progressive_shield_limit",
            "progressive_armor_limit", "progressive_bow_limit",
            "progressive_bottle_limit", "boss_heart_container_limit",
            "heart_piece_limit")}
    return {
        "version": 1,
        "profile": {
            "difficulty": world.difficulty[1],
            "progressive": world.progressive if isinstance(world.progressive, str)
            else world.progressive[1],
            "bow_mode": world.bow_mode[1],
        },
        "limits": {
            "sword": lim["progressive_sword_limit"],
            "shield": lim["progressive_shield_limit"],
            "armor": lim["progressive_armor_limit"],
            "bow": lim["progressive_bow_limit"],
            "bottle": lim["progressive_bottle_limit"],
            "boss_heart_container": lim["boss_heart_container_limit"],
            "heart_piece": lim["heart_piece_limit"],
        },
        "progressive": {
            "Progressive Sword": {"tiers": ["Fighter Sword", "Master Sword",
                                            "Tempered Sword", "Golden Sword"]},
            "Progressive Shield": {"tiers": ["Blue Shield", "Red Shield",
                                             "Mirror Shield"],
                                   "counter": "Shield Level"},
            "Progressive Glove": {"tiers": ["Power Glove", "Titans Mitts"]},
            "Progressive Bow": {"tiers": ["Bow", "Silver Arrows"]},
            "Progressive Armor": {"tiers": ["Blue Mail", "Red Mail"]},
        },
        "bottle_family": {
            "prefix": "Bottle",
            "members": list(ItemList.normalbottles),
            "hard_members": list(ItemList.hardbottles),
        },
        "collect_semantics": (
            "CollectionState.collect(item): items named 'Progressive X' fold "
            "into the next missing tier of the chain (guarded by the profile "
            "limit: e.g. a 4th Progressive Sword with sword limit 3 is "
            "dropped); 'Bottle*' items count as DISTINCT names and are capped "
            "at limits.bottle; every other item with item.advancement (or an "
            "event location) bumps its own name counter. has() is a plain "
            "counter lookup; bottle_count() counts distinct 'Bottle*' names."),
    }



def dump_fill_data(world, outdir: str) -> dict:
    """Write the five schema-gap files alongside the core dump."""
    keydoors = _dump_keydoors(world)
    shops = _dump_shops(world)
    barriers = _dump_barriers(world)
    vanilla = _dump_vanilla_locations(world)
    groups = _dump_groups(world)
    for fname, data in [("keydoors.json", keydoors),
                        ("shops.json", shops),
                        ("barriers.json", barriers),
                        ("vanilla_locations.json", vanilla),
                        ("groups.json", groups)]:
        with open(os.path.join(outdir, fname), "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1, default=str)
    return {
        "key_doors": len(keydoors["doors"]),
        "shops": len(shops["shops"]),
        "barrier_doors": len(barriers["doors"]),
        "barrier_rule_probes": len(barriers["rule_probes"]),
        "vanilla_locations": vanilla["counts"]["locations"],
        "vanilla_picked": vanilla["counts"]["picked"],
    }


# ---------------------------------------------------------------------------
# Boss placement (bosses.json)
# ---------------------------------------------------------------------------
#
# `--setting shufflebosses=simple|full|random` picks a boss per dungeon boss
# spot with the world's own RNG, so the answer is a property of the SEED, not
# of the settings: the engine cannot recompute it and has to be told.  This
# dumps the finished assignment straight off the generated World, the way
# BaseClasses.Spoiler does - including Ganon's Tower, which owns three boss
# spots ('bottom' / 'middle' / 'top') whose dungeon object can move under door
# shuffle, so each level is looked up by searching the dungeons for it.
#
# Room ids are the underworld super-tiles from
# source/dungeon/RoomList.boss_rooms + gt_boss_room; they are spelled out here
# so bosses.json is self-contained for the C side (which keys its sprite-list
# override by room).

_BOSS_SPOTS = [
    # (dungeon name, GT level or None, super-tile room id, vanilla boss)
    ("Eastern Palace", None, 0xC8, "Armos Knights"),
    ("Desert Palace", None, 0x33, "Lanmolas"),
    ("Tower of Hera", None, 0x07, "Moldorm"),
    ("Palace of Darkness", None, 0x5A, "Helmasaur King"),
    ("Swamp Palace", None, 0x06, "Arrghus"),
    ("Skull Woods", None, 0x29, "Mothula"),
    ("Thieves Town", None, 0xAC, "Blind"),
    ("Ice Palace", None, 0xDE, "Kholdstare"),
    ("Misery Mire", None, 0x90, "Vitreous"),
    ("Turtle Rock", None, 0xA4, "Trinexx"),
    ("Ganons Tower", "bottom", 0x1C, "Armos Knights"),
    ("Ganons Tower", "middle", 0x6C, "Lanmolas"),
    ("Ganons Tower", "top", 0x4D, "Moldorm"),
]


def dump_bosses(world, outdir: str, player: int = 1) -> dict:
    """Write bosses.json: the boss standing in every dungeon boss spot."""
    spots = []
    for dungeon, level, room, default in _BOSS_SPOTS:
        if level is None:
            boss = world.get_dungeon(dungeon, player).boss
        else:
            owner = next((d for d in world.dungeons
                          if d.player == player and level in d.bosses), None)
            boss = owner.bosses[level] if owner else None
        spots.append({
            "dungeon": dungeon,
            "spot": level,
            "room": room,
            "default": default,
            # a spot is never empty: Dungeons.create_dungeons seeds every one
            # with its vanilla boss before place_bosses() may replace it
            "boss": boss.name if boss else default,
        })
    data = {
        "dump_version": 1,
        "shuffle": world.boss_shuffle[player],
        "seed": int(world.seed),
        "spots": spots,
    }
    with open(os.path.join(outdir, "bosses.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, default=str)
    return data


# ---------------------------------------------------------------------------
# Enemy key drops (drops.json)
# ---------------------------------------------------------------------------
#
# `--setting dropshuffle=keys` turns the reference's fourteen "... Key Drop"
# locations (the small keys guards, rats, bari and pokeys drop, plus Hyrule
# Castle's big key) into real item slots.  The reference names them in words
# ("Hyrule Castle - Key Rat Key Drop"); the engine only ever knows which
# underworld super-tile it is standing in and which sprite of that room's
# list just died, so the dump has to carry the bridge between the two.
#
# PotShuffle.key_drop_data stores each drop as (snes_address, super_tile,
# sprite_index), where sprite_index indexes the room's FULL sprite list
# (world.data_tables[p].uw_enemy_table.room_map[room]) - overlords included.
# The engine's Dungeon_LoadSingleSprite (src/sprite.c) hands out a slot only
# to real sprites: an overlord record (x byte >= 0xe0, i.e. sub_type & 7 == 7)
# returns without consuming one, and so does the 0xe4 "this one drops a key"
# marker - which is not a room_map entry at all, the reference models it as
# the sprite's own drops_item / drop_item_kind pair.  So the engine's slot is
# the reference index minus the overlords in front of it, and that is what
# "slot" holds here; "index" keeps the reference's own number for reference.
#
# "kind" is the sprite id the engine must find in that slot for the match to
# be trusted (it checks, and leaves the drop vanilla when it disagrees), and
# "big" says the enemy drops the big key (engine die_action 2, sprite 0xe5)
# rather than a small key (die_action 1, sprite 0xe4).
#
# The file is written for EVERY dump, whatever dropshuffle is, so a rule
# folder always describes its own drops; "shuffle" says whether the fill was
# allowed to touch them.


def dump_drops(world, outdir: str, player: int = 1) -> dict:
    """Write drops.json: the enemy behind every "... Key Drop" location."""
    from PotShuffle import key_drop_data

    room_map = world.data_tables[player].uw_enemy_table.room_map
    drops = []
    for name, datum in sorted(key_drop_data.items()):
        if datum[0] != "Drop":
            continue
        address, room, index = datum[1]
        sprites = room_map.get(room) or []
        # prefer the LIVE binding: Regions.adjust_locations pins the location
        # onto the sprite object, so an enemy the generator moved (enemy
        # shuffle, boss shuffle) is still found at its real index
        live = next((i for i, s in enumerate(sprites)
                     if getattr(getattr(s, "location", None), "name", None) == name), None)
        if live is not None:
            index = live
        sprite = sprites[index] if index < len(sprites) else None
        drops.append({
            "name": name,
            "room": room,
            "index": index,
            # engine sprite slot: overlords in front of it do not take one
            "slot": sum(1 for s in sprites[:index] if (s.sub_type & 7) != 7),
            "kind": sprite.kind if sprite else None,
            "sub_type": sprite.sub_type if sprite else None,
            "big": bool(sprite is not None and sprite.drop_item_kind == 0xE5),
            "vanilla": datum[3],
            "address": hex(address),
        })
    data = {
        "dump_version": 1,
        "shuffle": world.dropshuffle[player],
        "seed": int(world.seed),
        "drops": drops,
    }
    with open(os.path.join(outdir, "drops.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, default=str)
    return data


# `--setting pottery=keys` turns the reference's nineteen "... Pot Key"
# locations into real item slots (CLI.py takes pottery as key=value, choices
# none/keys/dungeon/cave/cavekeys/reduced/clustered/nonempty/lottery - it is
# NOT a bare store_true flag).  Only `keys` is dumped for the engine's sake:
# it leaves every OTHER pot exactly where vanilla has it.
#
# PotShuffle.key_drop_data gives a Pot location only its underworld ROOM
# (e.g. 0x37); the pot inside that room is "the vanilla_pots entry whose item
# is PotItem.Key".  The engine finds a pot's contents in kDungeonSecrets, a
# per-room list of 3-byte (x, y|flags, item) records - which is byte for byte
# what Pot.pot_data() produces - and the engine compares the first two bytes
# as one little-endian word against dung_object_tilemap_pos.  So "pos" here
# is that word, and it is the engine's own name for the pot.
#
# In practice the ROOM alone is already unique: the engine's asset holds
# exactly nineteen item==8 (key) secret records in the whole game, one in
# each of these nineteen rooms, so the engine identifies a pot key by room
# and uses "pos" only to verify the dump against its own asset at load time.
#
# One trap: "Ice Palace - Hammer Block Key Drop" is spelled like an enemy
# drop but is type "Pot" (a key under a block, PotFlags.Block, no room object
# of its own) - it belongs here, not in drops.json's fourteen.
#
# Written for EVERY dump, whatever pottery is, so a rule folder always
# describes its own pots; "shuffle" says whether the fill was allowed to
# touch them.


def dump_pots(world, outdir: str, player: int = 1) -> dict:
    """Write pots.json: the pot behind every "... Pot Key" location."""
    from PotShuffle import key_drop_data, vanilla_pots
    from BaseClasses import PotItem

    pots = []
    for name, datum in sorted(key_drop_data.items()):
        if datum[0] != "Pot":
            continue
        room = datum[1]
        cands = [p for p in vanilla_pots.get(room, []) if p.item == PotItem.Key]
        pot = cands[0] if len(cands) == 1 else None
        data = pot.pot_data() if pot else None
        pots.append({
            "name": name,
            "room": room,
            # the kDungeonSecrets record: x, y (|0x20 when the pot lives in
            # the room's lower region), item (8 = key)
            "x": data[0] if data else None,
            "y": data[1] if data else None,
            "item": data[2] if data else None,
            # the little-endian word the engine matches on
            "pos": (data[1] << 8) | data[0] if data else None,
            "candidates": len(cands),
            "vanilla": datum[3],
        })
    out = {
        "dump_version": 1,
        "shuffle": world.pottery[player],
        "seed": int(world.seed),
        "pots": pots,
    }
    with open(os.path.join(outdir, "pots.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, default=str)
    return out


# `bonk_drops` is a BARE FLAG in the reference (CLI.py lists it with the other
# store_true settings and resources/app/cli/args.json gives it
# {"action": "store_true", "type": "bool"}), so it is "--setting bonk_drops",
# never "bonk_drops=on".
#
# With it on, ItemList.create_dynamic_bonkdrop_locations turns the 42 rows of
# Regions.py bonk_prize_table into real item slots of LocationType.Bonk, with
# the address 0x2abb00 + record*6 + 3 (a record in the reference's own custom
# ROM table - nothing this engine has).  So the ONLY thing the reference can
# tell the engine about a bonk drop is its NAME, its region and whether it
# needs Agahnim dead; the engine coordinates (which overworld sprite is that
# tree?) live on the C side, in kBonkOwSpots / kBonkUwSpots in randomizer.c,
# and are cross-checked against kOverworldSprites / kDungeonSprites at load.
#
# Written for EVERY dump, whatever bonk_drops is, so a rule folder always
# describes its own bonk set; "shuffle" says whether the fill was allowed to
# touch them.  The 42 rows are constant - they come from bonk_prize_table, not
# from the generated world - which is exactly what makes them usable as the
# engine's coverage check.


def dump_bonks(world, outdir: str, player: int = 1) -> dict:
    """Write bonks.json: the reference's 42 bonk / tree-pull prize slots."""
    from Regions import bonk_prize_table

    bonks = []
    for name, datum in bonk_prize_table.items():
        record, flag, aga, _default, region, hint = datum
        bonks.append({
            "name": name,
            # the reference's own id for the slot; the address below is
            # derived from it and means nothing to this engine
            "record": record,
            # which bit of the overworld event flag byte the reference's ASM
            # burns to remember the slot was taken
            "flag": flag,
            "aga": bool(aga),
            "region": region,
            "hint": hint,
            "address": hex(0x2abb00 + record * 6 + 3),
        })
    bonks.sort(key=lambda b: b["record"])
    out = {
        "dump_version": 1,
        "shuffle": bool(world.shuffle_bonk_drops[player]),
        "seed": int(world.seed),
        "bonks": bonks,
    }
    with open(os.path.join(outdir, "bonks.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, default=str)
    return out


# ---------------------------------------------------------------------------
# Entrance shuffle (entrances.json)
# ---------------------------------------------------------------------------
#
# `--setting shuffle=simple|restricted|full|...` rewires which overworld door
# leads to which interior.  Like boss shuffle the answer is rolled with the
# generator's own RNG, so it is a property of the SEED and the engine has to
# be told.  edges.json already carries the resulting LOGIC graph; what it does
# not carry is the three ROM-level tables the reference rewrites in Rom.py
# ("patch entrance/exits/holes", the loop over region.exits), which is exactly
# what the C engine has to mirror:
#
#   entrance.addresses is an int  -> index into the overworld door table
#       ($DBB73 in the ROM = the port's kOverworld_Entrance_Id); the byte
#       written is entrance.target, the entrance id the port puts in
#       `which_entrance` and feeds to Dungeon_LoadEntrance.
#   exit.addresses is a 13-tuple -> the OVERWORLD SPOT Link comes back out at;
#       exit.target is the row of the exit table to overwrite ($15AEE.. in the
#       ROM = the port's kExitData_* arrays, found by room in
#       LoadOverworldFromDungeon).  The room id is deliberately NOT written by
#       the reference, so the row keeps its identity.
#   entrance.addresses is a list -> a hole/drop; each address is one byte of
#       the 19-entry fall-hole table at $DB84C (the port's
#       kFallHole_Entrances), and entrance.target is the entrance id.
#
# Everything here is read straight off the generated World; no logic of our
# own.  The link_y fix-ups are the reference's own (Rom.py), replicated so the
# C side never has to know about them.
_HOLE_TABLE_BASE = 0xDB84C          # $DB84C, 19 bytes: kFallHole_Entrances
_HOLE_TABLE_LEN = 19

_EXITDATA_FIELDS = ("room", "ow_area", "vram_loc", "scroll_y", "scroll_x",
                    "link_y", "link_x", "camera_y", "camera_x",
                    "unknown_1", "unknown_2", "door_1", "door_2")


def _exit_link_y(world, exit, room_id, link_y, player):
    """Rom.py's link_y fix-ups for exits, so entrances.json is final."""
    if world.shuffle[player] not in ['insanity'] and exit.name in [
            'Eastern Palace Exit', 'Tower of Hera Exit', 'Thieves Town Exit',
            'Ice Palace Exit', 'Misery Mire Exit', 'Palace of Darkness Exit',
            'Swamp Palace Exit', 'Ganons Tower Exit', 'Desert Palace Exit (North)',
            'Agahnims Tower Exit', 'Spiral Cave Exit (Top)',
            'Superbunny Cave Exit (Bottom)', 'Turtle Rock Ledge Exit (East)']:
        return link_y
    if room_id == 0x0059 and world.fix_skullwoods_exit[player]:
        return 0x00F8
    if room_id == 0x004a and world.fix_palaceofdarkness_exit[player]:
        return 0x0640
    if room_id == 0x00d6 and world.fix_trock_exit[player]:
        return 0x0134
    if room_id == 0x000c and world.fix_gtower_exit[player]:
        return 0x00A4
    return link_y


def dump_entrances(world, outdir: str, player: int = 1) -> dict:
    """Write entrances.json: the finished overworld door / exit / hole wiring."""
    doors, exits, holes, unhandled = [], [], [], []
    for region in world.regions:
        for exit in region.exits:
            if exit.target is None or exit.player != player:
                continue
            dest = exit.connected_region.name if exit.connected_region else None
            if isinstance(exit.addresses, tuple):
                d = dict(zip(_EXITDATA_FIELDS, exit.addresses))
                d["link_y"] = _exit_link_y(world, exit, d["room"], d["link_y"], player)
                exits.append({
                    "name": exit.name,
                    "index": int(exit.target),
                    "from": region.name,
                    "to": dest,
                    "data": d,
                })
            elif isinstance(exit.addresses, list):
                slots = [a - _HOLE_TABLE_BASE for a in exit.addresses]
                holes.append({
                    "name": exit.name,
                    "from": region.name,
                    "to": dest,
                    "target": int(exit.target),
                    "slots": [s for s in slots if 0 <= s < _HOLE_TABLE_LEN],
                    "extra": [hex(a) for a in exit.addresses
                              if not 0 <= a - _HOLE_TABLE_BASE < _HOLE_TABLE_LEN],
                })
            elif isinstance(exit.addresses, int):
                doors.append({
                    "name": exit.name,
                    "door": int(exit.addresses),
                    "target": int(exit.target),
                    "from": region.name,
                    "to": dest,
                    # Rom.py mirrors this one byte into the tavern back door
                    "tavern_north": exit.name == 'Tavern North',
                })
            else:
                unhandled.append({"name": exit.name,
                                  "addresses": repr(exit.addresses),
                                  "target": repr(exit.target)})
    doors.sort(key=lambda e: (e["door"], e["name"]))
    exits.sort(key=lambda e: (e["index"], e["name"]))
    holes.sort(key=lambda e: (e["slots"], e["name"]))
    data = {
        "dump_version": 1,
        "shuffle": world.shuffle[player],
        "mode": world.mode[player],
        "door_shuffle": world.doorShuffle[player],
        "seed": int(world.seed),
        "hole_table_base": hex(_HOLE_TABLE_BASE),
        "hole_table_len": _HOLE_TABLE_LEN,
        "doors": doors,
        "exits": exits,
        "holes": holes,
        "unhandled": unhandled,
    }
    with open(os.path.join(outdir, "entrances.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, default=str)
    return data


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def run_seed(seed: int, opts, repo: str, versions: dict) -> dict:
    from CLI import parse_cli
    from Main import main as generate
    from source.classes.BabelFish import BabelFish

    argv = [
        f"--seed={seed}",
        f"--multi={opts.players}",
        f"--logic={opts.logic}",
        f"--mode={opts.mode}",
        f"--goal={opts.goal}",
        f"--door_shuffle={opts.door_shuffle}",
        f"--shuffle={opts.shuffle}",
        "--suppress_rom",
        "--spoiler=none",
        f"--loglevel={opts.loglevel}",
        f"--outputpath={opts.out}",
    ]
    for s in opts.setting or []:
        argv.append(f"--{s.replace('=', '=')}" if "=" in s else f"--{s}")

    args = parse_cli(argv)
    world = generate(args=args, seed=seed, fish=BabelFish())

    outdir = os.path.join(opts.out, f"seed_{seed}")
    meta = dump_world(world, outdir, repo, versions)
    bosses = dump_bosses(world, outdir)
    drops = dump_drops(world, outdir)
    pots = dump_pots(world, outdir)
    entrances = dump_entrances(world, outdir)
    bonks = dump_bonks(world, outdir)
    if opts.with_fill_data:
        counts = dump_fill_data(world, outdir)
        meta = dict(meta)
        meta["counts"] = dict(meta["counts"], **counts)
    log.info("seed %d bosses (%s): %s", seed, bosses["shuffle"],
             json.dumps({s["dungeon"] + (" (" + s["spot"] + ")" if s["spot"] else ""): s["boss"]
                         for s in bosses["spots"]}))
    log.info("seed %d key drops (dropshuffle=%s): %d located",
             seed, drops["shuffle"],
             sum(1 for d in drops["drops"] if d["kind"] is not None))
    log.info("seed %d pot keys (pottery=%s): %d located",
             seed, pots["shuffle"],
             sum(1 for d in pots["pots"] if d["pos"] is not None))
    log.info("seed %d entrances (shuffle=%s): %d doors, %d exits, %d holes%s",
             seed, entrances["shuffle"], len(entrances["doors"]),
             len(entrances["exits"]), len(entrances["holes"]),
             "" if not entrances["unhandled"] else
             " (%d UNHANDLED)" % len(entrances["unhandled"]))
    log.info("seed %d bonk drops (bonk_drops=%s): %d slots",
             seed, bonks["shuffle"], len(bonks["bonks"]))
    log.info("seed %d counts: %s", seed, json.dumps(meta["counts"]))
    log.info("seed %d rules: %s", seed, json.dumps(meta["rules"]))
    return meta


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo", default=DEFAULT_REPO)
    p.add_argument("--out", default=DEFAULT_OUT)
    p.add_argument("--seed", type=int, nargs="+", default=[1234, 5678],
                   help="one or more seeds (default: 1234 5678)")
    p.add_argument("--players", type=int, default=1)
    p.add_argument("--logic", default="noglitches",
                   choices=["noglitches", "minorglitches", "owglitches",
                            "hybridglitches", "nologic"])
    p.add_argument("--mode", default="open",
                   choices=["open", "standard", "inverted", "retro"])
    p.add_argument("--goal", default="ganon")
    p.add_argument("--door_shuffle", default="vanilla",
                   choices=["basic", "partitioned", "crossed", "vanilla"])
    p.add_argument("--shuffle", default="vanilla",
                   choices=["vanilla", "simple", "restricted", "full", "lite",
                            "lean", "district", "swapped", "crossed",
                            "insanity", "dungeonsfull", "dungeonssimple"])
    p.add_argument("--loglevel", default="error",
                   choices=["info", "error", "warning", "debug"])
    p.add_argument("--setting", action="append", metavar="key=value",
                   help="extra randomizer setting passed through (e.g. "
                        "--setting pottery=keys)")
    p.add_argument("--with-fill-data", action="store_true",
                   help="also dump keydoors.json, shops.json, barriers.json, "
                        "vanilla_locations.json and groups.json (phase-1 "
                        "schema-gap data for the fill-algorithm port)")
    opts = p.parse_args()

    logging.basicConfig(format="%(levelname)s %(name)s %(message)s",
                        level=logging.INFO)
    # resolve paths against the INVOKING cwd: setup_repo() chdirs into the
    # reference repo, so relative --out values would otherwise land there.
    opts.repo = os.path.abspath(opts.repo)
    opts.out = os.path.abspath(opts.out)
    setup_repo(opts.repo)

    from Main import __version__ as dr_version
    from OverworldShuffle import __version__ as or_version
    versions = {"dr": dr_version, "or": or_version, "git": git_hash(opts.repo)}
    log.info("reference repo: %s @ %s (%s / OR %s)", opts.repo,
             versions["git"], versions["dr"], versions["or"])

    os.makedirs(opts.out, exist_ok=True)
    for seed in opts.seed:
        run_seed(seed, opts, opts.repo, versions)
    log.info("done -> %s", opts.out)


if __name__ == "__main__":
    main()

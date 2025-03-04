""" Data structures for representing grammars. """

import collections
from .ordered import OrderedSet, OrderedFrozenSet
from . import types


# *** What is a grammar? ******************************************************
#
# A grammar is a dictionary mapping nonterminal names to lists of right-hand
# sides. Each right-hand side (also called a "production") is a list whose
# elements can include terminals, nonterminals, Optional elements, LookaheadRules,
# and NoLineTerminatorHere.
#
# The most common elements are terminals and nonterminals, so a grammar usually
# looks something like this:
def example_grammar():
    rules = {
        'expr': [
            ['term'],
            ['expr', '+', 'term'],
            ['expr', '-', 'term'],
        ],
        'term': [
            ['unary'],
            ['term', '*', 'unary'],
            ['term', '/', 'unary'],
        ],
        'unary': [
            ['prim'],
            ['-', 'unary'],
        ],
        'prim': [
            ['NUM'],
            ['VAR'],
            ['(', 'expr', ')'],
        ],
    }

    # The goal nonterminals are the nonterminals we're actually interested in
    # parsing. Here we want to parse expressions; all the other nonterminals
    # are interesting only as the building blocks of expressions.
    #
    # Variable terminals are terminal symbols that can have several different
    # values, like a VAR token that could be any identifier, or a NUM token
    # that could be any number.
    return Grammar(rules, goal_nts=['expr'], variable_terminals=['NUM', 'VAR'])


# A production consists of a left side, an optional condition, a right side,
# and a reducer. A `Production` object includes everything except the left
# side. Incorporating reducers lets us transform a grammar while preserving
# behavior.
#
# The production `expr ::= term` is represented by
# `Production(["term"], 0)`.
#
# The production `expr ::= expr + term => add($0, $2)` is represented by
# `Production(["expr", "+", "term"], CallMethod("add", (0, 2))`.
#
class Production:
    __slots__ = ['body', 'reducer', 'condition']

    def __init__(self, body, reducer, *, condition=None):
        self.body = body
        self.reducer = reducer
        self.condition = condition

    def __eq__(self, other):
        return (self.body == other.body
                and self.reducer == other.reducer
                and self.condition == other.condition)

    __hash__ = None

    def __repr__(self):
        if self.condition is None:
            return "Production({!r}, reducer={!r})".format(self.body, self.reducer)
        else:
            return ("Production({!r}, reducer={!r}, condition={!r})"
                    .format(self.body, self.reducer, self.condition))

    def copy_with(self, **kwargs):
        return Production(body=kwargs.get('body', self.body),
                          reducer=kwargs.get('reducer', self.reducer),
                          condition=kwargs.get('condition', self.condition))


# *** Reduce expressions ******************************************************
#
# Reduce expressions ("reducers" for short) are a little language used to
# specify what happens when a production is matched. A reduce expression is
# one of these:
#
# *   An integer evaluates to one of the syntactic components of the
#     production. For example, if the production we're reducing is
#     `sum ::= sum + term`, the integer `0` evaluates the `sum` to the left of
#     the plus sign, and `2` means the `term` on the right. (The integer
#     `1` would refer to the `+` token itself, but that's not super useful.)
#
#     These integers are not quite indexes into `production.body`, because
#     LookaheadRule, ErrorSymbol, and NoLineTerminatorHere elements don't
#     count: in the production `stmt ::= [lookahead != "let"] expr ";"`, `0` is
#     the expr, and `1` is the semicolon token.  See `is_concrete_element(e)`.
#
# *   CallMethod objects pass values to a builder method and return the result.
#     The `args` are nested reduce expressions.
#
# *   None is an expression used as a placeholder when an optional symbol is
#     omitted.
#
# *   Some(expr) is used when an optional symbol is found and parsed.
#     In Python, this just expands to the same thing as `expr`, but in Rust
#     this expands to a use of `Option::Some()`.
#
# In addition, the special reducer 'accept' means stop parsing. This is
# used only in productions for init nonterminals, created automatically by
# Grammar.__init__(). It's not a reduce expression, so it can't be nested.
#
CallMethod = collections.namedtuple("CallMethod", "method args")
Some = collections.namedtuple("Some", "inner")


def expr_to_str(expr):
    if isinstance(expr, int):
        return "${}".format(expr)
    elif isinstance(expr, CallMethod):
        return "{}({})".format(
            expr.method,
            ', '.join(expr_to_str(arg) for arg in expr.args))
    elif expr is None:
        return "None"
    elif isinstance(expr, Some):
        return "Some({})".format(expr_to_str(expr.inner))
    elif expr == "accept":
        return "<accept>"
    else:
        raise ValueError("unrecognized expression: {!r}".format(expr))


class Grammar:
    """A collection of productions.

    *   self.variable_terminals - OrderedFrozenSet(str) - Terminals that carry
        data, like (in JS) numeric literals and RegExps.

    *   self.terminals - OrderedFrozenSet(str) - All terminals used in the
        language, including those in self.variable_terminals and
        self.synthetic_terminals.

    *   self.synthetic_terminals - {str: OrderedFrozenSet(str)} - Maps terminals
        to sets of terminals. An entry `name: set` in this dictionary means
        that `name` is shorthand for "one of the terminals in `set`".

    *   self.nonterminals - {key: NtDef} - Keys are either (str|InitNt), early
        in the pipeline, or Nt objects later on. Values are the NtDef objects
        that contain the actual Productions.

    *   self.methods - {str: MethodType} - Type information for methods.

    *   self.init_nts - [InitNt or Nt] - The list of all elements of
        self.nonterminals.keys() that are init nonterminals.

    *   self._cache - {object: object} - Cache of immutable objects used by
        Grammar.intern().

    """

    def __init__(
            self,
            nonterminals,
            *,
            goal_nts=None,
            variable_terminals=(),
            synthetic_terminals=None,
            method_types=None):

        # This constructor supports passing in a sort of jumbled blob of
        # strings, lists, and actual objects, and normalizes it all to a more
        # typeful structure. Being able to interpret simple
        # "list-of-lists-of-strings" input is super useful for tests.
        #
        # We don't check here that the grammar is LR, that it's cycle-free, or
        # any other nice properties.

        # Copy/infer the arguments.
        nonterminals = dict(nonterminals.items())
        if goal_nts is None:
            # Default to the first nonterminal in the dictionary.
            goal_nts = []
            for name in nonterminals:
                goal_nts.append(name)
                break
        else:
            goal_nts = list(goal_nts)
        self.variable_terminals = OrderedFrozenSet(variable_terminals)
        if synthetic_terminals is None:
            synthetic_terminals = {}
        else:
            synthetic_terminals = {
                name: OrderedFrozenSet(set)
                for name, set in synthetic_terminals.items()
            }
            for key, values in synthetic_terminals.items():
                if key in nonterminals:
                    raise ValueError(
                        "invalid grammar: {!r} is both a terminal and a nonterminal"
                        .format(key))
                for t in values:
                    if t in nonterminals:
                        raise ValueError(
                            "invalid grammar: {!r} is both ".format(t)
                            + "a representation of a synthetic terminal and a nonterminal")
                    if t in synthetic_terminals:
                        # Nested synthetic terminals. Throw, for now. (This
                        # should pretty much just work, except expand_terminal
                        # doesn't support it; and we don't check for cycles
                        # where a synthetic terminal includes itself directly
                        # or indirectly.)
                        raise ValueError(
                            "unsupported: synthetic terminals can't include other "
                            "synthetic terminals; {!r} includes {!r}"
                            .format(key, t))
        # self.synthetic_terminals = synthetic_terminals
        self.synthetic_terminals = {}

        keys_are_nt = isinstance(next(iter(nonterminals)), Nt)
        key_type = Nt if keys_are_nt else (str, InitNt)

        self._cache = {}

        # Gather some information just by looking at keys (without examining
        # every production).
        #
        # str_to_nt maps the name of each non-parameterized
        # nonterminal to `Nt(name)`, a cache.
        str_to_nt = {}  # {str: Nt}
        # nt_params lists the names of each nonterminal's parameters (empty
        # tuple for non-parameterized nts).
        nt_params = {}  # {str: tuple(str)}
        for key in nonterminals:
            if not isinstance(key, key_type):
                raise ValueError(
                    "invalid grammar: conflicting key types in nonterminals dict - "
                    "expected either all str or all Nt, got {!r}"
                    .format(key.__class__.__name__))
            if keys_are_nt:
                name = key.name
                param_names = tuple(name for name, value in key.args)
            else:
                name = key
                param_names = ()
                if isinstance(nonterminals[key], NtDef):
                    param_names = tuple(nonterminals[key].params)
            if name not in nt_params:
                nt_params[name] = param_names
            else:
                if nt_params[name] != param_names:
                    raise ValueError(
                        "conflicting parameter name lists for nt {!r}: "
                        "both {!r} and {!r}"
                        .format(name, nt_params[name], param_names))
            if param_names == () and name not in str_to_nt:
                str_to_nt[name] = self.intern(Nt(name))

        # Validate, desugar, and copy the grammar. As a side effect, calling
        # validate_element on every element of the grammar populates
        # all_terminals.
        all_terminals = OrderedSet(self.variable_terminals)
        all_terminals.add(End())

        def note_terminal(t):
            """Add t (and all representations of it, if synthetic) to all_terminals."""
            if t not in all_terminals:
                all_terminals.add(t)
                if t in synthetic_terminals:
                    for k in synthetic_terminals[t]:
                        note_terminal(k)

        def validate_element(nt, i, j, e, context_params):
            if isinstance(e, str):
                if e in nt_params:
                    if nt_params[e] != ():
                        raise ValueError(
                            "invalid grammar: missing parameters for {!r} "
                            "in production `grammar[{!r}][{}][{}].inner`: {!r}"
                            .format(e, nt, i, j, nt_params[e]))
                    return str_to_nt[e]
                else:
                    note_terminal(e)
                    return e
            elif isinstance(e, Optional):
                if not isinstance(e.inner, (str, Nt)):
                    raise TypeError(
                        "invalid grammar: unrecognized element "
                        "in production `grammar[{!r}][{}][{}].inner`: {!r}"
                        .format(nt, i, j, e.inner))
                inner = validate_element(nt, i, j, e.inner, context_params)
                return self.intern(Optional(inner))
            elif isinstance(e, Literal):
                if not isinstance(e.text, str):
                    raise TypeError(
                        "invalid grammar: unrecognized element "
                        "in production `grammar[{!r}][{}][{}].text`: {!r}"
                        .format(nt, i, j, e.text))
                return self.intern(e)
            elif isinstance(e, UnicodeCategory):
                if not isinstance(e.cat_prefix, str):
                    raise TypeError(
                        "invalid grammar: unrecognized element "
                        "in production `grammar[{!r}][{}][{}].cat_prefix`: {!r}"
                        .format(nt, i, j, e.cat_prefix))
                return self.intern(e)
            elif isinstance(e, Exclude):
                if not isinstance(e.inner, (str, Nt)):
                    raise TypeError(
                        "invalid grammar: unrecognized element "
                        "in production `grammar[{!r}][{}][{}].inner`: {!r}"
                        .format(nt, i, j, e.inner))
                exclusion_list = []
                for value in e.exclusion_list:
                    if not isinstance(value, (str, Nt)):
                        raise TypeError(
                            "invalid grammar: unrecognized element "
                            "in production `grammar[{!r}][{}][{}].exclusion_list`: {!r}"
                            .format(nt, i, j, value))
                    value = validate_element(nt, i, j, value, context_params)
                    exclusion_list.append(value)
                inner = validate_element(nt, i, j, e.inner, context_params)
                return self.intern(Exclude(inner, tuple(exclusion_list)))
            elif isinstance(e, Nt):
                # Either the application or the original parameterized
                # production must be present in the dictionary.
                if e not in nonterminals and e.name not in nonterminals:
                    raise ValueError(
                        "invalid grammar: unrecognized nonterminal "
                        "in production `grammar[{!r}][{}][{}]`: {!r}"
                        .format(nt, i, j, e.name))
                args = tuple(pair[0] for pair in e.args)
                if e.name in nt_params and args != nt_params[e.name]:
                    raise ValueError(
                        "invalid grammar: wrong arguments passed to {!r} "
                        "in production `grammar[{!r}][{}][{}]`: "
                        "passed {!r}, expected {!r}"
                        .format(e.name, nt, i, j,
                                args, nt_params[e.name]))
                for param_name, arg_expr in e.args:
                    if isinstance(arg_expr, Var):
                        if arg_expr.name not in context_params:
                            raise ValueError(
                                "invalid grammar: undefined variable {!r} "
                                "in production `grammar[{!r}][{}][{}]`"
                                .format(arg_expr.name, nt, i, j))
                return self.intern(e)
            elif isinstance(e, (LookaheadRule, End, ErrorSymbol)):
                return self.intern(e)
            elif e is NoLineTerminatorHere:
                return e
            else:
                raise TypeError(
                    "invalid grammar: unrecognized element in production "
                    "`grammar[{!r}][{}][{}]`: {!r}"
                    .format(nt, i, j, e))
            assert False, "unreachable"

        def check_reduce_expr(nt, i, rhs, expr):
            if isinstance(expr, int):
                concrete_len = sum(1 for e in rhs.body
                                   if is_concrete_element(e))
                if not (0 <= expr < concrete_len):
                    raise ValueError(
                        "invalid grammar: element number {} out of range for "
                        "production {!r} in grammar[{!r}][{}].reducer ({!r})"
                        .format(expr, nt, rhs.body, i, rhs.reducer))
            elif isinstance(expr, CallMethod):
                if not isinstance(expr.method, str):
                    raise TypeError(
                        "invalid grammar: method names must be strings, "
                        "not {!r}, in grammar[{!r}[{}].reducer"
                        .format(expr.method, nt, i))
                if not expr.method.isidentifier():
                    name, space, pn = expr.method.partition(' ')
                    if space == ' ' and name.isidentifier() and pn.isdigit():
                        pass
                    else:
                        raise ValueError(
                            "invalid grammar: invalid method name {!r} "
                            "(not an identifier), in grammar[{!r}[{}].reducer"
                            .format(expr.method, nt, i))
                for arg_expr in expr.args:
                    check_reduce_expr(nt, i, rhs, arg_expr)
            elif expr is None:
                pass
            elif isinstance(expr, Some):
                check_reduce_expr(nt, i, rhs, expr.inner)
            else:
                raise TypeError(
                    "invalid grammar: unrecognized reduce expression {!r} "
                    "in grammar[{!r}][{}].reducer"
                    .format(expr, nt, i))

        def copy_rhs(nt, i, sole_production, rhs, context_params):
            if isinstance(rhs, list):
                # Bare list, no reducer. Desugar to a Production, inferring a
                # reasonable default reducer.
                nargs = sum(1 for e in rhs if is_concrete_element(e))
                if len(rhs) == 1 and nargs == 1:
                    reducer = 0  # don't call a method, just propagate the value
                else:
                    # Call a method named after the production. If the
                    # nonterminal has exactly one production, there's no need
                    # to include the production index `i` to the method name.
                    if sole_production:
                        method = nt
                    else:
                        method = '{}_{}'.format(nt, i)
                    reducer = CallMethod(method, args=tuple(range(nargs)))
                rhs = Production(rhs, reducer)

            if not isinstance(rhs, Production):
                raise TypeError(
                    "invalid grammar: grammar[{!r}][{}] should be "
                    "a Production or list of grammar symbols, not {!r}"
                    .format(nt, i, rhs))

            if rhs.condition is not None:
                param, value = rhs.condition
                if param not in context_params:
                    raise TypeError(
                        "invalid grammar: undefined parameter {!r} "
                        "in conditional for grammar[{!r}][{}]"
                        .format(param, nt, i))
            if rhs.reducer != 'accept':
                check_reduce_expr(nt, i, rhs, rhs.reducer)
            assert isinstance(rhs.body, list)
            return rhs.copy_with(body=[
                validate_element(nt, i, j, e, context_params)
                for j, e in enumerate(rhs.body)
            ])

        def copy_nt_def(nt, nt_def, params):
            if isinstance(nt_def, NtDef):
                for i, param in enumerate(nt_def.params):
                    if not isinstance(param, str):
                        raise TypeError(
                            "invalid grammar: parameter {} of {} should be "
                            "a string, not {!r}"
                            .format(i + 1, nt, param))
                params = nt_def.params
                rhs_list = nt_def.rhs_list
                ty = nt_def.type
            else:
                params = ()
                rhs_list = nt_def
                ty = None

            if not isinstance(rhs_list, list):
                raise TypeError(
                    "invalid grammar: grammar[{!r}] should be either a "
                    "list of right-hand sides or NtDef, not {!r}"
                    .format(nt, type(rhs_list).__name__))

            sole_production = len(rhs_list) == 1
            rhs_list = [copy_rhs(nt, i, sole_production, rhs, params)
                        for i, rhs in enumerate(rhs_list)]
            return NtDef(params, rhs_list, ty)

        def check_nt_key(nt):
            if isinstance(nt, str):
                if not nt.isidentifier():
                    raise ValueError(
                        "invalid grammar: nonterminal names must be identifiers, not {!r}"
                        .format(nt))
                if nt in self.variable_terminals or nt in self.synthetic_terminals:
                    raise TypeError(
                        "invalid grammar: {!r} is both a nonterminal and a variable terminal"
                        .format(nt))
            elif isinstance(nt, Nt):
                assert keys_are_nt  # checked earlier
                if not (isinstance(nt.name, (str, InitNt))
                        and isinstance(nt.args, tuple)):
                    raise TypeError(
                        "invalid grammar: expected str or Nt(name=str, "
                        "args=tuple) keys in nonterminals dict, got {!r}"
                        .format(nt))
                check_nt_key(nt.name)
                for pair in nt.args:
                    if (not isinstance(pair, tuple)
                            or len(pair) != 2
                            or not isinstance(pair[0], str)
                            or not isinstance(pair[1], bool)):
                        raise TypeError(
                            "invalid grammar: expected tuple((str, bool)) args, got {!r}"
                            .format(nt))
            elif isinstance(nt, InitNt):
                # Users don't include init nonterminals when initially creating
                # a Grammar. They are automatically added below. But if this
                # Grammar is being created by hacking on a previous Grammar, it
                # will already have them.
                if not isinstance(nt.goal, Nt):
                    raise TypeError(
                        "invalid grammar: InitNt.goal should be a nonterminal, "
                        "got {!r}"
                        .format(nt))
                # nt.goal is a "use", not a "def". Check it like a use.
                # Bogus question marks appear in error messages :-|
                validate_element(nt, '?', '?', nt.goal, [])
                if nt.goal not in goal_nts:
                    raise TypeError(
                        "invalid grammar: nonterminal referenced by InitNt "
                        "is not in the list of goals: {!r}"
                        .format(nt))
            else:
                raise TypeError(
                    "invalid grammar: expected string keys in nonterminals dict, got {!r}"
                    .format(nt))

        def validate_nt(nt, nt_def):
            check_nt_key(nt)
            if isinstance(nt, InitNt):
                # Check the form of init productions. Initially these look like
                # [[goal]], but after the pipeline goes to work, they can be
                # [[Optional(goal)]] or [[], [goal]].
                if not isinstance(nt_def, NtDef):
                    raise TypeError(
                        "invalid grammar: key {!r} must map to "
                        "value of type NtDef, not {!r}"
                        .format(nt, nt_def))
                rhs_list = nt_def.rhs_list
                g = nt.goal
                if (rhs_list != [Production([g], 0),
                                 Production([Nt(nt,()), End()], 'accept')]
                    and rhs_list != [Production([Optional(g)], 0),
                                     Production([Nt(nt,()), End()], 'accept')]
                    and rhs_list != [Production([End()], 'accept'),
                                     Production([g, End()], 'accept'),
                                     Production([Nt(nt,()), End()], 'accept')]):
                    raise ValueError(
                        "invalid grammar: grammar[{!r}] is not one of "
                        "the expected forms: got {!r}"
                        .format(nt, rhs_list))

            return nt, copy_nt_def(nt, nt_def, [])

        self.nonterminals = {}
        for nt, nt_def in nonterminals.items():
            nt, nt_def = validate_nt(nt, nt_def)
            self.nonterminals[nt] = nt_def
        for nt, nt_def in synthetic_terminals.items():
            nt_def = NtDef((nt,), [Production([e], 0) for e in nt_def], None)
            nt, nt_def = validate_nt(nt, nt_def)
            self.nonterminals[nt] = nt_def
            # Remove synthetic terminals from the list of terminals.
            all_terminals -= OrderedSet([nt])

        self.terminals = OrderedFrozenSet(all_terminals)

        # Check types of reduce expressions and infer method types. But if the
        # caller passed in precalculated type info, skip it -- otherwise we
        # would redo type checking many times as we make minor changes to the
        # Grammar along the pipeline.
        if method_types is None:
            types.infer_types(self)
        else:
            for nt, nt_def in self.nonterminals.items():
                assert isinstance(nt_def.type, types.Type)
            self.methods = method_types

        # Synthesize "init" nonterminals.
        self.init_nts = []
        for goal in goal_nts:
            # Convert str goals to Nt objects and validate.
            if isinstance(goal, str):
                ok = goal in str_to_nt
                if ok:
                    goal = str_to_nt[goal]
            elif isinstance(goal, Nt):
                if keys_are_nt:
                    ok = goal in nonterminals
                else:
                    ok = goal.name in nonterminals
            if not ok:
                raise ValueError(
                    "goal nonterminal {!r} is undefined".format(goal))

            # Weird, but the key of an init nonterminal really is
            # `Nt(InitNt(Nt(goal_name, goal_args)), ())`. It takes no arguments,
            # but it refers to a goal that might take arguments.
            init_key = InitNt(goal)
            init_nt = Nt(init_key, ())
            if keys_are_nt:
                init_key = init_nt
            if init_key not in self.nonterminals:
                self.nonterminals[init_key] = NtDef(
                    (), [Production([goal], 0), Production([init_nt, End()], 'accept')], types.NoReturnType)
            self.init_nts.append(init_nt)

    def intern(self, obj):
        """Return a shared copy of the immutable object `obj`.

        This saves memory and consistent use allows code to use `is` for
        equality testing.
        """
        try:
            return self._cache[obj]
        except KeyError:
            self._cache[obj] = obj
            return obj

    # Terminals are tokens that must appear verbatim in the input wherever they
    # appear in the grammar, like the operators '+' '-' *' '/' and brackets '(' ')'
    # in the example grammar.
    def is_terminal(self, element):
        return type(element) is str

    def expand_set_of_terminals(self, terminals):
        """Copy a set of terminals, replacing any synthetic terminals with their representations.

        Returns a new OrderedSet.

        terminals - an iterable of terminals and/or other unique elements like
        None or ErrorToken.
        """
        result = OrderedSet()
        for t in terminals:
            if t in self.synthetic_terminals:
                result |= self.expand_set_of_terminals(self.synthetic_terminals[t])
            else:
                result.add(t)
        return result

    def goals(self):
        """Return a list of this grammar's goal nonterminals."""
        return [init_nt.name.goal for init_nt in self.init_nts]

    def with_nonterminals(self, nonterminals):
        """Return a copy of self with the same attributes except different nonterminals."""
        if self.methods is not None:
            for nt_def in nonterminals.values():
                assert nt_def.type is not None
        return Grammar(
            nonterminals,
            goal_nts=self.goals(),
            variable_terminals=self.variable_terminals,
            synthetic_terminals=self.synthetic_terminals,
            method_types=self.methods)

    # === A few methods for dumping pieces of grammar.

    def element_to_str(self, e):
        if isinstance(e, Nt):
            return e.pretty()
        elif self.is_terminal(e):
            if e in self.variable_terminals or e in self.synthetic_terminals:
                return e
            return '"' + repr(e)[1:-1] + '"'
        elif isinstance(e, Optional):
            return self.element_to_str(e.inner) + "?"
        elif isinstance(e, LookaheadRule):
            if len(e.set) == 1:
                op = "==" if e.positive else "!="
                s = repr(list(e.set)[0])
            else:
                op = "in" if e.positive else "not in"
                s = '{' + repr(list(e.set))[1:-1] + '}'
            return "[lookahead {} {}]".format(op, s)
        elif isinstance(e, End):
            return "<END>"
        elif e is NoLineTerminatorHere:
            return "[no LineTerminator here]"
        else:
            return str(e)

    def symbols_to_str(self, rhs):
        return " ".join(self.element_to_str(e) for e in rhs)

    def rhs_to_str(self, rhs):
        if isinstance(rhs, Production):
            if rhs.condition is None:
                prefix = ''
            else:
                param, value = rhs.condition
                if value is True:
                    condition = "+" + param
                elif value is False:
                    condition = "~" + param
                else:
                    condition = "{} == {!r}".format(param, value)
                prefix = "#[if {}] ".format(condition)
            return prefix + self.rhs_to_str(rhs.body)
        elif isinstance(rhs, Production):
            return self.rhs_to_str(rhs.body)
        elif len(rhs) == 0:
            return "[empty]"
        else:
            return self.symbols_to_str(rhs)

    def production_to_str(self, nt, rhs, reducer=()):
        # As we have two ways of representing productions at the moment, just
        # take multiple arguments :(
        return "{} ::= {}{}".format(
            self.element_to_str(nt),
            self.rhs_to_str(rhs),
            "" if reducer == () else " => " + expr_to_str(reducer))

    def lr_item_to_str(self, prods, item):
        prod = prods[item.prod_index]
        if item.lookahead is None:
            la = []
        else:
            la = [self.element_to_str(item.lookahead)]
        return "{} ::= {} >> {{{}}}".format(
            self.element_to_str(prod.nt),
            " ".join([self.element_to_str(e) for e in prod.rhs[:item.offset]]
                     + ["\N{MIDDLE DOT}"]
                     + la
                     + [self.element_to_str(e) for e in prod.rhs[item.offset:]]),
            ", ".join(
                "$" if t is None else self.element_to_str(t)
                for t in item.followed_by)
        )

    def item_set_to_str(self, prods, item_set):
        return "{{{}}}".format(
            ",  ".join(self.lr_item_to_str(prods, item) for item in item_set)
        )

    def expand_terminal(self, t):
        return self.synthetic_terminals.get(t) or OrderedFrozenSet([t])

    def compatible_elements(self, e1, e2):
        return (e1 == e2
                or (self.is_terminal(e1)
                    and self.is_terminal(e2)
                    and len(self.expand_terminal(e1) & self.expand_terminal(e2)) > 0))

    def compatible_sequences(self, seq1, seq2):
        """True if the two sequences could be the same terminals."""
        return (len(seq1) == len(seq1)
                and all(self.compatible_elements(e1, e2) for e1, e2 in zip(seq1, seq2)))

    def dump(self):
        for nt, nt_def in self.nonterminals.items():
            left_side = self.element_to_str(nt)
            if nt_def.params:
                left_side += "[" + ", ".join(nt_def.params) + "]"
            print(left_side + " ::=")
            for rhs in nt_def.rhs_list:
                print("   ", self.rhs_to_str(rhs))
            print()

    def dump_type_info(self):
        for nt, nt_def in self.nonterminals.items():
            print(nt, nt_def.type)
        for name, mty in self.methods.items():
            print("fn {}({}) -> {}"
                  .format(name,
                          ", ".join(types.type_to_str(ty) for ty in mty.argument_types),
                          types.type_to_str(mty.return_type)))


InitNt = collections.namedtuple("InitNt", "goal")
InitNt.__doc__ = """\
InitNt(goal) is the name of the init nonterminal for the given goal.

One init nonterminal is created internally for each goal symbol in the grammar.

The idea is to have a nonterminal that the user has no control over, that is
never used in any production, but *only* as an entry point for the grammar,
that always has a single production "init_nt ::= goal_nt". This predictable
structure makes it easier to get into and out of parsing at run time.

When an init nonterminal is matched, we take the "accept" action rather than
a "reduce" action.
"""


# *** Elements ****************************************************************
#
# Elements are the things that can appear in the .body list of a Production:
#
# *   Strings represent terminals (see `Grammar.is_terminal`)
#
# *   `Nt` objects refer to nonterminals.
#
# *   `Optional` objects represent optional elements.
#
# *   `LookaheadRule` objects are like lookahead assertions in regular
#     expressions.
#
# *   The `NoLineTerminatorHere` singleton object can appear between two other
#     symbols to rule out line breaks between them.
#
# *   `ErrorSymbol` objects never match anything produced by the lexer. Instead
#     they match an ErrorToken that's artificially injected into the token
#     stream at runtime, by the parser itself, just before a token that does
#     not match anything else.


def is_concrete_element(e):
    """True if parsing the element `e` produces a value.

    A production's concrete elements can be used in reduce expressions.
    """
    return not isinstance(e, (LookaheadRule, ErrorSymbol, NoLineTerminatorHereClass))


class Nt:
    """Nt(name, ((param0, arg0), ...)) - An invocation of a nonterminal.

    Nonterminals are like lambdas. Each nonterminal in a grammar is defined by an
    NtDef which has 0 or more parameters.

    Parameter names are strings. The arguments are typically booleans. They can be
    whatever you want, but each function nonterminal gets expanded into a set of
    productions, one for every different argument tuple that is ever passed to it.
    """

    __slots__ = ['name', 'args']

    def __init__(self, name, args=()):
        assert isinstance(name, (str, InitNt))
        self.name = name
        self.args = args

    def __hash__(self):
        return hash(('nt', self.name, self.args))

    def __eq__(self, other):
        return (isinstance(other, Nt)
                and (self.name, self.args) == (other.name, other.args))

    def __repr__(self):
        if self.args:
            return 'Nt({!r}, {!r})'.format(self.name, self.args)
        else:
            return 'Nt({!r})'.format(self.name)

    def pretty(self):
        """Unique version of this Nt to use in the Python runtime.

        Also used in debug/verbose output.
        """
        def arg_to_str(name, value):
            if value is True:
                return '+' + name
            elif value is False:
                return '~' + name
            elif isinstance(value, Var):
                if value.name == name:
                    return '?' + value.name
                return name + "=" + value.name
            else:
                return name + "=" + repr(value)

        if isinstance(self.name, InitNt):
            return "Start_" + self.name.goal.pretty()
        if len(self.args) == 0:
            return self.name
        return "{}[{}]".format(self.name,
                               ", ".join(arg_to_str(name, value)
                                         for name, value in self.args))


# Optional elements. These are expanded out before states are calculated,
# so the core of the algorithm never sees them.
Optional = collections.namedtuple("Optional", "inner")
Optional.__doc__ = """Optional(nt) matches either nothing or the given nt."""


# Literal elements. These are sequences of characters which are expected as
# input.
Literal = collections.namedtuple("Literal", "text")
Literal.__doc__ = """Literal(str) matches a sequence of characters."""


# UnicodeCategory elements. These are a set of literal elements which
# correspond to a given unicode cat_prefix.
UnicodeCategory = collections.namedtuple("UnicodeCategory", "cat_prefix")
UnicodeCategory.__doc__ = """UnicodeCategory(str) matches any character with
a category matching the cat_prefix."""


# Lookahead restrictions stay with us throughout the algorithm.
LookaheadRule = collections.namedtuple("LookaheadRule", "set positive")
LookaheadRule.__doc__ = """\
LookaheadRule(set, pos) imposes a lookahead restriction on whatever follows.

It never consumes any tokens itself. Instead, the right-hand side
[LookaheadRule(frozenset(['a', 'b']), False), 'Thing']
matches a Thing that does not start with the token `a` or `b`.
"""


# A lookahead restriction really just specifies a set of allowed terminals.
#
# -   No lookahead restriction at all is equivalent to a rule specifying all terminals.
#
# -   A positive lookahead restriction explicitly lists all allowed tokens.
#
# -   A negative lookahead restriction instead specifies the set of all tokens
#     except a few.
#
def lookahead_contains(rule, t):
    """True if the given lookahead restriction `rule` allows the terminal `t`."""
    return (rule is None
            or (t in rule.set if rule.positive
                else t not in rule.set))


def lookahead_intersect(a, b):
    """Returns a single rule enforcing both `a` and `b`, allowing only terminals that pass both."""
    if a is None:
        return b
    elif b is None:
        return a
    elif a.positive:
        if b.positive:
            return LookaheadRule(a.set & b.set, True)
        else:
            return LookaheadRule(a.set - b.set, True)
    else:
        if b.positive:
            return LookaheadRule(b.set - a.set, True)
        else:
            return LookaheadRule(a.set | b.set, False)


class NoLineTerminatorHereClass:
    def __str__(self):
        return 'NoLineTerminatorHere'


NoLineTerminatorHere = NoLineTerminatorHereClass()

# Optional elements. These are expanded out before states are calculated,
# so the core of the algorithm never sees them.
Exclude = collections.namedtuple("Exclude", "inner exclusion_list")
Exclude.__doc__ = """Exclude(nt1, nt2) matches if nt1 matches and nt2 does not."""

# End. This is used to represent the terminal which is infinitely produced by
# the lexer when input end is reached.
End = collections.namedtuple("End", "")
End.__doc__ = """End() represent the end of the input content."""
End_default_eq = End.__eq__
End.__eq__ = lambda x, y: x.__class__ == y.__class__ and End_default_eq(x, y)

# Special grammar symbol that can be consumed to handle a syntax error.
#
# The error code is passed to an error-handling routine at run time which
# decides if the error is recoverable or not.
ErrorSymbol = collections.namedtuple("ErrorSymbol", "error_code")
ErrorSymbol.__doc__ = """Special grammar symbol that can be consumed to handle a syntax error."""
ErrorSymbol_default_eq = ErrorSymbol.__eq__
ErrorSymbol.__eq__ = lambda x, y: x.__class__ == y.__class__ and ErrorSymbol_default_eq(x, y)


class NtDef:
    """Definition of a nonterminal.

    Instances have three attributes:

    .params - Tuple of strings, the names of the parameters.

    .rhs_list - List of Production objects. Arguments to Nt elements in the
    productions can be Var(s) where `s in params`, indicating that parameter
    should be passed through unchanged.

    .type - The type of runtime value produced by parsing an instance of this
    nonterminal, or None.

    An NtDef is a sort of lambda.

    Some langauges have constructs that are allowed or disallowed in particular
    situations. For example, in many languages `return` statements are allowed
    only inside functions or methods. The ECMAScript standard (5.1.5 "Grammar
    Notation") offers this example of the notation it uses to specify this sort
    of thing:

        StatementList [Return] :
            [+Return] ReturnStatement
            ExpressionStatement

    This is an abbreviation for:

        StatementList :
            ExpressionStatement

        StatementList_Return :
            ReturnStatement
            ExpressionStatement

    We offer NtDef.params as a way of representing this in our system.

        "StatementList": NtDef(("Return",), [
            Production(["ReturnStatement"], condition=("Return", True)),
            ["ExpressionStatement"],
        ], None),

    This is an abbreviation for:

        "StatementList_0": [
            ["ExpressionStatement"],
        ],
        "StatementList_1": [
            ["ReturnStatement"],
            ["ExpressionStatement"],
        ],

    """

    __slots__ = ['params', 'rhs_list', 'type']

    def __init__(self, params, rhs_list, type):
        assert isinstance(params, tuple)
        self.params = params
        self.rhs_list = rhs_list
        self.type = type

    def __eq__(self, other):
        return (isinstance(other, NtDef)
                and ((self.params, self.rhs_list, self.type)
                     == (other.params, other.rhs_list, other.type)))

    __hash__ = None

    def __repr__(self):
        return "NtDef({!r}, {!r}, {!r})".format(self.params, self.rhs_list, self.type)

    def with_rhs_list(self, new_rhs_list):
        return NtDef(self.params, new_rhs_list, self.type)


Var = collections.namedtuple("Var", "name")
Var.__doc__ = """\
Var(name) represents the run-time value of the parameter with the given name.
"""

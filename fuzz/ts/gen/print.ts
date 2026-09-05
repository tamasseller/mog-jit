// fuzz — AST → DSL source.
//
// The bridge back into the toolchain: `ir` consumes source text, so a
// mutated tree only reaches `lowerProgram` by being printed and reparsed.
// That round trip is also the point — every mutant goes through the real
// parser, and `parse(print(ast))` must equal `ast` structurally, which is
// checked on every candidate (round_trip.ts).
//
// Precedence follows grammer.pegjs's own chain rather than full
// parenthesisation, because a finding is read by a person.

import type {
    ControlBody, Expression, Statement, SwitchCase, VariableDeclarator,
} from "mog-core"

// ── precedence ──────────────────────────────────────────────────────────
//
// Higher binds tighter. One level per grammer.pegjs production, so a child
// needs parentheses exactly when its level falls below what its position
// accepts.

const ASSIGN = 1, TERNARY = 2, PRIMARY = 15

const BINARY_LEVEL: Readonly<Record<string, number>> = {
    "||": 3, "&&": 4, "|": 5, "^": 6, "&": 7,
    "==": 8, "!=": 8,
    "<": 9, "<=": 9, ">": 9, ">=": 9,
    "<<": 10, ">>": 10,
    "+": 11, "-": 11,
    "*": 12, "/": 12, "%": 12,
}

const UNARY = 13, POSTFIX = 14

function levelOf(e: Expression): number
{
    switch(e.type)
    {
        case "AssignmentExpression": return ASSIGN
        case "ConditionalExpression": return TERNARY
        case "LogicalExpression": case "BinaryExpression": return BINARY_LEVEL[e.operator] ?? PRIMARY
        case "UnaryExpression": return UNARY
        case "UpdateExpression": return e.prefix ? UNARY : POSTFIX
        default: return PRIMARY
    }
}

/** `e` rendered so it parses back at a position accepting `need` or tighter. */
function sub(e: Expression, need: number): string
{
    const text = expr(e)
    return levelOf(e) < need ? `(${text})` : text
}

function literal(value: number): string
{
    const v = value >>> 0
    if(value < 0) throw new Error(`print: negative literal ${value} — the grammar has no such token, a minus is a UnaryExpression`)
    return v < 0x10000 ? String(v) : `0x${v.toString(16)}`
}

export function expr(e: Expression): string
{
    switch(e.type)
    {
        case "Literal": return literal(e.value)
        case "Identifier": return e.name
        case "CastExpression": return `${e.varType}(${expr(e.argument)})`
        case "CallExpression": return `${e.callee.name}(${e.arguments.map(expr).join(", ")})`

        case "AssignmentExpression":
            // Right-associative, and `left` is an Identifier by grammar.
            return `${e.left.name} ${e.operator} ${sub(e.right, ASSIGN)}`

        case "ConditionalExpression":
            // test is a LogicalOR position, alternate an Assignment one;
            // consequent sits between `?` and `:` and accepts anything.
            return `${sub(e.test, BINARY_LEVEL["||"]!)} ? ${expr(e.consequent)} : ${sub(e.alternate, ASSIGN)}`

        case "LogicalExpression":
        case "BinaryExpression":
        {
            const lvl = BINARY_LEVEL[e.operator]
            if(lvl === undefined) throw new Error(`print: no precedence for operator ${e.operator}`)
            // Left-associative throughout: the right operand binds one tighter.
            return `${sub(e.left, lvl)} ${e.operator} ${sub(e.right, lvl + 1)}`
        }

        case "UnaryExpression":
        {
            // A nested prefix is always parenthesised: `- -x` is two tokens
            // only by the space, and `--x` lexes as one update operator.
            const inner = e.argument
            const needsParens = inner.type === "UnaryExpression" || (inner.type === "UpdateExpression" && inner.prefix)
            return `${e.operator}${needsParens ? `(${expr(inner)})` : sub(inner, UNARY)}`
        }

        case "UpdateExpression":
            // Postfix takes a PrimaryExpression only (grammer.pegjs).
            return e.prefix
                ? `${e.operator}${sub(e.argument, PRIMARY)}`
                : `${sub(e.argument, PRIMARY)}${e.operator}`
    }
}

// ── statements ──────────────────────────────────────────────────────────

const pad = (depth: number): string => "    ".repeat(depth)

function declaration(d: VariableDeclarator): string
{
    return d.init === null ? d.id.name : `${d.id.name} = ${expr(d.init)}`
}

/** A control body on the same line as its header: a `Block` keeps its
 *  braces, because in this position they are the branch's own RTL block and
 *  dropping them changes what the lowering emits (isa-core.md §4.4). */
function body(b: ControlBody, depth: number): string
{
    return b.type === "BlockStatement"
        ? `{\n${statements(b.body, depth + 1)}${pad(depth)}}`
        : `\n${stmt(b, depth + 1)}`
}

function switchCase(c: SwitchCase, depth: number): string
{
    const head = c.test === null ? `${pad(depth)}default:` : `${pad(depth)}case ${expr(c.test)}:`
    return `${head}\n${statements(c.consequent, depth + 1)}`
}

export function stmt(s: Statement, depth: number): string
{
    const i = pad(depth)
    switch(s.type)
    {
        case "BlockStatement":
            return `${i}{\n${statements(s.body, depth + 1)}${i}}\n`

        case "IfStatement":
        {
            const head = `${i}if (${expr(s.test)}) ${body(s.consequent, depth)}`
            if(s.alternate === null) return `${head}\n`
            // `else` only reads as attached to the `if` when the consequent
            // is a block; a bare statement already ended with its newline.
            const joiner = s.consequent.type === "BlockStatement" ? " else " : `\n${i}else `
            return `${head}${joiner}${body(s.alternate, depth)}\n`
        }

        case "WhileStatement":
            return `${i}while (${expr(s.test)}) ${body(s.body, depth)}\n`

        case "DoWhileStatement":
            return `${i}do ${body(s.body, depth)} while (${expr(s.test)});\n`

        case "ForStatement":
        {
            // Every one of the three slots may be empty, and `init` carries
            // its own `;` because the grammar reads a whole statement there.
            const init = s.init === null ? ";"
                : s.init.type === "VariableDeclaration" ? declarationStatement(s.init)
                : `${expr(s.init)};`
            const test = s.test === null ? "" : expr(s.test)
            const update = s.update === null ? "" : expr(s.update)
            return `${i}for (${init} ${test}; ${update}) ${body(s.body, depth)}\n`
        }

        case "SwitchStatement":
            return `${i}switch (${expr(s.discriminant)}) {\n`
                + s.cases.map(c => switchCase(c, depth + 1)).join("")
                + `${i}}\n`

        case "VariableDeclaration":
            return `${i}${declarationStatement(s)}\n`

        case "BreakStatement": return `${i}break;\n`
        case "ReturnStatement": return s.argument === null ? `${i}return;\n` : `${i}return ${expr(s.argument)};\n`
        case "ExpressionStatement": return `${i}${expr(s.expression)};\n`
    }
}

/** One `TypeName Declarator, Declarator, ...;` — every declarator in a
 *  declaration shares the one type name the grammar reads, so a tree whose
 *  declarators disagree has no surface form and is rejected here rather
 *  than printed into something that reparses differently. */
function declarationStatement(d: {declarations: readonly VariableDeclarator[]}): string
{
    const [first, ...rest] = d.declarations
    if(first === undefined) throw new Error("print: declaration with no declarators")
    for(const other of rest)
    {
        if(other.varType !== first.varType)
            throw new Error(`print: declaration mixes ${first.varType} and ${other.varType} — one declaration carries one type name`)
    }
    return `${first.varType} ${d.declarations.map(declaration).join(", ")};`
}

export function statements(list: readonly Statement[], depth: number): string
{
    return list.map(s => stmt(s, depth)).join("")
}

/** A whole procedure body as DSL source. */
export function print(list: readonly Statement[]): string
{
    return statements(list, 0)
}

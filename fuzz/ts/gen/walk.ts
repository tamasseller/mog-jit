// fuzz — statement traversal.
//
// ast.ts exports `recurseOver`/`mapOver` for expressions; a `Statement` has
// no equivalent, and the generator, the UB analysis and the mutator all
// need one.

import { recurseOver } from "mog-core"
import type { ControlBody, Expression, Statement } from "mog-core"

const asList = (b: ControlBody): Statement[] => b.type === "BlockStatement" ? b.body : [b]

/** Every nested statement list `s` directly governs. */
export function childBodies(s: Statement): Statement[][]
{
    switch(s.type)
    {
        case "BlockStatement": return [s.body]
        case "IfStatement": return s.alternate === null ? [asList(s.consequent)] : [asList(s.consequent), asList(s.alternate)]
        case "WhileStatement": case "DoWhileStatement": case "ForStatement": return [asList(s.body)]
        case "SwitchStatement": return s.cases.map(c => c.consequent)
        default: return []
    }
}

/** Every expression `s` holds directly — not those nested inside them. */
export function childExpressions(s: Statement): Expression[]
{
    switch(s.type)
    {
        case "IfStatement": case "WhileStatement": case "DoWhileStatement": return [s.test]
        case "ForStatement":
        {
            const out: Expression[] = []
            if(s.init !== null && s.init.type !== "VariableDeclaration") out.push(s.init)
            if(s.init !== null && s.init.type === "VariableDeclaration")
                for(const d of s.init.declarations) if(d.init !== null) out.push(d.init)
            if(s.test !== null) out.push(s.test)
            if(s.update !== null) out.push(s.update)
            return out
        }
        case "SwitchStatement": return [s.discriminant, ...s.cases.flatMap(c => c.test === null ? [] : [c.test])]
        case "VariableDeclaration": return s.declarations.flatMap(d => d.init === null ? [] : [d.init])
        case "ReturnStatement": return s.argument === null ? [] : [s.argument]
        case "ExpressionStatement": return [s.expression]
        default: return []
    }
}

/** Pre-order over `list` and everything nested inside it. */
export function eachStatement(list: readonly Statement[], visit: (s: Statement) => void): void
{
    for(const s of list)
    {
        visit(s)
        for(const nested of childBodies(s)) eachStatement(nested, visit)
    }
}

/** Every sub-expression of `e`, `e` itself included, pre-order. */
export function eachSubExpression(e: Expression, visit: (e: Expression) => void): void
{
    visit(e)
    recurseOver<void, void>(e, child => eachSubExpression(child, visit), () => undefined, undefined)
}

/** Every name a declaration introduces, including a `for` init's own. A
 *  declared name that nothing ever reads appears in no expression, so
 *  collecting identifiers alone misses it — and handing it out again as a
 *  fresh name is a redeclaration. */
export function declaredNames(list: readonly Statement[]): Set<string>
{
    const names = new Set<string>()
    eachStatement(list, s =>
    {
        if(s.type === "VariableDeclaration") for(const d of s.declarations) names.add(d.id.name)
        if(s.type === "ForStatement" && s.init !== null && s.init.type === "VariableDeclaration")
            for(const d of s.init.declarations) names.add(d.id.name)
    })
    return names
}

/** How deeply `e` nests. The tiler's Pareto search prices an expression by
 *  its shape, so this is the axis a generator has to bound: left alone,
 *  each generation wraps what the last one built and lowering slows down
 *  faster than the tree grows. */
export function heightOf(e: Expression): number
{
    return 1 + recurseOver<number, number>(e, heightOf, (...v) => Math.max(0, ...v), 0)
}

/** Every expression anywhere in `list`. */
export function eachExpression(list: readonly Statement[], visit: (e: Expression) => void): void
{
    eachStatement(list, s => { for(const e of childExpressions(s)) eachSubExpression(e, visit) })
}

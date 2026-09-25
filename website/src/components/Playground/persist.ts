import {Parser, type Node} from 'acorn';
import jsx from 'acorn-jsx';

const JsxParser = Parser.extend(jsx());

type AnyNode = Node & Record<string, unknown>;

/** Best-effort name of the function enclosing a call, for a stable, readable key. */
function enclosingName(ancestors: AnyNode[]): string {
  for (let i = ancestors.length - 1; i >= 0; i--) {
    const n = ancestors[i];
    if (
      n.type !== 'FunctionDeclaration' &&
      n.type !== 'FunctionExpression' &&
      n.type !== 'ArrowFunctionExpression'
    ) {
      continue;
    }
    const id = n.id as AnyNode | null | undefined;
    if (id?.name) return id.name as string;
    const parent = ancestors[i - 1];
    if (parent?.type === 'VariableDeclarator')
      return ((parent.id as AnyNode).name as string) ?? '_';
    if (parent?.type === 'Property' || parent?.type === 'MethodDefinition') {
      return ((parent.key as AnyNode).name as string) ?? '_';
    }
    return '_';
  }
  return '_';
}

/**
 * The simulator's persist transform, for the browser: rewrites each `useState(init)` in an app
 * file to `usePersistentState("file::Component#n", init)`, so state survives the reload that
 * every edit triggers, the way it does under the dev server. Only a `useState` imported from
 * `react` or `embedded-react` is rewritten. Keys are the component name plus the hook's order in
 * it, so ordinary edits keep state and adding or reordering hooks resets that component.
 *
 * Text is spliced rather than regenerated, so everything else in the file is untouched. Returns
 * the source unchanged when there is nothing to rewrite (or it does not parse: the compiler will
 * report the error).
 */
export function persistUseState(source: string, moduleId: string): string {
  let ast: AnyNode;
  try {
    ast = JsxParser.parse(source, {
      ecmaVersion: 'latest',
      sourceType: 'module',
    }) as unknown as AnyNode;
  } catch {
    return source;
  }

  const importsUseState = (ast.body as AnyNode[]).some(
    n =>
      n.type === 'ImportDeclaration' &&
      ['react', 'embedded-react'].includes(
        (n.source as AnyNode).value as string,
      ) &&
      (n.specifiers as AnyNode[]).some(
        s =>
          s.type === 'ImportSpecifier' &&
          ((s.imported as AnyNode).name as string) === 'useState',
      ),
  );
  if (!importsUseState) return source;

  // A function that declares its own `useState` (a parameter or a local) shadows the import inside it.
  const declaresUseState = (fn: AnyNode): boolean => {
    const named = (n: unknown) =>
      (n as AnyNode | null)?.type === 'Identifier' &&
      (n as AnyNode).name === 'useState';
    if ((fn.params as AnyNode[]).some(named)) return true;
    const body = fn.body as AnyNode;
    if (body.type !== 'BlockStatement') return false;
    return (body.body as AnyNode[]).some(
      st =>
        (st.type === 'VariableDeclaration' &&
          (st.declarations as AnyNode[]).some(d => named(d.id))) ||
        (st.type === 'FunctionDeclaration' && named(st.id)),
    );
  };
  const isFunction = (n: AnyNode) =>
    n.type === 'FunctionDeclaration' ||
    n.type === 'FunctionExpression' ||
    n.type === 'ArrowFunctionExpression';

  const edits: {start: number; end: number; text: string}[] = [];
  const counters = new Map<string, number>();
  const visit = (node: AnyNode, ancestors: AnyNode[]) => {
    if (node.type === 'CallExpression') {
      const callee = node.callee as AnyNode;
      if (
        callee.type === 'Identifier' &&
        callee.name === 'useState' &&
        !ancestors.some(a => isFunction(a) && declaresUseState(a))
      ) {
        const fn = enclosingName(ancestors);
        const idx = counters.get(fn) ?? 0;
        counters.set(fn, idx + 1);
        const key = `${moduleId}::${fn}#${idx}`;
        const args = node.arguments as AnyNode[];
        edits.push({
          start: callee.start,
          end: args.length ? args[0].start : node.end - 1,
          text: `__erPersistState(${JSON.stringify(key)}${args.length ? ', ' : ''}`,
        });
      }
    }
    ancestors.push(node);
    for (const key of Object.keys(node)) {
      if (key === 'type' || key === 'start' || key === 'end') continue;
      const v = node[key];
      if (Array.isArray(v)) {
        for (const c of v)
          if (c && typeof (c as AnyNode).type === 'string')
            visit(c as AnyNode, ancestors);
      } else if (v && typeof (v as AnyNode).type === 'string') {
        visit(v as AnyNode, ancestors);
      }
    }
    ancestors.pop();
  };
  visit(ast, []);
  if (!edits.length) return source;

  let out = source;
  for (const e of edits.sort((a, b) => b.start - a.start)) {
    out = out.slice(0, e.start) + e.text + out.slice(e.end);
  }
  return `import {usePersistentState as __erPersistState} from 'embedded-react';\n${out}`;
}

import {Parser, type Node} from 'acorn';
import jsx from 'acorn-jsx';

const JsxParser = Parser.extend(jsx());

type AnyNode = Node & Record<string, unknown>;

/** Best-effort name of the function enclosing a call, for a stable, readable key. */
function enclosingName(ancestors: AnyNode[]): string {
  for (let index = ancestors.length - 1; index >= 0; index--) {
    const node = ancestors[index];
    if (
      node.type !== 'FunctionDeclaration' &&
      node.type !== 'FunctionExpression' &&
      node.type !== 'ArrowFunctionExpression'
    ) {
      continue;
    }
    const id = node.id as AnyNode | null | undefined;
    if (id?.name) return id.name as string;
    const parent = ancestors[index - 1];
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
    node =>
      node.type === 'ImportDeclaration' &&
      ['react', 'embedded-react'].includes(
        (node.source as AnyNode).value as string,
      ) &&
      (node.specifiers as AnyNode[]).some(
        specifier =>
          specifier.type === 'ImportSpecifier' &&
          ((specifier.imported as AnyNode).name as string) === 'useState',
      ),
  );
  if (!importsUseState) return source;

  // A function that declares its own `useState` (a parameter or a local) shadows the import inside it.
  const declaresUseState = (functionNode: AnyNode): boolean => {
    const named = (candidate: unknown) =>
      (candidate as AnyNode | null)?.type === 'Identifier' &&
      (candidate as AnyNode).name === 'useState';
    if ((functionNode.params as AnyNode[]).some(named)) return true;
    const body = functionNode.body as AnyNode;
    if (body.type !== 'BlockStatement') return false;
    return (body.body as AnyNode[]).some(
      statement =>
        (statement.type === 'VariableDeclaration' &&
          (statement.declarations as AnyNode[]).some(declarator =>
            named(declarator.id),
          )) ||
        (statement.type === 'FunctionDeclaration' && named(statement.id)),
    );
  };
  const isFunction = (node: AnyNode) =>
    node.type === 'FunctionDeclaration' ||
    node.type === 'FunctionExpression' ||
    node.type === 'ArrowFunctionExpression';

  const edits: {start: number; end: number; text: string}[] = [];
  const counters = new Map<string, number>();
  const visit = (node: AnyNode, ancestors: AnyNode[]) => {
    if (node.type === 'CallExpression') {
      const callee = node.callee as AnyNode;
      if (
        callee.type === 'Identifier' &&
        callee.name === 'useState' &&
        !ancestors.some(
          ancestor => isFunction(ancestor) && declaresUseState(ancestor),
        )
      ) {
        const enclosing = enclosingName(ancestors);
        const index = counters.get(enclosing) ?? 0;
        counters.set(enclosing, index + 1);
        const key = `${moduleId}::${enclosing}#${index}`;
        const callArguments = node.arguments as AnyNode[];
        edits.push({
          start: callee.start,
          end: callArguments.length ? callArguments[0].start : node.end - 1,
          text: `__erPersistState(${JSON.stringify(key)}${callArguments.length ? ', ' : ''}`,
        });
      }
    }
    ancestors.push(node);
    for (const key of Object.keys(node)) {
      if (key === 'type' || key === 'start' || key === 'end') continue;
      const value = node[key];
      if (Array.isArray(value)) {
        for (const child of value)
          if (child && typeof (child as AnyNode).type === 'string')
            visit(child as AnyNode, ancestors);
      } else if (value && typeof (value as AnyNode).type === 'string') {
        visit(value as AnyNode, ancestors);
      }
    }
    ancestors.pop();
  };
  visit(ast, []);
  if (!edits.length) return source;

  let output = source;
  for (const edit of edits.sort((left, right) => right.start - left.start)) {
    output = output.slice(0, edit.start) + edit.text + output.slice(edit.end);
  }
  return `import {usePersistentState as __erPersistState} from 'embedded-react';\n${output}`;
}

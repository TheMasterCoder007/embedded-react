/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {cScalarType, cstr} from '../c-syntax.mts';
import {printAnimationStart} from './animations.mts';
import {printUpdateVector} from './vectors.mts';
import {printCondition, printExpr} from './expressions.mts';
import {formatArgs, printText} from './text.mts';
import {slotOf} from '../ir/types.mts';
import type {IrExpr} from '../ir/expressions.mts';
import type {StateRecord} from '../types.mts';
import type {
  IrAppendItem,
  IrCallback,
  IrSetStateText,
  IrStmt,
  IrTimer,
} from '../ir/statements.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A generated C function: `static void <name>(…)` with `body` as its lines. */
export interface CFunction {
  name: string;
  body: string[];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** One level of indentation. */
const INDENT = '    ';

/** The line that re-applies state-dependent props after a body set state. */
export const APP_UPDATE_CALL = '    app_update();';

/** The indent list operations print at, whatever block they sit in. */
const LIST_INDENT = INDENT;

/** The file-scope flags that record which state was written since app_update() last ran. */
export const CHANGED_FLAGS = 's_changed';

/** Reports a fixed limit the app ran past to stderr, once per place it can happen. */
export const APP_LIMIT_DEFINITION = `/* Reports a fixed limit the app ran past, once per place it can happen, so a timer appending to a full list
   cannot flood the log. */
#define APP_LIMIT(msg)                         \\
    do                                         \\
    {                                          \\
        static bool reported_;                 \\
        if (!reported_)                        \\
        {                                      \\
            reported_ = true;                  \\
            fprintf(stderr, "AOT: %s\\n", msg); \\
        }                                      \\
    } while (0)
`;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Matches `<member>` as a whole C lvalue, so `s_state.label` does not also match `s_state.label2`.
 *
 * @param member  A C lvalue.
 *
 * @returns A pattern that finds it in the C source.
 */
const readsMember = (member: string): RegExp =>
  new RegExp(`(^|[^\\w.])${member.replace(/\./g, '\\.')}(?![\\w])`);

/**
 * The line that flags a state as written, so app_update() sets the nodes that read it again; none when no node
 * reads it.
 *
 * @param state  The state.
 * @param indent  The line's indentation.
 *
 * @returns The C lines.
 */
export const printChangedFlag = (
  state: StateRecord,
  indent: string,
): string[] =>
  state.isWatched ? [`${indent}${CHANGED_FLAGS}.${state.cField} = 1;`] : [];

/**
 * A timer as the C call that starts it and evaluates to its id. A timestamp or float delay goes through
 * ToInt32 and a floor at 0, as Flow A's setTimeout does.
 *
 * @param timer  The timer.
 *
 * @returns The C call.
 */
function printTimerAdd(timer: IrTimer): string {
  const delay = timer.delay;
  const delayMs = !delay
    ? '0'
    : delay.type.kind === 'int64'
      ? `app_delay_ms64(${printExpr(delay)})`
      : delay.type.kind === 'float'
        ? `app_delay_msf(${printExpr(delay)})`
        : printExpr(delay);
  return `er_timer_add((int)(${delayMs}), ${timer.isRepeating ? 'true' : 'false'}, ${timer.fn})`;
}

/**
 * A write into a fixed string buffer. One that can come out too long reports, once, when snprintf cuts it
 * short; a constant was checked to fit at compile time.
 *
 * @param buffer  The buffer, as a C lvalue.
 * @param args  snprintf's arguments after the buffer and its size.
 * @param isConstant  The value is a constant.
 * @param report  What APP_LIMIT reports when the value is cut short.
 * @param indent  The statement's indentation.
 *
 * @returns The C lines.
 */
export function printStringWrite(
  buffer: string,
  args: string,
  isConstant: boolean,
  report: string,
  indent: string,
): string[] {
  const write = `snprintf(${buffer}, sizeof(${buffer}), ${args})`;
  if (isConstant) {
    return [`${indent}${write};`];
  }

  return [
    `${indent}if (${write} >= (int)sizeof(${buffer}))`,
    `${indent}{`,
    `${indent}${INDENT}APP_LIMIT(${cstr(report)});`,
    `${indent}}`,
  ];
}

/**
 * The report for an app string buffer cut short.
 *
 * @param what  What the buffer holds.
 *
 * @returns The report.
 */
const cutShort = (what: string): string =>
  `${what} cut short, raise ER_AOT_LIST_STR_CAP`;

/**
 * A string state write. snprintf's source and destination may not overlap, so a value that reads the slot
 * itself (`setLabel(label + '!')`) is built in a temporary first.
 *
 * @param stmt  The write.
 * @param indent  The statement's indentation.
 *
 * @returns The C lines.
 */
function printSetStateText(stmt: IrSetStateText, indent: string): string[] {
  const member = stmt.state.cMember;
  const format = printText(stmt.text);
  const report = cutShort(`state '${stmt.state.name}'`);
  if (format.args.some(formatArg => readsMember(member).test(formatArg))) {
    return [
      `${indent}{`,
      `${indent}    char next[sizeof(${member})];`,
      ...printStringWrite(
        'next',
        formatArgs(format),
        false,
        report,
        indent + INDENT,
      ),
      `${indent}    memcpy(${member}, next, strlen(next) + 1);`,
      `${indent}}`,
    ];
  }

  return printStringWrite(
    member,
    formatArgs(format),
    !format.args.length,
    report,
    indent,
  );
}

/**
 * An appended list item: fill the next free slot's fields and count it, or report a full list.
 *
 * @param stmt  The append.
 *
 * @returns The C lines.
 */
function printAppendItem(stmt: IrAppendItem): string[] {
  const {name, arrayName, countMember, cap} = stmt.list;
  const lines = [
    `${LIST_INDENT}if (${countMember} < ${cap})`,
    `${LIST_INDENT}{`,
  ];
  for (const itemValue of stmt.values) {
    const slot = `${arrayName}[${countMember}].${itemValue.field.key}`;
    if ('text' in itemValue) {
      const format = printText(itemValue.text);
      lines.push(
        ...printStringWrite(
          slot,
          formatArgs(format),
          !format.args.length,
          cutShort(`field '${itemValue.field.key}' of list '${name}'`),
          LIST_INDENT + INDENT,
        ),
      );
    } else {
      lines.push(
        `${LIST_INDENT}${INDENT}${slot} = ${printExpr(itemValue.value)};`,
      );
    }
  }

  lines.push(
    `${LIST_INDENT}${INDENT}${countMember}++;`,
    `${LIST_INDENT}}`,
    `${LIST_INDENT}else`,
    `${LIST_INDENT}{`,
    `${LIST_INDENT}${INDENT}APP_LIMIT(${cstr(`list '${name}' full, raise ER_AOT_LIST_CAP`)});`,
    `${LIST_INDENT}}`,
  );
  return lines;
}

/**
 * Prints a statement as C lines.
 *
 * @param stmt  The statement.
 * @param indent  Its indentation.
 *
 * @returns Its C lines.
 */
function printStmt(stmt: IrStmt, indent: string): string[] {
  switch (stmt.kind) {
    case 'declareLocal': {
      // A string local is a buffer of its own, so a later write to the state it copied cannot change it.
      const init: IrExpr = stmt.init;
      if (init.type.kind === 'string') {
        const declaration = stmt.isHoisted
          ? []
          : [`${indent}char ${stmt.name}[${stmt.stringCap}];`];
        return [
          ...declaration,
          ...printStringWrite(
            stmt.name,
            `"%s", ${printExpr(init)}`,
            init.kind === 'string',
            cutShort(`local '${stmt.name}'`),
            indent,
          ),
        ];
      }

      const declaredType = stmt.isHoisted
        ? ''
        : cScalarType(slotOf(init.type)!) + ' ';
      return [`${indent}${declaredType}${stmt.name} = ${printExpr(init)};`];
    }
    case 'startTimer': {
      const timerAdd = printTimerAdd(stmt.timer);
      if (!stmt.idLocal) {
        return [`${indent}${timerAdd};`];
      }

      // The id is only read by a later clear*(); `(void)` keeps the compiler quiet when nothing does.
      const {name, isHoisted} = stmt.idLocal;
      return [
        `${indent}${isHoisted ? '' : 'int '}${name} = ${timerAdd};`,
        `${indent}(void)${name};`,
      ];
    }
    case 'clearTimer':
      return [`${indent}er_timer_clear(${printExpr(stmt.id)});`];
    case 'setState':
      return [
        `${indent}${stmt.state.cMember} = ${printExpr(stmt.value)};`,
        ...printChangedFlag(stmt.state, indent),
      ];
    case 'setStateText':
      return [
        ...printSetStateText(stmt, indent),
        ...printChangedFlag(stmt.state, indent),
      ];
    case 'clearList':
      return [
        `${LIST_INDENT}${stmt.list.countMember} = 0;`,
        ...printChangedFlag(stmt.list, LIST_INDENT),
      ];
    case 'appendItem':
      return [
        ...printAppendItem(stmt),
        ...printChangedFlag(stmt.list, LIST_INDENT),
      ];
    case 'dropLastItem': {
      const count = stmt.list.countMember;
      return [
        `${LIST_INDENT}if (${count} > 0) ${count}--;`,
        ...printChangedFlag(stmt.list, LIST_INDENT),
      ];
    }
    case 'keepFirstItems': {
      const count = stmt.list.countMember;
      return [
        `${LIST_INDENT}${count} = (${count} < ${stmt.count}) ? ${count} : ${stmt.count};`,
        ...printChangedFlag(stmt.list, LIST_INDENT),
      ];
    }
    case 'sliceItems': {
      const count = stmt.list.countMember;
      return [
        `${LIST_INDENT}${count} = app_slice_len(${count}, ${printExpr(stmt.end)});`,
        ...printChangedFlag(stmt.list, LIST_INDENT),
      ];
    }
    case 'writeRef':
      return [`${indent}${stmt.ref.cVar} ${stmt.op} ${printExpr(stmt.value)};`];
    case 'stepRef':
      return [`${indent}${stmt.ref.cVar}${stmt.op};`];
    case 'stopAnimations': {
      const lines = stmt.handles.map(
        handle =>
          `${indent}er_anim_value_set(${handle}, er_anim_value_get(${handle}));`,
      );
      if (stmt.waitOwner === null) {
        return lines;
      }

      // Waits are detached before a frozen value's completion can start a new run, and reported after it.
      return [
        `${indent}app_wait_detach(${stmt.waitOwner});`,
        ...lines,
        `${indent}app_wait_report(${stmt.waitOwner});`,
      ];
    }
    case 'if': {
      // Each branch is a nested C block.
      const lines = [
        `${indent}if (${printCondition(stmt.test)})`,
        `${indent}{`,
        ...printStmts(stmt.consequent, indent + INDENT),
        `${indent}}`,
      ];
      if (stmt.alternate) {
        lines.push(
          `${indent}else`,
          `${indent}{`,
          ...printStmts(stmt.alternate, indent + INDENT),
          `${indent}}`,
        );
      }

      return lines;
    }
    case 'return':
      return [`${indent}return;`];
    case 'armCleanup':
      return [`${indent}${stmt.flag} = 1;`];
    case 'startAnimations':
    case 'startChain':
    case 'completeAfter':
      return printAnimationStart(stmt);
    case 'updateVector':
      return printUpdateVector(stmt).map(line => indent + line);
  }
}

/**
 * Prints statements as C lines.
 *
 * @param stmts  The statements.
 * @param indent  Their indentation.
 *
 * @returns Their C lines.
 */
export function printStmts(stmts: IrStmt[], indent: string): string[] {
  return stmts.flatMap(stmt => printStmt(stmt, indent));
}

/**
 * A callback's function body: its statements, then app_update() when it set state.
 *
 * @param callback  The callback.
 *
 * @returns The C lines.
 */
export function printCallback(callback: IrCallback): string[] {
  const lines = printStmts(callback.body, INDENT);
  if (callback.updatesState) {
    lines.push(APP_UPDATE_CALL);
  }

  return lines;
}

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

// gdb-check — checks the GDB a debug session is about to run, before it runs.
//
// The debug adapter needs GDB 14 or later, built with Python. A toolchain's GDB is often older, has no Python, or
// is linked against a Python that isn't installed. Each of those gets its own message, with what to do about it.
//
// Espressif's GDB is a launcher beside one build per Python version. It asks the first Python on PATH for its
// version and runs the build for it, or the build with no Python when it has none. For that GDB the check finds a
// Python it has a build for, and returns an environment with it first on PATH.

import {execFile} from 'node:child_process';
import {
  mkdtempSync,
  readdirSync,
  realpathSync,
  rmSync,
  symlinkSync,
} from 'node:fs';
import {homedir, tmpdir} from 'node:os';
import {join, posix, win32} from 'node:path';

/** How a program ran. */
export interface Ran {
  /** Why it could not start, as Node names it (`ENOENT`), or null. */
  failure: string | null;
  status: number | null;
  /** What it printed, stdout then stderr. */
  output: string;
}

/** The machine the check runs on. Tests pass a stand-in. */
export interface Host {
  platform: NodeJS.Platform;
  home: string;
  run(
    command: string,
    args: string[],
    cwd: string | undefined,
    environment: NodeJS.ProcessEnv,
  ): Promise<Ran>;
  /** The names in a folder, or none when it can't be read. */
  list(folder: string): string[];
  /** A path with its links followed. */
  real(path: string): string;
  /** Makes a folder that holds one link, and returns the folder. */
  link(name: string, target: string): string;
  remove(folder: string): void;
}

/** A GDB that passed. */
export interface CheckedGdb {
  /** The environment to run it in. */
  environment: NodeJS.ProcessEnv;
  /** Something to tell the developer, or null. */
  note: string | null;
  /** Removes what the check made. */
  cleanUp(): void;
}

type Version = [major: number, minor: number];

const GDB_MAJOR = 14;
const RUN_TIMEOUT_MS = 15000;
const OUTPUT_KEPT = 600;
const LINES_KEPT = 3;
const PYTHON_MARK = 'erb-python ';
const PYTHON_VERSION = "import sys; print('%d.%d' % sys.version_info[:2])";
const PROBE = [
  '-nx',
  '-batch',
  '-ex',
  'show version',
  '-ex',
  `python import sys; print('${PYTHON_MARK}%d.%d' % sys.version_info[:2])`,
];
/** Windows's exit status for a program whose DLL is missing. */
const DLL_NOT_FOUND = 0xc0000135;
const GUIDE = 'https://embedded-react.dev/guides/debugging#a-gdb-for-the-board';

const hostMachine: Host = {
  platform: process.platform,
  home: homedir(),
  run: (command, args, cwd, environment) =>
    new Promise(done => {
      execFile(
        command,
        args,
        {cwd, env: environment, timeout: RUN_TIMEOUT_MS, windowsHide: true},
        (error: any, stdout, stderr) =>
          done({
            failure: typeof error?.code === 'string' ? error.code : null,
            status: error
              ? typeof error.code === 'number'
                ? error.code
                : null
              : 0,
            output: `${stdout}${stderr}`,
          }),
      );
    }),
  list: folder => {
    try {
      return readdirSync(folder);
    } catch {
      return [];
    }
  },
  real: path => {
    try {
      return realpathSync(path);
    } catch {
      return path;
    }
  },
  link: (name, target) => {
    const folder = mkdtempSync(join(tmpdir(), 'embedded-react-gdb-'));
    symlinkSync(target, join(folder, name));
    return folder;
  },
  remove: folder => rmSync(folder, {recursive: true, force: true}),
};

/**
 * Checks a GDB before a debug session runs it.
 *
 * @param command  The GDB, as the configuration names it.
 * @param cwd  The folder it runs in.
 * @param environment  Its environment.
 * @param host  The machine. Tests pass a stand-in.
 *
 * @returns The environment to run it in.
 * @throws An error that says what is wrong with it and what to do.
 */
export async function checkGdb(
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
  host: Host = hostMachine,
): Promise<CheckedGdb> {
  const probe = await host.run(command, PROBE, cwd, environment);
  const version = started(command, probe, host);
  if (pythonOf(probe.output)) {
    return {environment, note: null, cleanUp: () => {}};
  }
  if (!/Python scripting is not supported/.test(probe.output)) {
    throw new Error(
      `${command} has Python, but could not start it: ${tail(probe.output)}. Check PYTHONHOME and PYTHONPATH in the environment.`,
    );
  }
  const path = findProgram(command, cwd, environment, host);
  const builds = path ? pythonBuilds(path, host) : [];
  if (builds.length) {
    return withPython(command, cwd, environment, path!, builds, host);
  }
  const withIt = path && sibling(path, '-py', host);
  throw new Error(
    `${command} is GDB ${version.join('.')}, built without Python, which the debugger needs. ${
      withIt
        ? `Use ${withIt}, beside it.`
        : `Give "gdb" one that has it: ${otherGdb(host.platform)}.`
    } ${GUIDE}`,
  );
}

/**
 * Tells a GDB that ran from one that didn't, or is too old.
 *
 * @param command  The GDB, as the configuration names it.
 * @param probe  How it ran.
 * @param host  The machine.
 *
 * @returns Its version.
 * @throws An error for one that didn't start or is too old.
 */
function started(command: string, probe: Ran, host: Host): Version {
  if (probe.failure === 'ENOENT') {
    throw new Error(
      `${command} was not found. Give "gdb" its full path, or put its folder on PATH.`,
    );
  }
  const line = /^GNU gdb\b.*?(\d+)\.(\d+)[^\s)]*$/m.exec(probe.output);
  if (!line) {
    const python = missingPython(probe);
    throw new Error(
      python
        ? `${command} could not start: it is linked against Python${python === true ? '' : ` ${python}`}, which isn't installed where it looks. Install that Python, or give "gdb" another: ${otherGdb(host.platform)}. ${GUIDE}`
        : `${command} could not start: ${tail(probe.output) || probe.failure || `it ended with status ${probe.status}`}.`,
    );
  }
  const version: Version = [Number(line[1]), Number(line[2])];
  if (version[0] < GDB_MAJOR) {
    throw new Error(
      `${command} is GDB ${version.join('.')}. The debugger needs GDB ${GDB_MAJOR} or later, built with Python. Give "gdb" a newer one: ${otherGdb(host.platform)}. ${GUIDE}`,
    );
  }
  return version;
}

/**
 * The Python a program that didn't start was linked against.
 *
 * @param probe  How it ran.
 *
 * @returns Its version, true when the loader didn't name one, or null when Python wasn't what it missed.
 */
function missingPython(probe: Ran): string | true | null {
  if (probe.status === DLL_NOT_FOUND) {
    return true;
  }
  if (
    !/Library not loaded|error while loading shared libraries/.test(
      probe.output,
    )
  ) {
    return null;
  }
  const named =
    /libpython(\d+\.\d+)|python@(\d+\.\d+)|Python\.framework\/Versions\/(\d+\.\d+)/.exec(
      probe.output,
    );
  return named ? (named[1] ?? named[2] ?? named[3]) : null;
}

/**
 * The Python version the probe printed.
 *
 * @param output  What GDB printed.
 *
 * @returns The version, or null with no Python.
 */
function pythonOf(output: string): string | null {
  const at = output.lastIndexOf(PYTHON_MARK);
  return at < 0 ? null : output.slice(at + PYTHON_MARK.length).split(/\s/)[0];
}

/**
 * Runs Espressif's GDB with a Python it has a build for.
 *
 * @param command  The GDB, as the configuration names it.
 * @param cwd  The folder it runs in.
 * @param environment  Its environment.
 * @param path  Where the GDB is.
 * @param builds  The Python versions it has builds for, newest first.
 * @param host  The machine.
 *
 * @returns The environment that gives it one.
 * @throws An error when the machine has none of them.
 */
async function withPython(
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
  path: string,
  builds: Version[],
  host: Host,
): Promise<CheckedGdb> {
  const windows = host.platform === 'win32';
  const paths = windows ? win32 : posix;
  const python = windows ? 'python.exe' : 'python3';
  const has = (version: string) =>
    builds.some(build => build.join('.') === version);
  const versionIn = async (folder: string): Promise<string | null> => {
    if (!host.list(folder).includes(python)) {
      return null;
    }
    const ran = await host.run(
      paths.join(folder, python),
      ['-c', PYTHON_VERSION],
      cwd,
      environment,
    );
    return ran.status === 0 ? ran.output.trim() : null;
  };
  const attempt = async (
    folder: string,
    made: boolean,
  ): Promise<CheckedGdb | null> => {
    const changed = withFirst(environment, folder, host);
    const probe = await host.run(command, PROBE, cwd, changed);
    const version = pythonOf(probe.output);
    if (!version) {
      if (made) {
        host.remove(folder);
      }
      return null;
    }
    return {
      environment: changed,
      note: `${command} runs with Python ${version}.`,
      cleanUp: () => {
        if (made) {
          host.remove(folder);
        }
      },
    };
  };

  // ESP-IDF's own Python first, then any on PATH.
  for (const folder of [
    ...idfPythons(environment, host),
    ...pathFolders(environment, host),
  ]) {
    const version = await versionIn(folder);
    const found = version && has(version) && (await attempt(folder, false));
    if (found) {
      return found;
    }
  }
  // A `python3.12` beside a `python3` of another version is run through a link named `python3`.
  if (!windows) {
    for (const build of builds) {
      const name = `python${build.join('.')}`;
      const folder = pathFolders(environment, host).find(candidate =>
        host.list(candidate).includes(name),
      );
      const found =
        folder &&
        (await attempt(host.link(python, paths.join(folder, name)), true));
      if (found) {
        return found;
      }
    }
  }

  const first = pathFolders(environment, host).find(folder =>
    host.list(folder).includes(python),
  );
  const firstVersion = first && (await versionIn(first));
  const oldest = builds.at(-1)!.join('.');
  const newest = builds[0].join('.');
  throw new Error(
    `${command} has builds for Python ${oldest} to ${newest}, and ${
      firstVersion
        ? `the Python on PATH is ${firstVersion}`
        : 'PATH has no Python'
    }, so it ran with none. Install a Python from ${oldest} to ${newest}. ${paths.dirname(path)} lists the builds.`,
  );
}

/**
 * The Python versions Espressif's GDB has builds for.
 *
 * @param path  Where the GDB is.
 * @param host  The machine.
 *
 * @returns The versions, newest first. None for any other GDB.
 */
function pythonBuilds(path: string, host: Host): Version[] {
  const paths = host.platform === 'win32' ? win32 : posix;
  return host
    .list(paths.dirname(path))
    .map(name => /-gdb-(\d+)\.(\d+)(?:\.exe)?$/i.exec(name))
    .filter(match => match !== null)
    .map((match): Version => [Number(match[1]), Number(match[2])])
    .sort((left, right) => right[0] - left[0] || right[1] - left[1]);
}

/**
 * The folders of the Pythons ESP-IDF's installer made.
 *
 * @param environment  GDB's environment.
 * @param host  The machine.
 *
 * @returns The folders that hold each one's `python3`, the exported one first.
 */
function idfPythons(environment: NodeJS.ProcessEnv, host: Host): string[] {
  const paths = host.platform === 'win32' ? win32 : posix;
  const programs = host.platform === 'win32' ? 'Scripts' : 'bin';
  const tools =
    variable(environment, 'IDF_TOOLS_PATH', host) ??
    paths.join(host.home, '.espressif');
  const all = paths.join(tools, 'python_env');
  const exported = variable(environment, 'IDF_PYTHON_ENV_PATH', host);
  return [
    ...(exported ? [exported] : []),
    ...host
      .list(all)
      .sort()
      .reverse()
      .map(name => paths.join(all, name)),
  ].map(folder => paths.join(folder, programs));
}

/**
 * Where a program is.
 *
 * @param command  The program, as a path or a name on PATH.
 * @param cwd  The folder a relative path is from.
 * @param environment  The environment whose PATH is searched.
 * @param host  The machine.
 *
 * @returns Its path with links followed, or null when it isn't found.
 */
function findProgram(
  command: string,
  cwd: string | undefined,
  environment: NodeJS.ProcessEnv,
  host: Host,
): string | null {
  const windows = host.platform === 'win32';
  const paths = windows ? win32 : posix;
  const folders = /[\\/]/.test(command) ? [''] : pathFolders(environment, host);
  const endings = windows
    ? ['', ...(variable(environment, 'PATHEXT', host) ?? '.EXE').split(';')]
    : [''];
  for (const folder of folders) {
    for (const ending of endings) {
      const candidate = paths.resolve(cwd ?? '.', folder, command + ending);
      const name = paths.basename(candidate);
      const found = host
        .list(paths.dirname(candidate))
        .find(entry =>
          windows ? entry.toLowerCase() === name.toLowerCase() : entry === name,
        );
      if (found) {
        return host.real(paths.join(paths.dirname(candidate), found));
      }
    }
  }
  return null;
}

/**
 * A program beside another, named as it is with something added.
 *
 * @param path  The program.
 * @param added  What the other's name adds.
 * @param host  The machine.
 *
 * @returns The other's path, or null when there is none.
 */
function sibling(path: string, added: string, host: Host): string | null {
  const paths = host.platform === 'win32' ? win32 : posix;
  const ending = /\.exe$/i.exec(path)?.[0] ?? '';
  const name = paths.basename(path, ending) + added + ending;
  return host.list(paths.dirname(path)).includes(name)
    ? paths.join(paths.dirname(path), name)
    : null;
}

/**
 * An environment variable, whatever its case on Windows.
 *
 * @param environment  The environment.
 * @param name  The variable.
 * @param host  The machine.
 *
 * @returns Its name there and its value, or undefined.
 */
function variable(
  environment: NodeJS.ProcessEnv,
  name: string,
  host: Host,
): string | undefined {
  return environment[variableName(environment, name, host)] || undefined;
}

function variableName(
  environment: NodeJS.ProcessEnv,
  name: string,
  host: Host,
): string {
  if (host.platform !== 'win32') {
    return name;
  }
  return (
    Object.keys(environment).find(
      key => key.toUpperCase() === name.toUpperCase(),
    ) ?? name
  );
}

/**
 * The folders on PATH.
 *
 * @param environment  The environment.
 * @param host  The machine.
 *
 * @returns The folders, in order.
 */
function pathFolders(environment: NodeJS.ProcessEnv, host: Host): string[] {
  const paths = host.platform === 'win32' ? win32 : posix;
  return (variable(environment, 'PATH', host) ?? '')
    .split(paths.delimiter)
    .filter(Boolean);
}

/**
 * An environment with a folder first on PATH.
 *
 * @param environment  The environment.
 * @param folder  The folder.
 * @param host  The machine.
 *
 * @returns The new environment.
 */
function withFirst(
  environment: NodeJS.ProcessEnv,
  folder: string,
  host: Host,
): NodeJS.ProcessEnv {
  const paths = host.platform === 'win32' ? win32 : posix;
  return {
    ...environment,
    [variableName(environment, 'PATH', host)]: [
      folder,
      ...pathFolders(environment, host),
    ].join(paths.delimiter),
  };
}

/**
 * A GDB with Python that debugs Arm boards, for a toolchain whose own can't.
 *
 * @param platform  The machine's system.
 *
 * @returns What to install, as a message says it.
 */
function otherGdb(platform: NodeJS.Platform): string {
  switch (platform) {
    case 'darwin':
      return "Homebrew's gdb (brew install gdb)";
    case 'win32':
      return "MSYS2's gdb-multiarch";
    default:
      return "your system's gdb-multiarch";
  }
}

/**
 * The end of what a program printed, on one line.
 *
 * @param output  What it printed.
 *
 * @returns Its last lines.
 */
function tail(output: string): string {
  return output
    .trim()
    .split(/\s*\n\s*/)
    .slice(-LINES_KEPT)
    .join(' / ')
    .slice(-OUTPUT_KEPT);
}

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

import {describe, expect, it} from 'vitest';
import {checkGdb, type Host, type Ran} from '../gdb-check.mts';

const NO_PYTHON = 'Python scripting is not supported in this copy of GDB.\n';
const ESP_BIN = '/esp/tools/xtensa-esp-elf-gdb/bin';
const ESP_GDB = `${ESP_BIN}/xtensa-esp32s3-elf-gdb`;

const ran = (output: string, status = 0): Ran => ({
  failure: null,
  status,
  output,
});

interface Machine {
  platform?: NodeJS.Platform;
  /** Each folder's names. */
  folders: Record<string, string[]>;
  /** Each file's bytes. */
  files?: Record<string, string>;
  /** What each program prints, by its path. GDB's is given the first folder on its PATH. */
  programs: Record<string, (firstOnPath: string) => Ran>;
}

/** A machine with the folders and programs a test gives it. */
function machine(setup: Machine): Host & {made: string[]; removed: string[]} {
  const windows = setup.platform === 'win32';
  const folders = {...setup.folders};
  const made: string[] = [];
  const removed: string[] = [];
  return {
    platform: setup.platform ?? 'linux',
    home: windows ? 'C:\\Users\\dev' : '/home/dev',
    made,
    removed,
    run: async (command, _args, cwd, environment) => {
      const pathFolders = (environment.PATH ?? environment.Path ?? '')
        .split(windows ? ';' : ':')
        .map(folder => folder || cwd || '.');
      const separator = windows ? '\\' : '/';
      const path = /[\\/]/.test(command)
        ? command
        : pathFolders
            .filter(folder => folders[folder]?.includes(command))
            .map(folder => `${folder}${separator}${command}`)[0];
      const program = setup.programs[path];
      return program
        ? program(pathFolders[0])
        : {failure: 'ENOENT', status: null, output: ''};
    },
    list: folder => folders[folder] ?? [],
    real: path => path,
    read: path => setup.files?.[path] ?? '',
    link: (name, target) => {
      const folder = `/tmp/link-${made.length}`;
      folders[folder] = [name];
      setup.programs[`${folder}/${name}`] = setup.programs[target];
      made.push(target);
      return folder;
    },
    remove: folder => {
      removed.push(folder);
    },
  };
}

/** Espressif's launcher: the build for the first Python on PATH, or the one with none. */
function espressif(pythons: Record<string, string>): (first: string) => Ran {
  return first =>
    ran(
      `GNU gdb (esp-gdb) 17.1_20260402\n${
        /^3\.(8|9|1[0-4])$/.test(pythons[first] ?? '')
          ? `erb-python ${pythons[first]}\n`
          : NO_PYTHON
      }`,
    );
}

const ESP_BUILDS = [
  'xtensa-esp32s3-elf-gdb',
  'xtensa-esp-elf-gdb-3.8',
  'xtensa-esp-elf-gdb-3.12',
  'xtensa-esp-elf-gdb-3.14',
  'xtensa-esp-elf-gdb-no-python',
];

describe('checkGdb', () => {
  it('passes a GDB 14 with Python as it is', async () => {
    const environment = {PATH: '/usr/bin'};
    const checked = await checkGdb(
      'gdb',
      undefined,
      environment,
      machine({
        folders: {'/usr/bin': ['gdb']},
        programs: {
          '/usr/bin/gdb': () =>
            ran('GNU gdb (Debian 16.3-1) 16.3\nerb-python 3.13\n'),
        },
      }),
    );
    expect(checked.environment).toBe(environment);
    expect(checked.note).toBeNull();
  });

  it('says when the GDB is not found', async () => {
    await expect(
      checkGdb(
        'gdb',
        undefined,
        {PATH: '/usr/bin'},
        machine({folders: {}, programs: {}}),
      ),
    ).rejects.toThrow('gdb was not found. Give "gdb" its full path');
  });

  it('says when the GDB is too old', async () => {
    await expect(
      checkGdb(
        'gdb-multiarch',
        undefined,
        {PATH: '/usr/bin'},
        machine({
          folders: {'/usr/bin': ['gdb-multiarch']},
          programs: {
            '/usr/bin/gdb-multiarch': () =>
              ran(
                'GNU gdb (Ubuntu 12.1-0ubuntu1~22.04.2) 12.1\nerb-python 3.10\n',
              ),
          },
        }),
      ),
    ).rejects.toThrow(
      'gdb-multiarch is GDB 12.1. The debugger needs GDB 14 or later',
    );
  });

  it('names the GDB with Python beside one built without', async () => {
    const bin = '/opt/arm/bin';
    await expect(
      checkGdb(
        'arm-none-eabi-gdb',
        undefined,
        {PATH: bin},
        machine({
          platform: 'darwin',
          folders: {[bin]: ['arm-none-eabi-gdb', 'arm-none-eabi-gdb-py']},
          programs: {
            [`${bin}/arm-none-eabi-gdb`]: () =>
              ran(
                `GNU gdb (Arm GNU Toolchain 15.3.Rel2 (Build arm-15.181)) 16.3.90.20250906-git\n${NO_PYTHON}`,
              ),
          },
        }),
      ),
    ).rejects.toThrow(
      `arm-none-eabi-gdb is GDB 16.3, built without Python, which the debugger needs. Use ${bin}/arm-none-eabi-gdb-py, beside it.`,
    );
  });

  it('names another GDB for one built without Python', async () => {
    await expect(
      checkGdb(
        '/st/bin/arm-none-eabi-gdb',
        undefined,
        {PATH: ''},
        machine({
          folders: {'/st/bin': ['arm-none-eabi-gdb']},
          programs: {
            '/st/bin/arm-none-eabi-gdb': () =>
              ran(
                `GNU gdb (GNU Tools for STM32 14.3.rel1.20251027-0700) 15.2.90.20241229-git\n${NO_PYTHON}`,
              ),
          },
        }),
      ),
    ).rejects.toThrow(
      'Give "gdb" one that has it: your system\'s gdb-multiarch',
    );
  });

  it('names the Python a GDB that could not start is linked against', async () => {
    await expect(
      checkGdb(
        '/opt/arm/bin/arm-none-eabi-gdb',
        undefined,
        {PATH: ''},
        machine({
          folders: {},
          programs: {
            '/opt/arm/bin/arm-none-eabi-gdb': () =>
              ran(
                'arm-none-eabi-gdb: error while loading shared libraries: libpython3.8.so.1.0: cannot open shared object file: No such file or directory\n',
                127,
              ),
          },
        }),
      ),
    ).rejects.toThrow("it is linked against Python 3.8, which isn't installed");
  });

  it('names the Python a Windows GDB needs when its DLL is not found', async () => {
    const gdb = 'C:\\arm\\bin\\arm-none-eabi-gdb-py.exe';
    const windows = (folders: Record<string, string[]>) =>
      machine({
        platform: 'win32',
        folders: {'C:\\arm\\bin': ['arm-none-eabi-gdb-py.exe'], ...folders},
        files: {[gdb]: 'MZ...KERNEL32.dll\0python38.dll\0...'},
        programs: {[gdb]: () => ran('', 0xc0000135)},
      });
    await expect(
      checkGdb(gdb, undefined, {Path: 'C:\\tools'}, windows({})),
    ).rejects.toThrow("it is linked against Python 3.8, which isn't installed");
    // With Python's DLL there, another one is missing.
    await expect(
      checkGdb(
        gdb,
        undefined,
        {Path: 'C:\\Python38'},
        windows({'C:\\Python38': ['Python38.dll']}),
      ),
    ).rejects.toThrow('Windows could not find a DLL it needs');
  });

  it('does not blame Python for a missing DLL of a Windows GDB that needs none', async () => {
    const gdb = 'C:\\arm\\bin\\arm-none-eabi-gdb.exe';
    await expect(
      checkGdb(
        gdb,
        undefined,
        {Path: ''},
        machine({
          platform: 'win32',
          folders: {'C:\\arm\\bin': ['arm-none-eabi-gdb.exe']},
          files: {[gdb]: 'MZ...KERNEL32.dll\0libstdc++-6.dll\0...'},
          programs: {[gdb]: () => ran('', 0xc0000135)},
        }),
      ),
    ).rejects.toThrow(
      `${gdb} could not start: Windows could not find a DLL it needs. Run it from a Command Prompt to see which.`,
    );
  });

  it('says when Python is there and does not start', async () => {
    await expect(
      checkGdb(
        'gdb',
        undefined,
        {PATH: '/usr/bin'},
        machine({
          folders: {'/usr/bin': ['gdb']},
          programs: {
            '/usr/bin/gdb': () =>
              ran('GNU gdb (GDB) 18.1\nPython not initialized\n'),
          },
        }),
      ),
    ).rejects.toThrow('gdb has Python, but could not start it');
  });

  it("gives Espressif's GDB the Python ESP-IDF installed", async () => {
    const idf = '/home/dev/.espressif/python_env/idf6.1_py3.12_env/bin';
    const pythons = {'/usr/bin': '3.15', [idf]: '3.12'};
    const checked = await checkGdb(
      'xtensa-esp32s3-elf-gdb',
      undefined,
      {PATH: `/usr/bin:${ESP_BIN}`},
      machine({
        folders: {
          '/usr/bin': ['python3'],
          [ESP_BIN]: ESP_BUILDS,
          '/home/dev/.espressif/python_env': ['idf6.1_py3.12_env'],
          [idf]: ['python3'],
        },
        programs: {
          [ESP_GDB]: espressif(pythons),
          '/usr/bin/python3': () => ran('3.15\n'),
          [`${idf}/python3`]: () => ran('3.12\n'),
        },
      }),
    );
    expect(checked.environment.PATH).toBe(`${idf}:/usr/bin:${ESP_BIN}`);
    expect(checked.note).toBe('xtensa-esp32s3-elf-gdb runs with Python 3.12.');
  });

  it("links a versioned Python for Espressif's GDB, and removes the link", async () => {
    const host = machine({
      folders: {
        '/usr/bin': ['python3', 'python3.14', 'python3.15'],
        [ESP_BIN]: ESP_BUILDS,
      },
      programs: {
        [ESP_GDB]: first =>
          espressif({[first]: first.startsWith('/tmp/link') ? '3.14' : '3.15'})(
            first,
          ),
        '/usr/bin/python3': () => ran('3.15\n'),
      },
    });
    const checked = await checkGdb(
      ESP_GDB,
      undefined,
      {PATH: '/usr/bin'},
      host,
    );
    expect(host.made).toEqual(['/usr/bin/python3.14']);
    expect(checked.environment.PATH).toBe('/tmp/link-0:/usr/bin');
    checked.cleanUp();
    expect(host.removed).toEqual(['/tmp/link-0']);
  });

  it('reads an empty PATH entry as the folder GDB runs in', async () => {
    const idf = '/home/dev/.espressif/python_env/idf6.1_py3.12_env/bin';
    const checked = await checkGdb(
      'xtensa-esp32s3-elf-gdb',
      ESP_BIN,
      {PATH: ':/usr/bin'},
      machine({
        folders: {
          '/usr/bin': ['python3'],
          [ESP_BIN]: ESP_BUILDS,
          '/home/dev/.espressif/python_env': ['idf6.1_py3.12_env'],
          [idf]: ['python3'],
        },
        programs: {
          [ESP_GDB]: espressif({'/usr/bin': '3.15', [idf]: '3.12'}),
          '/usr/bin/python3': () => ran('3.15\n'),
          [`${idf}/python3`]: () => ran('3.12\n'),
        },
      }),
    );
    expect(checked.environment.PATH).toBe(`${idf}::/usr/bin`);
  });

  it("says which Pythons Espressif's GDB has builds for when none is installed", async () => {
    await expect(
      checkGdb(
        ESP_GDB,
        undefined,
        {PATH: '/usr/bin'},
        machine({
          folders: {'/usr/bin': ['python3'], [ESP_BIN]: ESP_BUILDS},
          programs: {
            [ESP_GDB]: espressif({'/usr/bin': '3.15'}),
            '/usr/bin/python3': () => ran('3.15\n'),
          },
        }),
      ),
    ).rejects.toThrow(
      'has builds for Python 3.8 to 3.14, and the Python on PATH is 3.15, so it ran with none. Install a Python from 3.8 to 3.14.',
    );
  });
});

# Copyright 2026 Cory Lamming
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""GDB commands for a Flow B app on a halted board.

Load the firmware's ELF, then this script, then the app's debug file:

    (gdb) source node_modules/embedded-react/gdb/erb.py
    (gdb) erb file dist/app.erdbg

The commands read the board's memory and run nothing on it. They find the runtime and the engine through the
descriptors the firmware exports (erb_debug and er_debug), so they work on any board and need no debug info
beyond the ELF's symbols. GDB must be built with Python 3.8 or later.
"""

import json
import os
import struct

import gdb

DEBUG_FILE_FORMAT = 1
ERB_DEBUG_MAGIC = 0x44425245
ERB_DEBUG_VERSION = 1
ER_DEBUG_MAGIC = 0x45445245
ER_DEBUG_VERSION = 1

NO_FUNCTION = 0xFFFF
NO_TAG = 0xFFFF
HANDLE_BYTES = 8
LIST_HEADER_BYTES = 4
TIMER_BYTES = 16
TIMER_ACTIVE_AT = 12
ANIMATION_ENTRY_BYTES = 20
ANIMATION_STATE_AT = 14
MOST_FRAMES = 64
MOST_ROWS_SHOWN = 8
LONGEST_STRING = 256

POOL_NAMES = [
    "nodes",
    "vector nodes",
    "animations",
    "animation groups",
    "animated values",
    "images",
    "font families",
]

RESOURCE_NAMES = {
    0: "arena",
    0x100: "nodes",
    0x101: "package slot",
    1: "vector nodes",
    2: "animated values",
    3: "animations",
    4: "animation groups",
    5: "images",
    6: "fonts",
}

ERROR_TOO_BIG = 7
ERROR_ENGINE_ABI = 5
ERROR_NO_PANEL = 6
ERROR_STACK = 21


class ErbError(gdb.GdbError):
    """A command's failure, shown without a Python traceback."""


# ----------------------------------------------------------------------------------------------------------------------
# Reading the target
# ----------------------------------------------------------------------------------------------------------------------


target_shape = {}


def pointer_bytes():
    if "pointer" not in target_shape:
        target_shape["pointer"] = gdb.lookup_type("void").pointer().sizeof
    return target_shape["pointer"]


def endian():
    if "endian" not in target_shape:
        target_shape["endian"] = ">" if "big" in gdb.execute("show endian", to_string=True) else "<"
    return target_shape["endian"]


# A new ELF can be for another target.
gdb.events.new_objfile.connect(lambda event: target_shape.clear())


def read(address, length):
    try:
        return bytes(gdb.selected_inferior().read_memory(address, length))
    except gdb.MemoryError:
        raise ErbError("can't read 0x%x (%d bytes): is the board halted and the ELF the one it runs?" % (address, length))


def unpack(fmt, data, at=0):
    return struct.unpack_from(endian() + fmt, data, at)[0]


def read_value(fmt, address):
    return unpack(fmt, read(address, struct.calcsize(fmt)))


def read_pointer(address):
    return read_value("I" if pointer_bytes() == 4 else "Q", address)


def read_string(address, limit=LONGEST_STRING):
    if not address:
        return None
    data = read(address, limit)
    end = data.find(b"\0")
    return data[: end if end >= 0 else limit].decode("utf-8", "replace")


def write(address, data):
    gdb.selected_inferior().write_memory(address, data)


def symbol_address(name):
    try:
        return int(gdb.parse_and_eval("(unsigned long)&%s" % name))
    except gdb.error:
        raise ErbError(
            "no %s in the ELF: load the firmware's ELF first, built with the Flow B runtime of this version" % name
        )


class Layout:
    """A C struct's field offsets, worked out with natural alignment for the target's pointer size."""

    def __init__(self, fields):
        sizes = {"u8": 1, "u16": 2, "u32": 4, "ptr": pointer_bytes()}
        self.offsets = {}
        offset = 0
        largest = 1
        for name, kind, count in fields:
            size = sizes[kind]
            offset = (offset + size - 1) // size * size
            self.offsets[name] = offset
            offset += size * count
            largest = max(largest, size)
        self.size = (offset + largest - 1) // largest * largest


def runtime_layout():
    return Layout(
        [
            ("magic", "u32", 1),
            ("version", "u16", 1),
            ("frame_cells", "u16", 1),
            ("engine", "ptr", 1),
            ("reasons", "ptr", 1),
            ("reason_count", "u16", 1),
            ("reserved", "u16", 1),
            ("error", "ptr", 1),
            ("error_fields", "u16", 10),
            ("loaded", "ptr", 1),
            ("faulted", "ptr", 1),
            ("in_vm", "ptr", 1),
            ("build_id", "ptr", 1),
            ("panel", "ptr", 1),
            ("arena", "ptr", 1),
            ("stack_offset", "ptr", 1),
            ("function", "ptr", 1),
            ("program_counter", "ptr", 1),
            ("frame_base", "ptr", 1),
            ("update", "ptr", 1),
        ]
    )


def engine_layout():
    return Layout(
        [
            ("magic", "u32", 1),
            ("version", "u16", 1),
            ("pool_count", "u16", 1),
            ("pools", "ptr", len(POOL_NAMES)),
            ("node", "u16", 10),
            ("root_tag", "ptr", 1),
        ]
    )


def pool_layout():
    return Layout([("records", "ptr", 1), ("count", "u16", 1), ("stride", "u16", 1), ("used_at", "u16", 1)])


ERROR_FIELDS = [
    "status_at",
    "status_bytes",
    "detail_at",
    "needed_at",
    "available_at",
    "resource_at",
    "function_at",
    "offset_at",
    "name_at",
    "build_id_at",
]

NODE_FIELDS = [
    "tag_at",
    "parent_at",
    "first_child_at",
    "next_sibling_at",
    "serial_at",
    "type_at",
    "type_bytes",
    "rect_at",
    "scroll_x_at",
    "scroll_y_at",
]


class Runtime:
    """The runtime's descriptor, read from the board."""

    def __init__(self):
        address = symbol_address("erb_debug")
        layout = runtime_layout()
        data = read(address, layout.size)
        field = lambda name: layout.offsets[name]
        if unpack("I", data, field("magic")) != ERB_DEBUG_MAGIC:
            raise ErbError("erb_debug doesn't hold the runtime's descriptor: is the ELF the one the board runs?")
        version = unpack("H", data, field("version"))
        if version != ERB_DEBUG_VERSION:
            raise ErbError(
                "the firmware's runtime descriptor is version %d, but this script reads version %d: "
                "use the script from the embedded-react the firmware was built with" % (version, ERB_DEBUG_VERSION)
            )
        pointer = "I" if pointer_bytes() == 4 else "Q"
        self.pointer = lambda name: unpack(pointer, data, field(name))
        self.frame_cells = unpack("H", data, field("frame_cells"))
        self.engine_address = self.pointer("engine")
        self.reasons = self.pointer("reasons")
        self.reason_count = unpack("H", data, field("reason_count"))
        self.error_address = self.pointer("error")
        self.error_fields = {
            name: unpack("H", data, field("error_fields") + 2 * index) for index, name in enumerate(ERROR_FIELDS)
        }

    def flag(self, name):
        return read_value("B", self.pointer(name)) != 0

    def u16(self, name):
        return read_value("H", self.pointer(name))

    def u32(self, name):
        return read_value("I", self.pointer(name))

    @property
    def arena(self):
        return read_pointer(self.pointer("arena"))

    def reason(self, status):
        if 0 <= status < self.reason_count:
            return read_string(read_pointer(self.reasons + status * pointer_bytes()))
        return "unknown status %d" % status

    def request_update(self):
        write(self.pointer("update"), b"\x01")


class Engine:
    """The engine's descriptor, read from the board."""

    def __init__(self, address):
        layout = engine_layout()
        data = read(address, layout.size)
        field = lambda name: layout.offsets[name]
        if unpack("I", data, field("magic")) != ER_DEBUG_MAGIC:
            raise ErbError("the engine's descriptor is missing: is the ELF the one the board runs?")
        version = unpack("H", data, field("version"))
        if version != ER_DEBUG_VERSION:
            raise ErbError(
                "the firmware's engine descriptor is version %d, but this script reads version %d" % (version, ER_DEBUG_VERSION)
            )
        pointer = "I" if pointer_bytes() == 4 else "Q"
        count = min(unpack("H", data, field("pool_count")), len(POOL_NAMES))
        self.pools = []
        pool = pool_layout()
        for index in range(count):
            record = read(unpack(pointer, data, field("pools") + index * pointer_bytes()), pool.size)
            self.pools.append(
                {
                    "records": unpack(pointer, record, pool.offsets["records"]),
                    "count": unpack("H", record, pool.offsets["count"]),
                    "stride": unpack("H", record, pool.offsets["stride"]),
                    "used_at": unpack("H", record, pool.offsets["used_at"]),
                }
            )
        self.node = {name: unpack("H", data, field("node") + 2 * index) for index, name in enumerate(NODE_FIELDS)}
        self.root_tag_address = unpack(pointer, data, field("root_tag"))

    def records(self, pool):
        """Each record of a pool, as bytes, and whether it is in use."""
        data = read(pool["records"], pool["count"] * pool["stride"])
        for index in range(pool["count"]):
            record = data[index * pool["stride"] : (index + 1) * pool["stride"]]
            yield index, record, record[pool["used_at"]] != 0


# ----------------------------------------------------------------------------------------------------------------------
# The debug file and the running app
# ----------------------------------------------------------------------------------------------------------------------

debug_file = None
debug_path = None


def load_debug_file(path):
    global debug_file, debug_path
    path = os.path.expanduser(path)
    try:
        with open(path, encoding="utf-8") as handle:
            loaded = json.load(handle)
    except OSError as error:
        raise ErbError("can't read %s: %s" % (path, error.strerror))
    except ValueError:
        raise ErbError("%s is not a debug file: build the app again to write a new app.erdbg" % path)
    if not isinstance(loaded, dict) or loaded.get("format") != DEBUG_FILE_FORMAT:
        raise ErbError(
            "%s is a debug file of format %s, but this script reads format %d: build the app with the embedded-react "
            "this script came from" % (path, loaded.get("format") if isinstance(loaded, dict) else "unknown", DEBUG_FILE_FORMAT)
        )
    debug_file = loaded
    debug_path = path


def build_hex(build_id):
    return "%08x" % (build_id & 0xFFFFFFFF)


class App:
    """The running app: the descriptors, and the debug file's panel for the layout the board runs."""

    def __init__(self, need_running=True):
        self.runtime = Runtime()
        if debug_file is None:
            raise ErbError("no debug file: load the app's with `erb file dist/app.erdbg`")
        self.loaded = self.runtime.flag("loaded")
        self.build_id = self.runtime.u32("build_id")
        if need_running and not self.loaded:
            raise ErbError("no app is running: `erb error` says why")
        if self.loaded and self.build_id != debug_file["buildId"]:
            raise ErbError(
                "the board runs build %s, but %s is build %s: load the debug file of the build the board runs"
                % (build_hex(self.build_id), debug_path, build_hex(debug_file["buildId"]))
            )
        panel = self.runtime.u16("panel") if self.loaded else 0
        if panel >= len(debug_file["panels"]):
            raise ErbError("the board runs layout %d, but the debug file has %d" % (panel, len(debug_file["panels"])))
        self.panel = debug_file["panels"][panel]
        self.arena = self.runtime.arena if self.loaded else 0

    def place(self, function, offset):
        """`App.jsx:41 onPress` for a function and offset, as the board's error screen finds it."""
        functions = self.panel["functions"]
        if function >= len(functions):
            return "function %d at %d" % (function, offset)
        entry = functions[function]
        line = None
        for candidate in entry["lines"]:
            if candidate["offset"] > offset:
                break
            line = candidate
        name = entry["name"] or "function %d" % function
        if line is None:
            return "%s (function %d at %d)" % (name, function, offset)
        return "%s:%d  %s" % (debug_file["file"], line["line"], name)

    def frames(self):
        """The app's call chain, innermost first: each function and the offset it is at."""
        runtime = self.runtime
        frames = [(runtime.u16("function"), runtime.u32("program_counter"))]
        stack = self.arena + runtime.u32("stack_offset")
        base = runtime.u32("frame_base")
        for _ in range(MOST_FRAMES):
            cells = read(stack + 4 * base, 12)
            caller = unpack("i", cells, 0)
            if caller < 0:
                break
            # The caller resumes after its CALL, which is one word.
            frames.append((caller, unpack("I", cells, 4) - 1))
            base = unpack("I", cells, 8)
        return frames

    def state(self, name):
        for state in self.panel["states"]:
            if state["name"] == name or state["storage"] == name:
                return state
        names = ", ".join(state["name"] for state in self.panel["states"] if not state["hidden"])
        raise ErbError("no state named %s; the app's state: %s" % (name, names or "none"))


def format_value(kind, data, at, cap=None, values=None):
    if kind == "int":
        return str(unpack("i", data, at))
    if kind == "bool":
        return "true" if unpack("i", data, at) else "false"
    if kind == "float":
        return "%g" % unpack("f", data, at)
    if kind == "int64":
        return str(unpack("q", data, at))
    if kind == "double":
        return "%g" % unpack("d", data, at)
    if kind == "enum":
        index = unpack("i", data, at)
        return json.dumps(values[index]) if values and 0 <= index < len(values) else "<value %d>" % index
    if kind == "string":
        text = data[at : at + (cap or 0) + 1]
        end = text.find(b"\0")
        return json.dumps(text[: end if end >= 0 else len(text)].decode("utf-8", "replace"))
    return "<%s>" % kind


VALUE_BYTES = {"int": 4, "bool": 4, "float": 4, "enum": 4, "int64": 8, "double": 8}


def state_value(app, state):
    kind = state["type"]
    address = app.arena + state["offset"]
    if kind == "string":
        return format_value(kind, read(address, state["cap"] + 1), 0, state["cap"])
    if kind == "list":
        count = read_value("I", address)
        rows = []
        shown = min(count, MOST_ROWS_SHOWN)
        data = read(address + LIST_HEADER_BYTES, shown * state["rowBytes"]) if shown else b""
        for row in range(shown):
            fields = []
            for field in state["fields"]:
                if field["name"].endswith("?"):
                    continue
                value = format_value(field["type"], data, row * state["rowBytes"] + field["offset"], field.get("cap"))
                fields.append(("%s: %s" % (field["name"], value)) if field["name"] else value)
            rows.append("{" + ", ".join(fields) + "}" if len(state["fields"]) > 1 or state["fields"][0]["name"] else fields[0])
        more = ", …" if count > shown else ""
        return "[%s%s]  (%d of %d rows)" % (", ".join(rows), more, count, state["cap"])
    return format_value(kind, read(address, VALUE_BYTES.get(kind, 4)), 0, values=state.get("values"))


def state_label(state):
    if state.get("component"):
        return "%s.%s" % (state["component"]["name"], state["name"])
    return state["name"]


# ----------------------------------------------------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------------------------------------------------


class ErbCommand(gdb.Command):
    """Commands for a Flow B app on a halted board.

erb file PATH        Load the app's debug file (dist/app.erdbg).
erb where            The app's source line and call chain.
erb error            The last fault or refused load.
erb state [NAME]     Every state, or one.
erb set NAME VALUE   Set a state; the app runs its update at its next tick.
erb watch NAME       Stop when a state changes, on the line that wrote it.
erb pools            What the app and the engine use of their pools.
er nodes             The node tree, with each node's rect and source line."""

    def __init__(self):
        super().__init__("erb", gdb.COMMAND_DATA, gdb.COMPLETE_NONE, True)


class ErbFile(gdb.Command):
    """Load the app's debug file: erb file dist/app.erdbg"""

    def __init__(self):
        super().__init__("erb file", gdb.COMMAND_FILES, gdb.COMPLETE_FILENAME)

    def invoke(self, argument, from_tty):
        if not argument.strip():
            raise ErbError("name the debug file: erb file dist/app.erdbg")
        load_debug_file(argument.strip())
        print(
            "Loaded %s: build %s, %d layout%s"
            % (debug_path, build_hex(debug_file["buildId"]), len(debug_file["panels"]), "" if len(debug_file["panels"]) == 1 else "s")
        )


class ErbWhere(gdb.Command):
    """The app's current source line, and the lines that called it."""

    def __init__(self):
        super().__init__("erb where", gdb.COMMAND_STACK)

    def invoke(self, argument, from_tty):
        app = App()
        frames = app.frames()
        if not app.runtime.flag("in_vm"):
            function, offset = frames[0]
            last = app.place(function, offset) if function != NO_FUNCTION else "nothing yet"
            print("The app isn't running its code. Last ran: %s" % last)
            return
        print(app.place(*frames[0]))
        for function, offset in frames[1:]:
            print("  called from %s" % app.place(function, offset))


class ErbErrorCommand(gdb.Command):
    """The last fault or refused load: the reason, the amounts, and the source line."""

    def __init__(self):
        super().__init__("erb error", gdb.COMMAND_STATUS)

    def invoke(self, argument, from_tty):
        runtime = Runtime()
        fields = runtime.error_fields
        data = read(runtime.error_address, fields["build_id_at"] + 4)
        status = unpack("I" if fields["status_bytes"] == 4 else "B" if fields["status_bytes"] == 1 else "H", data, fields["status_at"])
        if status == 0:
            print("No error: the app is %s." % ("running" if runtime.flag("loaded") else "not loaded"))
            return
        pointer = "I" if pointer_bytes() == 4 else "Q"
        name = read_string(unpack(pointer, data, fields["name_at"]))
        detail = read_string(unpack(pointer, data, fields["detail_at"]))
        needed = unpack("I", data, fields["needed_at"])
        available = unpack("I", data, fields["available_at"])
        resource = unpack("H", data, fields["resource_at"])
        function = unpack("H", data, fields["function_at"])
        offset = unpack("I", data, fields["offset_at"])
        build_id = unpack("I", data, fields["build_id_at"])

        reason = runtime.reason(status) + (": %s" % name if name else "")
        print(reason)
        if status == ERROR_TOO_BIG:
            print("  %s: needs %d, the firmware reserves %d" % (RESOURCE_NAMES.get(resource, "resource %d" % resource), needed, available))
        elif status == ERROR_ENGINE_ABI:
            print("  built for engine ABI %d" % needed)
        elif status == ERROR_NO_PANEL:
            print("  this screen is %d x %d" % (needed, available))
        elif status == ERROR_STACK:
            print("  needs %d cells, the arena has %d" % (needed, available))
        if detail:
            print("  check: %s" % detail)
        if function != NO_FUNCTION:
            where = "function %d at %d" % (function, offset)
            if debug_file is not None and build_id == debug_file["buildId"]:
                app = App(need_running=False)
                where = app.place(function, offset)
            elif debug_file is not None:
                where += " (build %s, but the debug file is build %s)" % (build_hex(build_id), build_hex(debug_file["buildId"]))
            print("  at %s" % where)
        if build_id:
            print("  build %s" % build_hex(build_id))
        print("The app %s." % ("stopped" if runtime.flag("faulted") else "is running" if runtime.flag("loaded") else "was not loaded"))


class ErbState(gdb.Command):
    """Every state by name, or one: erb state [NAME]. Add `all` to show the state the compiler made too."""

    def __init__(self):
        super().__init__("erb state", gdb.COMMAND_DATA)

    def invoke(self, argument, from_tty):
        app = App()
        words = argument.split()
        if words and words[0] != "all":
            state = app.state(words[0])
            print("%s = %s" % (state_label(state), state_value(app, state)))
            return
        shown = [state for state in app.panel["states"] if "all" in words or not state["hidden"]]
        if not shown:
            print("The app has no state.")
        for state in shown:
            print("%s = %s" % (state_label(state), state_value(app, state)))


def encode_value(state, text):
    kind = state["type"]
    text = text.strip()
    try:
        if kind == "int":
            return struct.pack(endian() + "i", int(text, 0))
        if kind == "int64":
            return struct.pack(endian() + "q", int(text, 0))
        if kind == "float":
            return struct.pack(endian() + "f", float(text))
        if kind == "bool":
            if text not in ("true", "false", "1", "0"):
                raise ValueError
            return struct.pack(endian() + "i", 1 if text in ("true", "1") else 0)
    except ValueError:
        raise ErbError("%s holds a %s, not %s" % (state["name"], kind, text))
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        text = text[1:-1]
    if kind == "enum":
        values = state.get("values") or []
        if text not in values:
            raise ErbError("%s is one of %s" % (state["name"], ", ".join(json.dumps(value) for value in values)))
        return struct.pack(endian() + "i", values.index(text))
    if kind == "string":
        encoded = text.encode("utf-8")
        if len(encoded) > state["cap"]:
            raise ErbError("%s holds at most %d bytes" % (state["name"], state["cap"]))
        return encoded + b"\0"
    raise ErbError("a %s state can't be set from the debugger" % kind)


class ErbSet(gdb.Command):
    """Set a state: erb set NAME VALUE. The app runs its update, as a setter would, at its next tick."""

    def __init__(self):
        super().__init__("erb set", gdb.COMMAND_DATA)

    def invoke(self, argument, from_tty):
        parts = argument.strip().split(None, 1)
        if len(parts) != 2:
            raise ErbError("name the state and its value: erb set count 3")
        app = App()
        state = app.state(parts[0])
        write(app.arena + state["offset"], encode_value(state, parts[1]))
        if "dirtyBit" in state:
            bit = state["dirtyBit"]
            word = app.arena + app.panel["regions"]["dirty"]["offset"] + 4 * (bit // 32)
            write(word, struct.pack(endian() + "I", read_value("I", word) | (1 << (bit % 32))))
            app.runtime.request_update()
        print("%s = %s" % (state_label(state), state_value(app, state)))


class StateWatchpoint(gdb.Breakpoint):
    """A hardware watchpoint on a state, which says what changed and where."""

    def __init__(self, app, state):
        self.state = state
        self.label = state_label(state)
        # Every write to a state writes its first word, and a word is what every probe can watch at a 4-byte
        # boundary: a 64-bit slot is only 4-aligned in the arena, a string's buffer is longer, and a list's first
        # word is its row count.
        address = app.arena + state["offset"]
        super().__init__("*(unsigned int *) 0x%x" % address, gdb.BP_WATCHPOINT, gdb.WP_WRITE, internal=False)
        self.previous = state_value(app, state)

    def stop(self):
        try:
            app = App()
            value = state_value(app, self.state)
            if app.runtime.flag("in_vm"):
                where = app.place(*app.frames()[0])
            else:
                where = "the runtime (a host input, a load, or a reset)"
            print("%s: %s → %s, written at %s" % (self.label, self.previous, value, where))
            self.previous = value
        except gdb.GdbError as error:
            print("erb watch: %s" % error)
        return True


class ErbWatch(gdb.Command):
    """Stop when a state changes, on the line that wrote it: erb watch NAME. Uses a hardware watchpoint."""

    def __init__(self):
        super().__init__("erb watch", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        if not argument.strip():
            raise ErbError("name the state: erb watch count")
        app = App()
        state = app.state(argument.strip())
        watchpoint = StateWatchpoint(app, state)
        print("Watching %s (watchpoint %d); it is %s" % (watchpoint.label, watchpoint.number, watchpoint.previous))


class ErbPools(gdb.Command):
    """What the app and the engine use of their pools."""

    def __init__(self):
        super().__init__("erb pools", gdb.COMMAND_STATUS)

    def invoke(self, argument, from_tty):
        app = App()
        engine = Engine(app.runtime.engine_address)
        print("Engine")
        for name, pool in zip(POOL_NAMES, engine.pools):
            used = sum(1 for _, _, in_use in engine.records(pool) if in_use)
            print("  %-17s %5d of %d" % (name, used, pool["count"]))
        print("App (arena %d B)" % app.panel["arenaBytes"])
        regions = app.panel["regions"]
        for name, size, at in (("timers", TIMER_BYTES, TIMER_ACTIVE_AT), ("animations", ANIMATION_ENTRY_BYTES, ANIMATION_STATE_AT)):
            region = regions.get(name)
            if region and region["count"]:
                data = read(app.arena + region["offset"], region["count"] * size)
                used = sum(1 for index in range(region["count"]) if data[index * size + at])
                print("  %-17s %5d of %d" % (name, used, region["count"]))
        for state in app.panel["states"]:
            if state["type"] == "list":
                rows = read_value("I", app.arena + state["offset"])
                print("  %-17s %5d of %d rows" % (state_label(state), rows, state["cap"]))


class ErCommand(gdb.Command):
    """Engine commands. er nodes: the node tree."""

    def __init__(self):
        super().__init__("er", gdb.COMMAND_DATA, gdb.COMPLETE_NONE, True)


def template_children(template):
    """Each template node's children, by index, from the pre-order list and its child counts."""
    children = [[] for _ in template["nodes"]]

    def walk(index):
        following = index + 1
        for _ in range(template["nodes"][index]["children"]):
            children[index].append(following)
            following = walk(following)
        return following

    if template["nodes"]:
        walk(0)
    return children


class ErNodes(gdb.Command):
    """The node tree: each node's type, its rect on the screen, and the source line of its element."""

    def __init__(self):
        super().__init__("er nodes", gdb.COMMAND_DATA)

    def invoke(self, argument, from_tty):
        app = App()
        engine = Engine(app.runtime.engine_address)
        fields = engine.node
        pool = engine.pools[0]
        nodes = {}
        for tag, record, in_use in engine.records(pool):
            if in_use:
                nodes[tag] = record
        type_format = {1: "B", 2: "H", 4: "I"}[fields["type_bytes"]]

        def u16(record, name):
            return unpack("H", record, fields[name])

        # Each node code reaches, by its handle.
        templates = app.panel["templates"]
        places = {}
        for template_index, template in enumerate(templates):
            for node_index, node in enumerate(template["nodes"]):
                handle = node.get("handle")
                if not handle:
                    continue
                data = read(app.arena + handle["offset"], handle["count"] * HANDLE_BYTES)
                for row in range(handle["count"]):
                    tag = unpack("i", data, row * HANDLE_BYTES) - 1
                    serial = unpack("I", data, row * HANDLE_BYTES + 4)
                    if tag in nodes and unpack("I", nodes[tag], fields["serial_at"]) == serial:
                        places[tag] = (template_index, node_index)
        children_of = [template_children(template) for template in templates]
        node_types = debug_file.get("nodeTypes", [])

        def type_name(record):
            number = unpack(type_format, record, fields["type_at"])
            name = node_types[number] if number < len(node_types) and node_types[number] else "type %d" % number
            return name.replace("ER_NODE_", "").lower()

        def engine_children(tag):
            child = u16(nodes[tag], "first_child_at")
            while child != NO_TAG and child in nodes:
                yield child
                child = u16(nodes[child], "next_sibling_at")

        # The rest are matched to their template by where they sit under a node whose place is known.
        def match(tag):
            place = places.get(tag)
            if place is None:
                return
            template_index, node_index = place
            template = templates[template_index]
            unclaimed = [index for index in children_of[template_index][node_index]]
            for child in engine_children(tag):
                known = places.get(child)
                if known is not None:
                    if known[0] == template_index and known[1] in unclaimed:
                        unclaimed = unclaimed[unclaimed.index(known[1]) + 1 :]
                elif unclaimed and template["nodes"][unclaimed[0]]["type"].replace("ER_NODE_", "").lower() == type_name(nodes[child]):
                    places[child] = (template_index, unclaimed.pop(0))
                match(child)

        # Before the app's first commit the engine has no root yet, so each node without a parent is shown.
        root = read_value("H", engine.root_tag_address)
        tops = [root] if root in nodes else [tag for tag, record in nodes.items() if u16(record, "parent_at") not in nodes]
        if not tops:
            print("No nodes: the app has built nothing.")
            return
        if tops[0] not in places and templates and templates[0]["nodes"]:
            places[tops[0]] = (0, 0)
        for top in tops:
            match(top)

        # Layout places a node on the screen; a ScrollView above it moves it by its offset.
        def show(tag, depth, scrolled_x, scrolled_y):
            record = nodes[tag]
            x, y, width, height = struct.unpack_from(endian() + "hhhh", record, fields["rect_at"])
            left, top = x - scrolled_x, y - scrolled_y
            place = places.get(tag)
            source = ""
            if place:
                node = templates[place[0]]["nodes"][place[1]]
                if node["line"] is not None:
                    source = "  %s:%d" % (debug_file["file"], node["line"])
            print("%s%s #%d  %d,%d %dx%d%s" % ("  " * depth, type_name(record), tag, left, top, width, height, source))
            scroll_x = unpack("f", record, fields["scroll_x_at"])
            scroll_y = unpack("f", record, fields["scroll_y_at"])
            for child in engine_children(tag):
                show(child, depth + 1, scrolled_x + int(scroll_x), scrolled_y + int(scroll_y))

        for top in tops:
            show(top, 0, 0, 0)


# ----------------------------------------------------------------------------------------------------------------------
# The app's line in a backtrace
# ----------------------------------------------------------------------------------------------------------------------

try:
    from gdb.FrameDecorator import FrameDecorator

    class VmFrame(FrameDecorator):
        """The VM's frame, with the app's source line beside it: erb_vm_run [App.jsx:41  onPress]."""

        def __init__(self, base, place):
            super().__init__(base)
            self.place = place

        def function(self):
            name = super().function()
            if isinstance(name, int):
                name = str(name)
            return "%s [%s]" % (name, self.place)

    class VmFrameFilter:
        def __init__(self):
            self.name = "embedded-react"
            self.priority = 100
            self.enabled = True
            gdb.frame_filters[self.name] = self

        def filter(self, frames):
            place = None
            for frame in frames:
                if frame.inferior_frame().name() == "erb_vm_run":
                    if place is None:
                        place = self.app_place()
                    if place:
                        frame = VmFrame(frame, place)
                yield frame

        @staticmethod
        def app_place():
            if debug_file is None:
                return None
            try:
                app = App()
                if not app.runtime.flag("in_vm"):
                    return None
                return app.place(*app.frames()[0])
            except gdb.GdbError:
                return None

    VmFrameFilter()
except ImportError:
    pass


ErbCommand()
ErbFile()
ErbWhere()
ErbErrorCommand()
ErbState()
ErbSet()
ErbWatch()
ErbPools()
ErCommand()
ErNodes()

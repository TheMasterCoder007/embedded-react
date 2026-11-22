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

The commands that read run nothing on the board. They find the runtime and the engine through the descriptors the
firmware exports (erb_debug and er_debug), so they work on any board and need no debug info beyond the ELF's
symbols. Breakpoints and stepping (erb break, step, next, finish) need firmware built with ERB_DEBUG_HOOK=1, whose VM
stops itself where the debugger asks. GDB must be built with Python 3.8 or later.
"""

import json
import os
import re
import struct

import gdb

DEBUG_FILE_FORMAT = 1
ERB_DEBUG_MAGIC = 0x44425245
ERB_DEBUG_VERSION = 2
ER_DEBUG_MAGIC = 0x45445245
ER_DEBUG_VERSION = 2

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
        data = bytes(gdb.selected_inferior().read_memory(address, length))
        # On the ESP32-S3's built-in JTAG, the first read of flash the cache doesn't hold gives its own address for
        # the first word. A second read is right.
        if length >= 4 and data[:4] == struct.pack(endian() + "I", address & 0xFFFFFFFF):
            data = bytes(gdb.selected_inferior().read_memory(address, length))
        return data
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
            ("halted", "ptr", 1),
            ("control", "ptr", 1),
            ("stop_capacity", "u16", 1),
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
            ("halted", "ptr", 1),
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
        self.control = self.pointer("control")
        self.stop_capacity = unpack("H", data, field("stop_capacity"))

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
        self.halted = unpack(pointer, data, field("halted"))

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
    missing = shape_fault(loaded)
    if missing:
        raise ErbError(
            "%s has no %s: it was written by another embedded-react, so build the app again with the one this script "
            "came from" % (path, missing)
        )
    debug_file = loaded
    debug_path = path


def shape_fault(loaded):
    """The first field a debug file lacks that the commands read, or None."""
    is_number = lambda value: isinstance(value, (int, float)) and not isinstance(value, bool)
    for key, check in (("buildId", is_number), ("file", lambda value: isinstance(value, str)), ("nodeTypes", lambda value: isinstance(value, list)), ("panels", lambda value: isinstance(value, list))):
        if not check(loaded.get(key)):
            return key
    for index, panel in enumerate(loaded["panels"]):
        where = "panel %d's " % index
        if not isinstance(panel, dict):
            return where + "fields"
        for key in ("width", "height", "arenaBytes"):
            if not is_number(panel.get(key)):
                return where + key
        if not isinstance(panel.get("regions"), dict):
            return where + "regions"
        for key in ("functions", "states", "templates"):
            if not isinstance(panel.get(key), list):
                return where + key
        for function in panel["functions"]:
            if not isinstance(function, dict) or not isinstance(function.get("name"), str) or not isinstance(function.get("lines"), list):
                return where + "function names and lines"
        for state in panel["states"]:
            if not isinstance(state, dict) or not all(key in state for key in ("name", "storage", "type", "slot", "offset", "hidden")):
                return where + "state fields"
        for template in panel["templates"]:
            nodes = template.get("nodes") if isinstance(template, dict) else None
            if not isinstance(nodes, list) or not all(isinstance(node, dict) and "type" in node and "children" in node and "line" in node for node in nodes):
                return where + "template nodes"
    return None


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

    def label(self, state):
        return state_labels(self.panel)[state["storage"]]

    def state(self, name):
        """A state by the label the commands show, its storage name, or its own name when only one has it."""
        states = self.panel["states"]
        labels = state_labels(self.panel)
        found = [state for state in states if labels[state["storage"]] == name or state["storage"] == name]
        if not found:
            found = [state for state in states if state["name"] == name]
        if len(found) == 1:
            return found[0]
        if found:
            raise ErbError("%s names %d states: %s" % (name, len(found), ", ".join(labels[state["storage"]] for state in found)))
        names = ", ".join(labels[state["storage"]] for state in states if not state["hidden"])
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


def state_labels(panel):
    """Each state's label, by storage name: its name, or `Component.name` for an inlined component's. When one component
    is inlined more than once, the label adds the instance's line (`Counter:16.taps`), and a number when instances share
    a line (`Counter:16[2].taps`). A label that is still shared falls back to the storage name."""
    labels = panel.get("_labels")
    if labels is not None:
        return labels

    def instance(state):
        match = re.match(r"c(\d+)_", state["storage"])
        return int(match.group(1)) if match else None

    states = panel["states"]
    # Each component's instances, and each line's, in source order.
    by_component = {}
    by_line = {}
    for state in states:
        component = state.get("component")
        if component and instance(state) is not None:
            by_component.setdefault(component["name"], set()).add(instance(state))
            by_line.setdefault((component["name"], component["line"]), set()).add(instance(state))

    def label(state):
        component = state.get("component")
        if not component:
            return state["name"]
        if len(by_component.get(component["name"], ())) <= 1:
            return "%s.%s" % (component["name"], state["name"])
        on_line = sorted(by_line.get((component["name"], component["line"]), ()))
        number = "[%d]" % (on_line.index(instance(state)) + 1) if len(on_line) > 1 and instance(state) in on_line else ""
        return "%s:%d%s.%s" % (component["name"], component["line"], number, state["name"])

    labels = {state["storage"]: label(state) for state in states}
    shared = {}
    for value in labels.values():
        shared[value] = shared.get(value, 0) + 1
    labels = {key: key if shared[value] > 1 else value for key, value in labels.items()}
    panel["_labels"] = labels
    return labels


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
erb break [LINE]     Stop at a source line (App.jsx:41, or 41); with no line, list the breakpoints.
erb delete [N...]    Delete breakpoints, or all of them.
erb step             Run to the next statement, into a function the app calls.
erb next             Run to the next statement, over a function the app calls.
erb finish           Run until the function returns to its caller.
erb pools            What the app and the engine use of their pools.
er nodes             The node tree, with each node's rect and source line.

Breakpoints and stepping need firmware built with ERB_DEBUG_HOOK=1."""

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
        note_missing_hook()
        # Attaching halts the board without a stop event.
        if board_halted():
            try:
                mark_halted()
            except (gdb.GdbError, gdb.error, gdb.MemoryError):
                pass


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
            print("%s = %s" % (app.label(state), state_value(app, state)))
            return
        shown = [state for state in app.panel["states"] if "all" in words or not state["hidden"]]
        if not shown:
            print("The app has no state.")
        for state in shown:
            print("%s = %s" % (app.label(state), state_value(app, state)))


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
        value = state_value(app, state)
        for watchpoint in watchpoints:
            if watchpoint.is_valid() and watchpoint.state["storage"] == state["storage"]:
                watchpoint.previous = value
                watchpoint.copying = None
        print("%s = %s" % (app.label(state), value))


# The states being watched, so a value the debugger writes isn't taken for one the app wrote.
watchpoints = []


class StateWatchpoint(gdb.Breakpoint):
    """A hardware watchpoint on a state's first word, which says what changed and where.

    A word is what every probe can watch at a 4-byte boundary (a 64-bit slot is only 4-aligned in the arena), and
    every write to a state writes it: a string is copied in whole from its start. GDB stops a write watchpoint only
    when the watched word changes, though, and a string can change past its first 4 bytes, so a string is watched
    for any access and the whole value compared here. Its first change traps partway through the copy, so the
    watchpoint stops at the first access after the copy (a read, which leaves it as it is) and names the line that
    wrote it. A list's first word is its row count: its rows are more than a probe can watch, so a change to a
    row's field doesn't stop."""

    def __init__(self, app, state):
        self.state = state
        self.label = app.label(state)
        self.whole = state["type"] == "string"
        address = app.arena + state["offset"]
        kind = gdb.WP_ACCESS if self.whole else gdb.WP_WRITE
        super().__init__("*(unsigned int *) 0x%x" % address, gdb.BP_WATCHPOINT, kind, internal=False)
        self.previous = state_value(app, state)
        watchpoints.append(self)
        # A string's copy under way: the instruction writing it, what it holds so far, and the line that wrote it.
        self.copying = None

    def stop(self):
        try:
            app = App()
            value = state_value(app, self.state)
            if self.whole:
                pc = gdb.selected_frame().pc()
                if self.copying:
                    # The copy goes on while the same instruction writes it, or while it changes.
                    if pc == self.copying["pc"] or value != self.copying["value"]:
                        self.copying.update(pc=pc, value=value)
                        return False
                    where = self.copying["where"]
                    self.copying = None
                elif value == self.previous:
                    return False
                else:
                    self.copying = {"pc": pc, "value": value, "where": self.writer(app)}
                    return False
            else:
                where = self.writer(app)
            print("%s: %s → %s, written at %s" % (self.label, self.previous, value, where))
            self.previous = value
        except gdb.GdbError as error:
            print("erb watch: %s" % error)
        return True

    @staticmethod
    def writer(app):
        if app.runtime.flag("in_vm"):
            return app.place(*app.frames()[0])
        return "the runtime (a host input, a load, or a reset)"


class ErbWatch(gdb.Command):
    """Stop when a state changes, on the line that wrote it: erb watch NAME. Uses a hardware watchpoint.
A list stops when its rows are added or removed, not when a row's field changes."""

    def __init__(self):
        super().__init__("erb watch", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        if not argument.strip():
            raise ErbError("name the state: erb watch count")
        app = App()
        state = app.state(argument.strip())
        watchpoint = StateWatchpoint(app, state)
        print("Watching %s (watchpoint %d); it is %s" % (watchpoint.label, watchpoint.number, watchpoint.previous))
        if state["type"] == "list":
            print("A list stops when rows are added or removed; a change to a row's field doesn't stop.")


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
                print("  %-17s %5d of %d rows" % (app.label(state), rows, state["cap"]))


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
# Breakpoints and stepping
# ----------------------------------------------------------------------------------------------------------------------

STEP_NONE = 0
STEP_INTO = 1
STEP_OVER = 2
STEP_OUT = 3
STEP_ANY = 4
AT_STOP = 0
AT_RETURN = 1
AT_LOAD = 2
END_OF_FUNCTION = 0xFFFFFFFF

NO_HOOK = (
    "the firmware has no breakpoint hook: build it with ERB_DEBUG_HOOK=1 for erb break, step, next and finish. "
    "The other commands work without it."
)


def control_layout():
    return Layout(
        [
            ("armed", "u8", 1),
            ("step", "u8", 1),
            ("event", "u8", 1),
            ("reserved", "u8", 1),
            ("step_function", "u16", 1),
            ("stop_count", "u16", 1),
            ("step_from", "u32", 1),
            ("step_to", "u32", 1),
            ("step_base", "u32", 1),
            ("stops", "u32", 0),
        ]
    )


class Hook:
    """The VM's breakpoint hook (ErbDebugControl): the debugger writes where it stops, the VM reads it."""

    def __init__(self, runtime):
        if not runtime.control:
            raise ErbError(NO_HOOK)
        self.address = runtime.control
        self.capacity = runtime.stop_capacity
        self.offsets = control_layout().offsets

    def read(self, name, fmt):
        return read_value(fmt, self.address + self.offsets[name])

    def write(self, name, fmt, value):
        write(self.address + self.offsets[name], struct.pack(endian() + fmt, value))

    def event(self):
        return self.read("event", "B")

    def step(self):
        return self.read("step", "B")

    def set_step(self, kind, function=0, start=0, end=0, base=0):
        self.write("step_function", "H", function)
        self.write("step_from", "I", start)
        self.write("step_to", "I", end)
        self.write("step_base", "I", base)
        self.write("step", "B", kind)
        self.arm()

    def set_stops(self, places):
        if len(places) > self.capacity:
            raise ErbError(
                "the breakpoints need %d places, but the firmware's hook holds %d: delete some, or build the firmware "
                "with a larger ERB_DEBUG_STOPS" % (len(places), self.capacity)
            )
        data = b"".join(struct.pack(endian() + "HHI", function, 0, offset) for function, offset in places)
        if data:
            write(self.address + self.offsets["stops"], data)
        self.write("stop_count", "H", len(places))
        self.arm()

    def arm(self):
        armed = self.read("stop_count", "H") > 0 or self.step() != STEP_NONE
        self.write("armed", "B", 1 if armed else 0)


def statements(panel, function):
    """A function's statements, in order: the offset each starts at and its line. Entries that carry on the line
    before them are part of its statement."""
    functions = panel["functions"]
    if function >= len(functions):
        return []
    found = []
    for entry in functions[function]["lines"]:
        if not found or found[-1][1] != entry["line"]:
            found.append((entry["offset"], entry["line"]))
    return found


def statement_range(panel, function, offset):
    """The instructions of the statement that holds an offset: [start, end). Before the first, from 0."""
    start, end = 0, END_OF_FUNCTION
    for at, _ in statements(panel, function):
        if at <= offset:
            start = at
        else:
            end = at
            break
    return start, end


def line_places(panel, line):
    """Each function and offset a source line's statements start at."""
    return [
        (function, at)
        for function in range(len(panel["functions"]))
        for at, statement in statements(panel, function)
        if statement == line
    ]


def parse_line(text):
    """A source line from `App.jsx:41` or `41`."""
    name, _, number = text.strip().rpartition(":")
    if name and os.path.basename(name) != debug_file["file"]:
        raise ErbError("the app's source is %s, not %s" % (debug_file["file"], name))
    try:
        line = int(number)
    except ValueError:
        raise ErbError("name a source line: erb break %s:41" % debug_file["file"])
    if not any(line_places(panel, line) for panel in debug_file["panels"]):
        lines = sorted(
            {entry["line"] for panel in debug_file["panels"] for function in panel["functions"] for entry in function["lines"]}
        )
        before = [candidate for candidate in lines if candidate < line]
        after = [candidate for candidate in lines if candidate > line]
        nearest = " and ".join(str(candidate) for candidate in (before[-1:] + after[:1]))
        raise ErbError(
            "%s:%d has no code%s" % (debug_file["file"], line, "; the nearest lines with code are " + nearest if nearest else "")
        )
    return line


# The app's breakpoints, each {"number", "line"}; the hook holds their places for the layout the board runs.
breakpoints = []
breakpoint_numbers = [0]
# Set while the app is stopped where a step reached the return to the runtime.
returned = [False]
# The hook's breakpoint, and whether the board's last stop was in it.
hook_state = {"breakpoint": None, "stopped": False, "noted": False}
source_lines = {}


def source_text(line):
    """A source line's text, when the source file can be found beside the debug file or here."""
    path = source_lines.get("path")
    if path is None:
        found = ""
        if debug_path:
            folder = os.path.dirname(os.path.abspath(debug_path))
            for candidate in (os.getcwd(), folder, os.path.dirname(folder)):
                for sub in ("", "src"):
                    full = os.path.join(candidate, sub, debug_file["file"])
                    if not found and os.path.isfile(full):
                        found = full
        source_lines["path"] = found
        try:
            with open(found, encoding="utf-8") as handle:
                source_lines["lines"] = handle.read().splitlines()
        except OSError:
            source_lines["lines"] = []
    lines = source_lines["lines"]
    return lines[line - 1].rstrip() if 0 < line <= len(lines) else None


def stop_line(app, function, offset):
    """The place a stop shows, and its source text under it when the source is at hand."""
    text = app.place(function, offset)
    _, line = statement_line(app.panel, function, offset)
    source = source_text(line) if line else None
    return text + ("\n%d\t%s" % (line, source) if source is not None else "")


def statement_line(panel, function, offset):
    """The start and line of the statement that holds an offset, or None for both before the first."""
    line = None
    start = None
    for at, statement in statements(panel, function):
        if at > offset:
            break
        start, line = at, statement
    return start, line


def write_stops():
    """Writes the breakpoints' places for the layout the board runs into the hook. Raises when it can't."""
    app = App()
    hook = Hook(app.runtime)
    places = sorted({place for entry in breakpoints for place in line_places(app.panel, entry["line"])})
    hook.set_stops(places)
    return app


def hook_breakpoint():
    """The breakpoint on erb_debug_break() the hook stops at, made the first time it is needed."""
    if not hook_state["breakpoint"] or not hook_state["breakpoint"].is_valid():
        hook_state["breakpoint"] = HookBreakpoint()
    return hook_state["breakpoint"]


class HookBreakpoint(gdb.Breakpoint):
    """Where the hook stops: at a breakpoint's place, where a step ends, and after each load, when the breakpoints
    are written for the package that loaded. A step that comes back from a call partway through a statement runs on
    to the next one here, without a stop."""

    def __init__(self):
        super().__init__("erb_debug_break", gdb.BP_BREAKPOINT, internal=True)
        self.silent = True

    def stop(self):
        try:
            return self.decide()
        except gdb.GdbError as error:
            self.report("erb: %s" % error)
            return True

    @staticmethod
    def report(message):
        hook_state["stopped"] = True
        print(message)

    def decide(self):
        runtime = Runtime()
        hook = Hook(runtime)
        event = hook.event()
        if event == AT_LOAD:
            hook.set_step(STEP_NONE)
            if breakpoints:
                try:
                    write_stops()
                except gdb.GdbError as error:
                    print("erb: the app loaded, but its breakpoints aren't set: %s" % error)
            return False
        app = App()
        function, offset = app.frames()[0]
        has_lines = bool(statements(app.panel, function))
        if event == AT_RETURN:
            if not has_lines:
                # Code the compiler made has no lines: on to the app's next statement.
                hook.set_step(STEP_ANY)
                return False
            hook.set_step(STEP_NONE)
            returned[0] = True
            self.report("%s returns to the runtime" % app.panel["functions"][function]["name"])
            return True
        step = hook.step()
        start, _ = statement_line(app.panel, function, offset)
        if step in (STEP_INTO, STEP_OVER, STEP_ANY) and (not has_lines or start != offset):
            # Partway through a statement, back from a call, or in code with no lines: on to the next statement.
            begin, end = statement_range(app.panel, function, offset) if has_lines else (0, END_OF_FUNCTION)
            hook.set_step(STEP_OVER if step == STEP_OVER else STEP_INTO, function, begin, end, runtime.u32("frame_base"))
            return False
        hook.set_step(STEP_NONE)
        numbers = [
            str(entry["number"]) for entry in breakpoints if (function, offset) in line_places(app.panel, entry["line"])
        ]
        prefix = "erb breakpoint %s, " % ", ".join(numbers) if numbers else ""
        self.report(prefix + stop_line(app, function, offset))
        return True


def run_step(kind):
    """Starts a step and resumes the board, then shows where it stopped."""
    if not has_hook():
        raise ErbError(NO_HOOK)
    app = App()
    hook = Hook(app.runtime)
    hook_breakpoint()
    function, offset = app.frames()[0]
    base = app.runtime.u32("frame_base")
    if returned[0] or not app.runtime.flag("in_vm"):
        if kind == STEP_OUT:
            raise ErbError("the app isn't in a function it can return from: `continue` runs it on")
        # Stops at the next statement the app runs, whatever runs it.
        hook.set_step(STEP_ANY)
    elif kind == STEP_OUT:
        hook.set_step(STEP_OUT, function, 0, 0, base)
    else:
        start, end = statement_range(app.panel, function, offset)
        hook.set_step(kind, function, start, end, base)
    gdb.execute("continue")


def has_symbol(name):
    try:
        symbol_address(name)
        return True
    except ErbError:
        return False


def has_hook():
    """Whether the firmware has the hook. Read from its symbols, as a pointer in a descriptor of a program that isn't
    running yet may not be relocated."""
    return has_symbol("erb_debug_break")


def note_missing_hook():
    """Says once per ELF when its Flow B firmware has no hook."""
    if not hook_state["noted"] and has_symbol("erb_debug") and not has_hook():
        hook_state["noted"] = True
        print("Note: %s" % NO_HOOK)


class ErbBreak(gdb.Command):
    """Stop at a source line: erb break App.jsx:41, or erb break 41. With no line, list the breakpoints.
Needs firmware built with ERB_DEBUG_HOOK=1. A breakpoint set before the app loads is written when it does."""

    def __init__(self):
        super().__init__("erb break", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        if debug_file is None:
            raise ErbError("no debug file: load the app's with `erb file dist/app.erdbg`")
        if not argument.strip():
            if not breakpoints:
                print("No erb breakpoints.")
            for entry in breakpoints:
                print("%d  %s:%d" % (entry["number"], debug_file["file"], entry["line"]))
            return
        if not has_hook():
            raise ErbError(NO_HOOK)
        line = parse_line(argument)
        hook_breakpoint()
        breakpoint_numbers[0] += 1
        entry = {"number": breakpoint_numbers[0], "line": line}
        breakpoints.append(entry)
        try:
            write_stops()
            pending = ""
        except ErbError as error:
            if "ERB_DEBUG_STOPS" in str(error):
                breakpoints.remove(entry)
                raise
            pending = " (set when the app loads)"
        print("erb breakpoint %d at %s:%d%s" % (entry["number"], debug_file["file"], line, pending))


class ErbDelete(gdb.Command):
    """Delete breakpoints: erb delete 2 3, or erb delete for all of them."""

    def __init__(self):
        super().__init__("erb delete", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        try:
            numbers = [int(word) for word in argument.split()]
        except ValueError:
            raise ErbError("name breakpoints by number: erb delete 1 2")
        deleted = [entry for entry in breakpoints if not numbers or entry["number"] in numbers]
        missing = sorted(set(numbers) - {entry["number"] for entry in deleted})
        if missing:
            raise ErbError("no erb breakpoint %s" % ", ".join(str(number) for number in missing))
        for entry in deleted:
            breakpoints.remove(entry)
        try:
            write_stops()
        except ErbError:
            # Not running: the next load writes what is left.
            pass
        print("Deleted %d erb breakpoint%s." % (len(deleted), "" if len(deleted) == 1 else "s"))


class ErbStep(gdb.Command):
    """Run to the next statement, into a function the app calls. Past the end of the app's code it stops at the next
statement the app runs. Needs firmware built with ERB_DEBUG_HOOK=1."""

    def __init__(self):
        super().__init__("erb step", gdb.COMMAND_RUNNING)

    def invoke(self, argument, from_tty):
        run_step(STEP_INTO)


class ErbNext(gdb.Command):
    """Run to the next statement, over a function the app calls. Needs firmware built with ERB_DEBUG_HOOK=1."""

    def __init__(self):
        super().__init__("erb next", gdb.COMMAND_RUNNING)

    def invoke(self, argument, from_tty):
        run_step(STEP_OVER)


class ErbFinish(gdb.Command):
    """Run until the function returns to its caller. Needs firmware built with ERB_DEBUG_HOOK=1."""

    def __init__(self):
        super().__init__("erb finish", gdb.COMMAND_RUNNING)

    def invoke(self, argument, from_tty):
        run_step(STEP_OUT)


def mark_halted():
    """The app doesn't see the time the board is halted as elapsed. Returns the runtime."""
    engine = symbol_address("er_debug")
    offsets = engine_layout().offsets
    # An engine of another version may not have the flag where this one is.
    if read_value("I", engine) == ER_DEBUG_MAGIC and read_value("H", engine + offsets["version"]) == ER_DEBUG_VERSION:
        write(read_pointer(engine + offsets["halted"]), b"\x01")
    runtime = Runtime()
    write(runtime.pointer("halted"), b"\x01")
    return runtime


def board_halted():
    thread = gdb.selected_thread()
    return thread is not None and thread.is_valid() and thread.is_stopped()


def on_stop(event):
    """Every halt: the app doesn't see the time it took as elapsed, and a step another stop cut short ends."""
    try:
        runtime = mark_halted()
        if not hook_state["stopped"] and runtime.control:
            hook = Hook(runtime)
            if hook.step() != STEP_NONE:
                hook.set_step(STEP_NONE)
    except (gdb.GdbError, gdb.error, gdb.MemoryError):
        pass


def on_resume(event):
    hook_state["stopped"] = False
    returned[0] = False


def on_new_objfile(event):
    hook_state["noted"] = False


gdb.events.stop.connect(on_stop)
gdb.events.cont.connect(on_resume)
gdb.events.new_objfile.connect(on_new_objfile)


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
note_missing_hook()
ErbBreak()
ErbDelete()
ErbStep()
ErbNext()
ErbFinish()
ErbPools()
ErCommand()
ErNodes()

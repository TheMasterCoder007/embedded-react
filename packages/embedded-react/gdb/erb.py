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
symbols. Breakpoints and stepping (erb break, log, step, next, finish) and the stop at a fault need firmware built
with ERB_DEBUG_HOOK=1, whose VM stops itself where the debugger asks. GDB must be built with Python 3.8 or later.
"""

import base64
import json
import math
import os
import re
import struct
import sys

import gdb

DEBUG_FILE_FORMAT = 2
ERB_DEBUG_MAGIC = 0x44425245
ERB_DEBUG_VERSION = 3
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
    source_lines.clear()


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
            locals_ = function.get("locals")
            if not isinstance(locals_, list) or not all(isinstance(local, dict) and all(key in local for key in ("name", "from", "to")) for local in locals_):
                return where + "function locals"
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
        return [(function, offset) for function, offset, _ in self.frame_chain()]

    def frame_chain(self):
        """The app's call chain, innermost first: each function, the offset it is at, and its frame's base in cells."""
        runtime = self.runtime
        base = runtime.u32("frame_base")
        frames = [(runtime.u16("function"), runtime.u32("program_counter"), base)]
        stack = self.stack
        for _ in range(MOST_FRAMES):
            cells = read(stack + 4 * base, 12)
            caller = unpack("i", cells, 0)
            if caller < 0:
                break
            base = unpack("I", cells, 8)
            # The caller resumes after its CALL, which is one word.
            frames.append((caller, unpack("I", cells, 4) - 1, base))
        return frames

    @property
    def stack(self):
        return self.arena + self.runtime.u32("stack_offset")

    def locals_at(self, function, offset):
        """The parameters and locals that hold a value at an offset in a function, in the order the code names them."""
        functions = self.panel["functions"]
        if function >= len(functions):
            return []
        return [local for local in functions[function]["locals"] if local["from"] <= offset < local["to"]]

    def state_at(self, slot):
        for state in self.panel["states"]:
            if state["slot"] == slot:
                return state
        return None

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


class Undefined:
    """JavaScript's undefined. None is its null."""

    def __repr__(self):
        return "undefined"


UNDEFINED = Undefined()


class Unreadable:
    """A value the debugger can't read, and what it shows in its place."""

    def __init__(self, text):
        self.text = text


class Row:
    """A list's row: its bytes, and its number in the list when it was read from one."""

    def __init__(self, state, data, at=0, index=None):
        self.state = state
        self.data = data
        self.at = at
        self.index = index

    def has(self, field):
        """Whether the row holds a field: an optional one has a flag beside it."""
        for flag in self.state["fields"]:
            if flag["name"] == field["name"] + "?":
                return bool(read_typed("bool", self.data, self.at + flag["offset"]))
        return True

    def field(self, name):
        for field in self.state["fields"]:
            if field["name"] == name and not name.endswith("?"):
                if not self.has(field):
                    return UNDEFINED
                return read_typed(field["type"], self.data, self.at + field["offset"], field.get("cap"))
        names = ", ".join(field["name"] for field in self.state["fields"] if not field["name"].endswith("?"))
        raise ErbError("a row of %s has no %s; it has %s" % (self.state["name"], name, names))

    def text(self):
        fields = []
        for field in self.state["fields"]:
            if field["name"].endswith("?") or not self.has(field):
                continue
            value = show(read_typed(field["type"], self.data, self.at + field["offset"], field.get("cap")))
            fields.append("%s: %s" % (field["name"], value))
        return "{" + ", ".join(fields) + "}"


class Rows:
    """A list state: its rows are read as they are asked for."""

    def __init__(self, app, state):
        self.app = app
        self.state = state
        self.address = app.arena + state["offset"]
        self.count = read_value("I", self.address)

    def row(self, index):
        if not 0 <= index < self.count:
            return UNDEFINED
        size = self.state["rowBytes"]
        return read_row(self.state, read(self.address + LIST_HEADER_BYTES + index * size, size), 0, index)

    def text(self):
        shown = min(self.count, MOST_ROWS_SHOWN)
        size = self.state["rowBytes"]
        data = read(self.address + LIST_HEADER_BYTES, shown * size) if shown else b""
        rows = [show(read_row(self.state, data, row * size, row)) for row in range(shown)]
        more = ", …" if self.count > shown else ""
        return "[%s%s]  (%d of %d rows)" % (", ".join(rows), more, self.count, self.state["cap"])


def read_row(state, data, at, index=None):
    """A list's row from its bytes at an offset. A list of plain values has one unnamed field, which is the row."""
    fields = state["fields"]
    if len(fields) == 1 and not fields[0]["name"]:
        return read_typed(fields[0]["type"], data, at + fields[0]["offset"], fields[0].get("cap"))
    return Row(state, data, at, index)


def read_float(data, at):
    """A 32-bit float as the shortest number that reads back as it, so 0.1 isn't 0.10000000149."""
    value = unpack("f", data, at)
    if value != value or value in (float("inf"), float("-inf")):
        return value
    for digits in range(1, 10):
        short = float("%.*g" % (digits, value))
        if struct.pack("f", short) == struct.pack("f", value):
            return short
    return value


def read_typed(kind, data, at, cap=None, values=None):
    """A value from its bytes, as the expressions hold it."""
    if kind == "int":
        return unpack("i", data, at)
    if kind == "bool":
        return unpack("i", data, at) != 0
    if kind == "float":
        return read_float(data, at)
    if kind == "int64":
        return unpack("q", data, at)
    if kind == "double":
        return unpack("d", data, at)
    if kind == "enum":
        index = unpack("i", data, at)
        return values[index] if values and 0 <= index < len(values) else Unreadable("<value %d>" % index)
    if kind == "string":
        text = data[at : at + (cap or 0) + 1]
        end = text.find(b"\0")
        return text[: end if end >= 0 else len(text)].decode("utf-8", "replace")
    return Unreadable("<%s>" % kind)


def number_text(value):
    """A number as JavaScript prints it."""
    if isinstance(value, int):
        return str(value)
    if value != value:
        return "NaN"
    if value in (float("inf"), float("-inf")):
        return "Infinity" if value > 0 else "-Infinity"
    if value == int(value) and abs(value) < 1e21:
        return str(int(value))
    return repr(value)


def show(value):
    """A value as the commands print it."""
    if value is None:
        return "null"
    if value is UNDEFINED:
        return "undefined"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return number_text(value)
    if isinstance(value, str):
        text = json.dumps(value, ensure_ascii=False)
        try:
            text.encode("utf-8")
        except UnicodeEncodeError:
            # Half of a UTF-16 pair can't be printed: it shows as its escape.
            text = json.dumps(value)
        return text
    return value.text() if isinstance(value, (Row, Rows)) else value.text


def type_name(value):
    """A value's type, as an editor shows it beside the value."""
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    return "row" if isinstance(value, Row) else "list" if isinstance(value, Rows) else ""


VALUE_BYTES = {"int": 4, "bool": 4, "float": 4, "enum": 4, "int64": 8, "double": 8}


def state_read(app, state):
    """A state's value."""
    kind = state["type"]
    address = app.arena + state["offset"]
    if kind == "string":
        return read_typed(kind, read(address, state["cap"] + 1), 0, state["cap"])
    if kind == "list":
        return Rows(app, state)
    return read_typed(kind, read(address, VALUE_BYTES.get(kind, 4)), 0, values=state.get("values"))


def state_value(app, state):
    return show(state_read(app, state))


STRING_ARENA = 0x80000000
STRING_NATIVE = 0x40000000


def register_string(app, ref):
    """A string register's text: empty, in the arena, a native event's, or a package constant."""
    if ref == 0:
        return ""
    if ref & STRING_ARENA:
        return read_string(app.arena + (ref & ~STRING_ARENA)) or ""
    if ref & STRING_NATIVE:
        try:
            texts = gdb.parse_and_eval("'erb_native.c'::s_texts")
            return read_string(int(texts[(ref & ~STRING_NATIVE) - 1])) or ""
        except (gdb.error, gdb.MemoryError):
            return Unreadable("<a native event's string>")
    return Unreadable("<string constant %d>" % (ref - 1))


def list_row(app, local, row):
    """A row local's value: its list's row, through a filtered or sorted list when it counts in one."""
    counted = app.state_at(local.get("through", local["list"]))
    state = app.state_at(local["list"])
    if counted is None or state is None:
        return Unreadable("<row %d>" % row)
    address = app.arena + counted["offset"]
    count = read_value("I", address)
    if not 0 <= row < count:
        return Unreadable("<no row %d: %s has %d>" % (row, app.label(counted), count))
    if "through" in local:
        first = counted["fields"][0]
        row = read_value("i", address + LIST_HEADER_BYTES + row * counted["rowBytes"] + first["offset"])
        address = app.arena + state["offset"]
    data = read(address + LIST_HEADER_BYTES + row * state["rowBytes"], state["rowBytes"])
    return read_row(state, data, 0, row)


def local_read(app, local, base):
    """A parameter's or a local's value, in the frame at a base."""
    kind = local.get("type")
    if kind is None:
        return Unreadable("<optimised out>")
    if kind == "list":
        state = app.state_at(local["list"])
        return Rows(app, state) if state else Unreadable("<list>")
    if "register" in local:
        address = app.stack + 4 * (base + app.runtime.frame_cells + local["register"])
        if kind == "row":
            return list_row(app, local, read_value("i", address))
        if kind == "string":
            return register_string(app, read_value("I", address))
        return read_typed(kind, read(address, VALUE_BYTES.get(kind, 4)), 0)
    address = app.arena + local["offset"]
    if kind == "string":
        return read_typed(kind, read(address, local["cap"] + 1), 0, local["cap"])
    return read_typed(kind, read(address, VALUE_BYTES.get(kind, 4)), 0)


def local_value(app, local, base):
    return show(local_read(app, local, base))


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
# Expressions
# ----------------------------------------------------------------------------------------------------------------------

TOKEN = re.compile(
    r"""\s*(?:
        (?P<number>0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)
      | (?P<name>[A-Za-z_$][\w$]*)
      | (?P<string>"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')
      | (?P<operator>>>>=|\.\.\.|===|!==|\*\*=|<<=|>>=|>>>|&&=|\|\|=|\?\?=|=>|==|!=|<=|>=|&&|\|\||\?\?|\?\.|\*\*|\+\+|--|<<|>>
                    |[-+*/%&|^]=|[-+*/%<>=!&|^~?:.,()\[\]{}`])
    )""",
    re.VERBOSE,
)

# Each binary operator's precedence: a higher one binds tighter.
BINARY = {
    "??": 1,
    "||": 2,
    "&&": 3,
    "|": 4,
    "^": 5,
    "&": 6,
    "==": 7,
    "!=": 7,
    "===": 7,
    "!==": 7,
    "<": 8,
    "<=": 8,
    ">": 8,
    ">=": 8,
    "<<": 9,
    ">>": 9,
    ">>>": 9,
    "+": 10,
    "-": 10,
    "*": 11,
    "/": 11,
    "%": 11,
    "**": 12,
}
ASSIGNMENTS = {"=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "??=", "++", "--"}
CONSTANTS = {"true": True, "false": False, "null": None, "undefined": UNDEFINED, "NaN": float("nan"), "Infinity": float("inf")}
REFUSED_WORDS = {"new", "delete", "void", "await", "yield", "function", "class", "this", "super", "async", "in", "instanceof"}
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}


class Node:
    """A part of a parsed expression, with its source text for an error to quote."""

    def __init__(self, kind, text, parts):
        self.kind = kind
        self.text = text
        self.parts = parts


class Parser:
    """Parses the expressions a debugger reads: names, members and indexes, literals and operators. Whatever would
    run the app's code or change it is refused by name."""

    def __init__(self, source):
        self.source = source.strip()
        self.tokens = []
        self.at = 0
        self.last_end = 0
        position = 0
        while position < len(self.source):
            match = TOKEN.match(self.source, position)
            if not match:
                raise self.broken("`%s` isn't expected there" % self.source[position:].lstrip()[0])
            kind = match.lastgroup
            self.tokens.append((kind, match.group(kind), match.start(kind), match.end()))
            position = match.end()
        if not self.tokens:
            raise ErbError("an expression is missing")

    def broken(self, why):
        return ErbError("`%s` isn't an expression the debugger reads: %s" % (self.source, why))

    def refuse(self, what):
        return ErbError(
            "`%s` has %s, which the debugger doesn't evaluate: it reads names, members and indexes, literals and "
            "operators, and runs nothing on the board" % (self.source, what)
        )

    def peek(self):
        if self.at < len(self.tokens):
            return self.tokens[self.at]
        return (None, None, len(self.source), len(self.source))

    def take(self):
        token = self.tokens[self.at]
        self.at += 1
        self.last_end = token[3]
        return token

    def expect(self, text):
        if self.peek()[1] != text:
            raise self.unexpected("`%s` is missing" % text)
        self.take()

    def node(self, kind, start, *parts):
        return Node(kind, self.source[start : self.last_end], parts)

    def unexpected(self, otherwise=None):
        """The error for the next token: what it is when the debugger refuses it, else that it doesn't belong."""
        kind, text, _, _ = self.peek()
        if text == "(":
            return self.refuse("a call")
        if text in ASSIGNMENTS:
            return self.refuse("an assignment (%s)" % text)
        if text == "=>":
            return self.refuse("a function")
        if text == "`":
            return self.refuse("a template literal")
        if text in (",", "..."):
            return self.refuse("`%s`" % text)
        if kind == "name" and text in REFUSED_WORDS:
            return self.refuse("`%s`" % text)
        if otherwise:
            return self.broken(otherwise)
        if kind is None:
            return self.broken("it ends where a value should be")
        return self.broken("`%s` isn't expected there" % text)

    def parse(self):
        tree = self.conditional()
        if self.at < len(self.tokens):
            raise self.unexpected()
        return tree

    def conditional(self):
        start = self.peek()[2]
        test = self.binary(1)
        if self.peek()[1] != "?":
            return test
        self.take()
        then = self.conditional()
        self.expect(":")
        return self.node("conditional", start, test, then, self.conditional())

    def binary(self, least):
        start = self.peek()[2]
        left = self.unary()
        while True:
            kind, text, _, _ = self.peek()
            level = BINARY.get(text) if kind == "operator" else None
            if level is None or level < least:
                return left
            self.take()
            # ** groups from the right.
            right = self.binary(level if text == "**" else level + 1)
            left = self.node("binary", start, text, left, right)

    def unary(self):
        kind, text, start, _ = self.peek()
        if (kind == "operator" and text in ("!", "-", "+", "~")) or (kind == "name" and text == "typeof"):
            self.take()
            return self.node("unary", start, text, self.unary())
        return self.postfix()

    def postfix(self):
        start = self.peek()[2]
        node = self.primary()
        while True:
            kind, text, _, _ = self.peek()
            if kind != "operator" or text not in (".", "?.", "["):
                if text in ("(", "++", "--", "`"):
                    raise self.unexpected()
                return node
            self.take()
            optional = text == "?."
            if optional and self.peek()[1] == "[":
                text = self.take()[1]
            if text == "[":
                index = self.conditional()
                self.expect("]")
                node = self.node("index", start, node, index, optional)
                continue
            kind, name, _, _ = self.peek()
            if kind != "name":
                raise self.unexpected("a name should follow `%s`" % text)
            self.take()
            node = self.node("member", start, node, name, optional)

    def primary(self):
        kind, text, start, _ = self.peek()
        if kind == "number":
            self.take()
            try:
                value = int(text, 0)
            except ValueError:
                value = float(text)
            return self.node("literal", start, value)
        if kind == "string":
            self.take()
            return self.node("literal", start, unquoted(text))
        if kind == "name" and text in CONSTANTS:
            self.take()
            return self.node("literal", start, CONSTANTS[text])
        if kind == "name" and text not in REFUSED_WORDS:
            self.take()
            return self.node("name", start, text)
        if text == "(":
            self.take()
            inner = self.conditional()
            self.expect(")")
            return inner
        if text == "[":
            raise self.refuse("an array literal")
        if text == "{":
            raise self.refuse("an object literal")
        raise self.unexpected()


def unquoted(text):
    """A string literal's value."""

    def escape(match):
        body = match.group(1)
        if body[0] in "ux" and len(body) > 1:
            return chr(int(body.strip("ux{}"), 16))
        return ESCAPES.get(body, body)

    return re.sub(r"\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|.)", escape, text[1:-1])


def parse_expression(text):
    return Parser(text).parse()


def to_number(value):
    """A value as a number, as JavaScript's operators take it."""
    if isinstance(value, (bool, int)):
        return int(value)
    if isinstance(value, float):
        return value
    if value is None:
        return 0
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return 0
        try:
            if re.match(r"^0[xXbBoO]", text):
                return int(text, 0)
            if re.match(r"^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$", text):
                return float(text) if re.search(r"[.eE]", text) else int(text)
            if re.match(r"^[+-]?Infinity$", text):
                return float(text.replace("Infinity", "inf"))
        except ValueError:
            pass
    return float("nan")


def to_text(value):
    """A value as text, as JavaScript joins it to a string."""
    return value if isinstance(value, str) else show(value)


def to_int32(value):
    number = to_number(value)
    if number != number or number in (float("inf"), float("-inf")):
        return 0
    number = int(number) & 0xFFFFFFFF
    return number - 0x100000000 if number & 0x80000000 else number


def truthy(value):
    if isinstance(value, Unreadable):
        raise ErbError("a value that can't be read (%s) is neither true nor false" % value.text)
    if value is None or value is UNDEFINED:
        return False
    if isinstance(value, (bool, int, float)):
        return value == value and value != 0
    return len(value) > 0 if isinstance(value, str) else True


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def same(left, right):
    """=== on two values. A row is the same as itself: its list's row of that number."""
    if is_number(left) and is_number(right):
        return left == right
    if isinstance(left, Row) and isinstance(right, Row):
        return left.state is right.state and left.index is not None and left.index == right.index
    if isinstance(left, Rows) and isinstance(right, Rows):
        return left.state is right.state
    if type(left) is not type(right):
        return False
    return left == right if isinstance(left, (bool, str)) else left is right


def loosely_same(left, right):
    """== on two values."""
    nullish = lambda value: value is None or value is UNDEFINED
    if nullish(left) or nullish(right):
        return nullish(left) and nullish(right)
    if isinstance(left, (Row, Rows)) or isinstance(right, (Row, Rows)) or type(left) is type(right):
        return same(left, right)
    return to_number(left) == to_number(right)


def arithmetic(operator, left, right):
    """An arithmetic operator on two numbers, as JavaScript works it: whole numbers stay whole."""
    not_a_number = float("nan")
    infinity = float("inf")
    try:
        if operator == "+":
            return left + right
        if operator == "-":
            return left - right
        if operator == "*":
            return left * right
        if operator == "/":
            if right == 0:
                return not_a_number if left == 0 or left != left else math.copysign(infinity, left) * math.copysign(1, right)
            whole = isinstance(left, int) and isinstance(right, int) and left % right == 0
            return left // right if whole else left / right
        if operator == "%":
            if right == 0 or left in (infinity, -infinity):
                return not_a_number
            if isinstance(left, int) and isinstance(right, int):
                return int(math.copysign(abs(left) % abs(right), left))
            return math.fmod(left, right)
        result = left**right
        return not_a_number if isinstance(result, complex) else result
    except ZeroDivisionError:
        return infinity
    except OverflowError:
        return infinity


def compare(operator, left, right):
    if not (isinstance(left, str) and isinstance(right, str)):
        left, right = to_number(left), to_number(right)
    if operator == "<":
        return left < right
    if operator == "<=":
        return left <= right
    return left > right if operator == ">" else left >= right


def kind_of(value):
    """What a value is, for an error."""
    if isinstance(value, Row):
        return "a row"
    if isinstance(value, Rows):
        return "a list"
    if isinstance(value, str):
        return "a string"
    return "a boolean" if isinstance(value, bool) else "a number"


class Scope:
    """What an expression's names read: a frame's parameters and locals, then the app's state."""

    def __init__(self, app, frame=0):
        self.app = app
        self.base = 0
        self.locals = {}
        chain = app.frame_chain() if app.runtime.flag("in_vm") else []
        if 0 <= frame < len(chain):
            function, offset, self.base = chain[frame]
            self.locals = {local["name"]: local for local in frame_locals(app, function, offset)}

    def has(self, name):
        labels = state_labels(self.app.panel)
        return name in self.locals or any(
            name in (state["name"], state["storage"], labels[state["storage"]]) for state in self.app.panel["states"]
        )

    def read(self, name):
        if name in self.locals:
            return local_read(self.app, self.locals[name], self.base)
        if not self.has(name):
            labels = state_labels(self.app.panel)
            names = list(self.locals) + [labels[state["storage"]] for state in self.app.panel["states"] if not state["hidden"]]
            raise ErbError("nothing here is named %s; the locals and the app's state: %s" % (name, ", ".join(names) or "none"))
        return state_read(self.app, self.app.state(name))

    def labelled(self, label):
        """A state by the label the commands show, such as a component's `Counter.taps`, or None."""
        labels = state_labels(self.app.panel)
        for state in self.app.panel["states"]:
            if labels[state["storage"]] == label:
                return state
        return None


class Skipped(Exception):
    """An optional chain (`a?.b.c`) met null or undefined: the whole chain is undefined."""


class Evaluator:
    def __init__(self, scope):
        self.scope = scope

    def value(self, node):
        if node.kind in ("member", "index"):
            try:
                return self.link(node)
            except Skipped:
                return UNDEFINED
        return getattr(self, node.kind)(node)

    def usable(self, node):
        """A node's value, for an operator to work on."""
        value = self.value(node)
        if isinstance(value, Unreadable):
            raise ErbError("%s can't be read: it is %s" % (node.text, value.text))
        return value

    def plain(self, node, operator):
        """A node's value, for an operator that takes numbers, strings and booleans."""
        value = self.usable(node)
        if isinstance(value, (Row, Rows)):
            raise ErbError("%s is %s, which `%s` can't take" % (node.text, kind_of(value), operator))
        return value

    def literal(self, node):
        return node.parts[0]

    def name(self, node):
        return self.scope.read(node.parts[0])

    def link(self, node):
        """A member or an index, in a chain that an optional link ends."""
        target_node, key, optional = node.parts
        if node.kind == "member" and target_node.kind == "name" and not self.scope.has(target_node.parts[0]):
            state = self.scope.labelled("%s.%s" % (target_node.parts[0], key))
            if state is not None:
                return state_read(self.scope.app, state)
        if target_node.kind in ("member", "index"):
            target = self.link(target_node)
        else:
            target = self.value(target_node)
        if isinstance(target, Unreadable):
            raise ErbError("%s can't be read: it is %s" % (target_node.text, target.text))
        if node.kind == "index":
            key = self.usable(key)
        if target is None or target is UNDEFINED:
            if optional:
                raise Skipped()
            raise ErbError("%s is %s, so it has no %s" % (target_node.text, show(target), to_text(key)))
        whole = is_number(key) and math.isfinite(key) and key == int(key)
        if isinstance(target, Row) and isinstance(key, str):
            return target.field(key)
        if isinstance(target, Rows) and node.kind == "index" and is_number(key):
            return target.row(int(key)) if whole else UNDEFINED
        if isinstance(target, str) and node.kind == "index" and is_number(key):
            # By UTF-16 unit, as `length` counts: half of a pair is a string of its own.
            units = target.encode("utf-16-le")
            if not whole or not 0 <= key < len(units) // 2:
                return UNDEFINED
            return units[2 * int(key) : 2 * int(key) + 2].decode("utf-16-le", "surrogatepass")
        if isinstance(target, Rows) and key == "length":
            return target.count
        if isinstance(target, str) and key == "length":
            return len(target.encode("utf-16-le")) // 2
        raise ErbError("%s is %s, which has no %s here" % (target_node.text, kind_of(target), to_text(key)))

    def unary(self, node):
        operator, operand = node.parts
        if operator == "typeof":
            value = self.usable(operand)
            if value is UNDEFINED:
                return "undefined"
            return type_name(value) if isinstance(value, (bool, int, float, str)) else "object"
        if operator == "!":
            return not truthy(self.usable(operand))
        value = self.plain(operand, operator)
        if operator == "~":
            return ~to_int32(value)
        return arithmetic("-", 0, to_number(value)) if operator == "-" else to_number(value)

    def binary(self, node):
        operator, left_node, right_node = node.parts
        if operator in ("&&", "||", "??"):
            left = self.usable(left_node)
            if operator == "??":
                keep = left is not None and left is not UNDEFINED
            else:
                keep = truthy(left) == (operator == "||")
            return left if keep else self.usable(right_node)
        if operator in ("===", "!==", "==", "!="):
            left, right = self.usable(left_node), self.usable(right_node)
            equal = same(left, right) if len(operator) == 3 else loosely_same(left, right)
            return equal == (operator[0] == "=")
        left, right = self.plain(left_node, operator), self.plain(right_node, operator)
        if operator == "+" and (isinstance(left, str) or isinstance(right, str)):
            # Two halves of a UTF-16 pair join into its character.
            joined = to_text(left) + to_text(right)
            return joined.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "surrogatepass")
        if operator in ("<", "<=", ">", ">="):
            return compare(operator, left, right)
        if operator in ("&", "|", "^", "<<", ">>", ">>>"):
            return bitwise(operator, left, right)
        return arithmetic(operator, to_number(left), to_number(right))

    def conditional(self, node):
        test, then, otherwise = node.parts
        return self.value(then if truthy(self.usable(test)) else otherwise)


def bitwise(operator, left, right):
    """A bitwise operator, on 32-bit integers."""
    left, right = to_int32(left), to_int32(right)
    if operator == "&":
        return left & right
    if operator == "|":
        return left | right
    if operator == "^":
        return left ^ right
    if operator == "<<":
        return to_int32(left << (right & 31))
    if operator == ">>":
        return left >> (right & 31)
    return (left & 0xFFFFFFFF) >> (right & 31)


def evaluate(tree, scope):
    return Evaluator(scope).value(tree)


def evaluate_text(app, text, frame=0):
    """An expression's value over a frame's locals and the app's state."""
    scope = Scope(app, frame)
    # A state's label needn't read as an expression: `Counter:16.taps`.
    state = None if text in scope.locals else scope.labelled(text)
    return state_read(app, state) if state is not None else evaluate(parse_expression(text), scope)


# ----------------------------------------------------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------------------------------------------------


class ErbCommand(gdb.Command):
    """Commands for a Flow B app on a halted board.

erb file PATH        Load the app's debug file (dist/app.erdbg).
erb where            The app's source line and call chain, with each function's parameters.
erb locals [N]       The current function's parameters and locals, or the Nth caller's.
erb print EXPR       An expression over the locals and the state: item.label, count + 1, rows.length > 3.
erb error            The last fault or refused load.
erb state [NAME]     Every state, or one.
erb set NAME VALUE   Set a state; the app runs its update at its next tick.
erb watch NAME       Stop when a state changes, on the line that wrote it.
erb break [LINE]     Stop at a source line (App.jsx:41, or 41); with no line, list the breakpoints.
                     LINE hits N stops from the Nth pass on; LINE if EXPR stops when the expression is true.
erb log LINE TEXT    Print a message at a source line without stopping, with each {expression} in it worked out.
erb delete [N...]    Delete breakpoints, or all of them.
erb faults [on|off]  Stop when the app faults or a load is refused (on by default).
erb step             Run to the next statement, into a function the app calls.
erb next             Run to the next statement, over a function the app calls.
erb finish           Run until the function returns to its caller.
erb pools            What the app and the engine use of their pools.
er nodes             The node tree, with each node's rect and source line.

Breakpoints, stepping and the stop at a fault need firmware built with ERB_DEBUG_HOOK=1."""

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
        if fault_stops[0] and has_hook():
            hook_breakpoint()
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
        chain = app.frame_chain()
        print(app.place(*frames[0]) + arguments_text(app, *chain[0]))
        for function, offset, base in chain[1:]:
            print("  called from %s%s" % (app.place(function, offset), arguments_text(app, function, offset, base)))


def arguments_text(app, function, offset, base):
    """A frame's parameters, as `  (name=value, …)`, or nothing for a function entered with none."""
    parameters = [local for local in app.locals_at(function, offset) if local.get("parameter")]
    if not parameters:
        return ""
    return "  (%s)" % ", ".join("%s=%s" % (local["name"], local_value(app, local, base)) for local in parameters)


class ErbLocals(gdb.Command):
    """The current function's parameters and locals: erb locals [N] for the Nth caller in `erb where`."""

    def __init__(self):
        super().__init__("erb locals", gdb.COMMAND_STACK)

    def invoke(self, argument, from_tty):
        app = App()
        if not app.runtime.flag("in_vm"):
            raise ErbError("the app isn't running its code, so it has no locals")
        chain = app.frame_chain()
        try:
            number = int(argument.strip() or "0")
        except ValueError:
            raise ErbError("name a frame by its number: erb locals 1 for the function that called this one")
        if not 0 <= number < len(chain):
            raise ErbError("the app's call chain has %d frame%s" % (len(chain), "" if len(chain) == 1 else "s"))
        function, offset, base = chain[number]
        shown = frame_locals(app, function, offset)
        if not shown:
            print("No locals.")
        for local in shown:
            print("%s = %s" % (local["name"], local_value(app, local, base)))


class ErbPrint(gdb.Command):
    """An expression's value, over the current function's locals and the app's state: erb print rows.length > 3.
It takes names, members and indexes (item.label, rows[2].done), literals and operators. It reads the board and runs
nothing on it, so it has no calls and no assignments."""

    def __init__(self):
        super().__init__("erb print", gdb.COMMAND_DATA)

    def invoke(self, argument, from_tty):
        text = argument.strip()
        if not text:
            raise ErbError("give an expression: erb print count + 1")
        print("%s = %s" % (text, show(evaluate_text(App(), text))))


def frame_locals(app, function, offset):
    """The parameters and locals a frame shows, parameters first."""
    # A name declared again in an inner block hides the outer one.
    shown = {}
    for local in app.locals_at(function, offset):
        shown[local["name"]] = local
    return sorted(shown.values(), key=lambda local: not local.get("parameter"))


class ErbErrorCommand(gdb.Command):
    """The last fault or refused load: the reason, the amounts, and the source line."""

    def __init__(self):
        super().__init__("erb error", gdb.COMMAND_STATUS)

    def invoke(self, argument, from_tty):
        runtime = Runtime()
        error = last_error(runtime)
        if error is None:
            print("No error: the app is %s." % ("running" if runtime.flag("loaded") else "not loaded"))
            return
        print("\n".join(error["lines"]))
        print("The app %s." % ("stopped" if runtime.flag("faulted") else "is running" if runtime.flag("loaded") else "was not loaded"))


def last_error(runtime):
    """The last fault or refused load: its reason, the lines `erb error` prints, and its source line. None for none."""
    fields = runtime.error_fields
    data = read(runtime.error_address, fields["build_id_at"] + 4)
    status = unpack("I" if fields["status_bytes"] == 4 else "B" if fields["status_bytes"] == 1 else "H", data, fields["status_at"])
    if status == 0:
        return None
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
    lines = [reason]
    source_line = None
    if status == ERROR_TOO_BIG:
        lines.append("  %s: needs %d, the firmware reserves %d" % (RESOURCE_NAMES.get(resource, "resource %d" % resource), needed, available))
    elif status == ERROR_ENGINE_ABI:
        lines.append("  built for engine ABI %d" % needed)
    elif status == ERROR_NO_PANEL:
        lines.append("  this screen is %d x %d" % (needed, available))
    elif status == ERROR_STACK:
        lines.append("  needs %d cells, the arena has %d" % (needed, available))
    if detail:
        lines.append("  check: %s" % detail)
    if function != NO_FUNCTION:
        where = "function %d at %d" % (function, offset)
        if debug_file is not None and build_id == debug_file["buildId"]:
            app = App(need_running=False)
            where = app.place(function, offset)
            source_line = statement_line(app.panel, function, offset)[1]
        elif debug_file is not None:
            where += " (build %s, but the debug file is build %s)" % (build_hex(build_id), build_hex(debug_file["buildId"]))
        lines.append("  at %s" % where)
    if build_id:
        lines.append("  build %s" % build_hex(build_id))
    return {"reason": reason, "lines": lines, "line": source_line}


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
        print("%s = %s" % (app.label(state), set_state(app, state, parts[1])))


def set_state(app, state, text):
    """Writes a state from its text and asks the app for an update. Returns the value it now shows."""
    write(app.arena + state["offset"], encode_value(state, text))
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
    return value


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
AT_FAULT = 3
AT_REFUSED = 4
END_OF_FUNCTION = 0xFFFFFFFF

NO_HOOK = (
    "the firmware has no breakpoint hook: build it with ERB_DEBUG_HOOK=1 for erb break, step, next and finish, and "
    "for the stop at a fault. "
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

    def step_ends(self, function, offset, base):
        """Whether the step under way ends at a place, as the VM tests it."""
        kind = self.step()
        if kind in (STEP_NONE, STEP_ANY):
            return kind == STEP_ANY
        outside = not self.read("step_from", "I") <= offset < self.read("step_to", "I")
        step_base = self.read("step_base", "I")
        if kind == STEP_INTO:
            return function != self.read("step_function", "H") or outside
        return base < step_base or (kind == STEP_OVER and base == step_base and outside)

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


# The app's breakpoints, each {"number", "line"} and its terms; the hook holds their places for the layout the board
# runs.
breakpoints = []
breakpoint_numbers = [0]
# Set while the app is stopped where a step reached the return to the runtime.
returned = [False]
# The hook's breakpoint, and whether the board's last stop was in it.
hook_state = {"breakpoint": None, "stopped": False, "noted": False, "hit": []}
# Whether a fault or a refused load stops the board, and the one it is stopped at.
fault_stops = [True]
fault_stop = [None]
source_lines = {}


def source_path():
    """The app's source file, when it can be found beside the debug file or here, else an empty string."""
    path = source_lines.get("path")
    if path is None:
        path = ""
        if debug_path:
            folder = os.path.dirname(os.path.abspath(debug_path))
            for candidate in (os.getcwd(), folder, os.path.dirname(folder)):
                for sub in ("", "src"):
                    full = os.path.join(candidate, sub, debug_file["file"])
                    if not path and os.path.isfile(full):
                        path = os.path.abspath(full)
        source_lines["path"] = path
    return path


def source_text(line):
    """A source line's text, when the source file can be found."""
    if "lines" not in source_lines:
        try:
            with open(source_path(), encoding="utf-8") as handle:
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


HIT_FORM = re.compile(r"^(>=|<=|==|=|>|<|%)?\s*(\d+)$")


def set_terms(entry, condition=None, hits=None, log=None):
    """Gives a breakpoint its condition, hit count and log message. Each is parsed here, so a mistake is said when
    the breakpoint is set."""
    terms = tuple((term or "").strip() or None for term in (condition, hits, log))
    if entry.get("terms", (None, None, None)) == terms and "passes" in entry:
        return
    condition, hits, log = terms
    tree = parse_expression(condition) if condition else None
    test = None
    if hits:
        match = HIT_FORM.match(hits)
        if not match or (match.group(1) == "%" and int(match.group(2)) == 0):
            raise ErbError("a hit count is a number, or one after >, >=, ==, <, <= or %%, not `%s`" % hits)
        test = (match.group(1) or ">=", int(match.group(2)))
    parts = None
    if log:
        # Text and {expression} parts by turns.
        parts = [part if index % 2 == 0 else parse_expression(part) for index, part in enumerate(re.split(r"\{(.*?)\}", log))]
    entry.update(terms=terms, tree=tree, test=test, parts=parts, passes=0)


def terms_text(entry):
    """A breakpoint's terms, as `erb break` lists them."""
    condition, hits, log = entry.get("terms", (None, None, None))
    return "".join("  %s %s" % (word, term) for word, term in (("hits", hits), ("if", condition), ("log", log)) if term)


def hit_counts(test, passes):
    """Whether a hit count takes a pass. A bare number takes that pass and each after it."""
    operator, number = test
    if operator == "%":
        return passes % number == 0
    if operator in ("=", "=="):
        return passes == number
    if operator in ("<", "<="):
        return passes < number or (operator == "<=" and passes == number)
    return passes > number or (operator == ">=" and passes == number)


def log_line(text):
    """Prints a logpoint's message, to the editor in a debug adapter's session, as GDB's own logpoints do."""
    text = text.encode("utf-8", "replace").decode("utf-8")
    server = sys.modules.get("gdb.dap.server")
    if server is not None and hasattr(server, "send_event"):
        server.send_event("output", {"category": "console", "output": text + "\n"})
    else:
        print(text)


def stops_at(entry, scope):
    """Whether a breakpoint the app reached stops it. It doesn't when its condition is false or its hit count
    leaves the pass out, and one with a message logs it instead. A condition that can't be worked out stops."""
    try:
        if entry.get("tree") is not None and not truthy(evaluate(entry["tree"], scope)):
            return False
    except gdb.GdbError as error:
        log_line("erb breakpoint %d's condition: %s" % (entry["number"], error))
        return True
    entry["passes"] = entry.get("passes", 0) + 1
    if entry.get("test") and not hit_counts(entry["test"], entry["passes"]):
        return False
    if entry.get("parts") is None:
        return True
    message = ""
    for index, part in enumerate(entry["parts"]):
        if index % 2 == 0:
            message += part
            continue
        try:
            message += to_text(evaluate(part, scope))
        except gdb.GdbError as error:
            message += "<%s>" % error
    log_line(message)
    return False


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
    """Where the hook stops: at a breakpoint's place, where a step ends, at a fault or a refused load, and after each
    load, when the breakpoints are written for the package that loaded. A step that comes back from a call partway
    through a statement runs on to the next one here, without a stop."""

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
            for entry in breakpoints:
                entry["passes"] = 0
            if breakpoints:
                try:
                    write_stops()
                except gdb.GdbError as error:
                    print("erb: the app loaded, but its breakpoints aren't set: %s" % error)
            return False
        if event in (AT_FAULT, AT_REFUSED):
            hook.set_step(STEP_NONE)
            return self.at_fault(runtime, event == AT_REFUSED)
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
        reached = [entry for entry in breakpoints if (function, offset) in line_places(app.panel, entry["line"])]
        scope = Scope(app) if reached else None
        stopping = [entry for entry in reached if stops_at(entry, scope)]
        if reached and not stopping and not hook.step_ends(function, offset, runtime.u32("frame_base")):
            # The step under way, if any, goes on.
            return False
        hook.set_step(STEP_NONE)
        hook_state["hit"] = [entry["number"] for entry in stopping]
        numbers = [str(number) for number in hook_state["hit"]]
        prefix = "erb breakpoint %s, " % ", ".join(numbers) if numbers else ""
        self.report(prefix + stop_line(app, function, offset))
        return True

    def at_fault(self, runtime, refused):
        """Stops at a fault or a refused load, with what `erb error` says of it."""
        if not fault_stops[0]:
            return False
        error = last_error(runtime)
        if error is None:
            return False
        lines = ["%s: %s" % ("Load refused" if refused else "App fault", error["reason"])] + error["lines"][1:]
        source = source_text(error["line"]) if error["line"] else None
        if source is not None:
            lines.append("%d\t%s" % (error["line"], source))
        fault_stop[0] = {"reason": error["reason"], "text": "\n".join(error["lines"]), "refused": refused}
        self.report("\n".join(lines))
        return True


def arm_step(kind):
    """Writes a step into the hook, for the board's next resume."""
    if not has_hook():
        raise ErbError(NO_HOOK)
    if fault_stop[0]:
        raise ErbError("the app %s, so it has no next statement: `continue` runs the firmware on" % ("wasn't loaded" if fault_stop[0]["refused"] else "faulted"))
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


def run_step(kind):
    """Starts a step and resumes the board, then shows where it stopped."""
    arm_step(kind)
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


def add_breakpoint(line, condition=None, hits=None, log=None):
    """Adds a breakpoint at a source line, as `erb break` and `erb log` name it, and says so."""
    if debug_file is None:
        raise ErbError("no debug file: load the app's with `erb file dist/app.erdbg`")
    if not has_hook():
        raise ErbError(NO_HOOK)
    entry = {"number": breakpoint_numbers[0] + 1, "line": parse_line(line)}
    set_terms(entry, condition, hits, log)
    hook_breakpoint()
    breakpoint_numbers[0] += 1
    breakpoints.append(entry)
    try:
        write_stops()
        pending = ""
    except ErbError as error:
        if "ERB_DEBUG_STOPS" in str(error):
            breakpoints.remove(entry)
            raise
        pending = " (set when the app loads)"
    print("erb breakpoint %d at %s:%d%s%s" % (entry["number"], debug_file["file"], entry["line"], terms_text(entry), pending))


class ErbBreak(gdb.Command):
    """Stop at a source line: erb break App.jsx:41, or erb break 41. With no line, list the breakpoints.
erb break 41 if count === 5 stops when the expression is true, as `erb print` works it out at each pass.
erb break 41 hits 3 stops from the third pass on; hits ==3, >3, <3 and %3 (every third) pick others.
Needs firmware built with ERB_DEBUG_HOOK=1. A breakpoint set before the app loads is written when it does.
The debugger tests a condition, not the board, so each pass halts the board for a moment."""

    def __init__(self):
        super().__init__("erb break", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        if not argument.strip():
            if debug_file is None:
                raise ErbError("no debug file: load the app's with `erb file dist/app.erdbg`")
            if not breakpoints:
                print("No erb breakpoints.")
            for entry in breakpoints:
                print("%d  %s:%d%s" % (entry["number"], debug_file["file"], entry["line"], terms_text(entry)))
            return
        match = re.match(r"^(\S+)(?:\s+hits\s+(\S+))?(?:\s+if\s+(.+))?$", argument.strip())
        if not match:
            raise ErbError("erb break LINE [hits N] [if EXPRESSION]: erb break 41 if count === 5")
        add_breakpoint(match.group(1), match.group(3), match.group(2))


class ErbLog(gdb.Command):
    """Print a message each time a source line is reached, without stopping: erb log App.jsx:41 pressed {count}.
Each {expression} is worked out as `erb print` does. `erb break` lists it and `erb delete` deletes it.
Needs firmware built with ERB_DEBUG_HOOK=1."""

    def __init__(self):
        super().__init__("erb log", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        parts = argument.strip().split(None, 1)
        if len(parts) != 2:
            raise ErbError("name the line and the message: erb log 41 pressed {count}")
        add_breakpoint(parts[0], log=parts[1])


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


class ErbFaults(gdb.Command):
    """Stop when the app faults or a load is refused: erb faults on, or off. With neither, says which it is.
Needs firmware built with ERB_DEBUG_HOOK=1. The stop is before the fault is handled, so `erb where` and `erb locals`
still read the handler that faulted."""

    def __init__(self):
        super().__init__("erb faults", gdb.COMMAND_BREAKPOINTS)

    def invoke(self, argument, from_tty):
        word = argument.strip()
        if word not in ("", "on", "off"):
            raise ErbError("erb faults on, or erb faults off")
        if word:
            set_fault_stops(word == "on")
        if fault_stops[0] and not has_hook():
            print("A fault doesn't stop the board: %s" % NO_HOOK)
            return
        print("A fault %s the board." % ("stops" if fault_stops[0] else "doesn't stop"))


def set_fault_stops(stop):
    if stop and not has_hook():
        raise ErbError(NO_HOOK)
    fault_stops[0] = stop
    if stop:
        hook_breakpoint()


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
    hook_state["hit"] = []
    fault_stop[0] = None
    returned[0] = False


def on_new_objfile(event):
    hook_state["noted"] = False


gdb.events.stop.connect(on_stop)
gdb.events.cont.connect(on_resume)
gdb.events.new_objfile.connect(on_new_objfile)


# ----------------------------------------------------------------------------------------------------------------------
# The debug adapter's requests
# ----------------------------------------------------------------------------------------------------------------------

MOST_ROWS_LISTED = 256
STEP_KINDS = {"into": STEP_INTO, "over": STEP_OVER, "out": STEP_OUT}


def dap_info(request):
    return {"file": debug_file["file"] if debug_file else None, "source": source_path(), "hook": has_hook()}


def dap_status(request):
    """Where the board stopped, as the adapter shows it: the app's frames, and whether the hook made the stop."""
    fault = fault_stop[0] if hook_state["stopped"] else None
    try:
        app = App()
    except gdb.GdbError as error:
        return {"loaded": False, "note": str(error), "fault": fault}
    functions = app.panel["functions"]
    chain = app.frame_chain() if app.runtime.flag("in_vm") else []
    frames = []
    for function, offset, _ in chain:
        named = function < len(functions) and functions[function]["name"]
        frames.append({"name": named or "function %d" % function, "line": statement_line(app.panel, function, offset)[1]})
    status = {"loaded": True, "hookStop": hook_state["stopped"] and bool(frames), "frames": frames, "hit": [], "fault": fault}
    if fault:
        return status
    if status["hookStop"] and returned[0]:
        status["description"] = "%s returns to the runtime" % frames[0]["name"]
    elif status["hookStop"]:
        status["hit"] = hook_state["hit"]
    return status


def dap_breakpoints(request):
    """Replaces the app's breakpoints with the editor's, each a line and its condition, hit count and log message. A
    line with no code moves to the next line that has some."""
    if debug_file is None:
        raise ErbError("no debug file")
    # A line that stays keeps its number, so the editor's breakpoint keeps its id.
    kept = {entry["line"]: entry for entry in breakpoints}
    del breakpoints[:]
    if not has_hook():
        return {"breakpoints": [{"verified": False, "message": NO_HOOK} for _ in request["breakpoints"]]}
    hook_breakpoint()
    coded = sorted(
        {entry["line"] for panel in debug_file["panels"] for function in panel["functions"] for entry in function["lines"]}
    )
    answers = []
    for wanted in request["breakpoints"]:
        line = next((candidate for candidate in coded if candidate >= wanted["line"]), None)
        if line is None:
            answers.append({"verified": False, "message": "%s:%d has no code" % (debug_file["file"], wanted["line"])})
            continue
        entry = next((entry for entry in breakpoints if entry["line"] == line), None)
        if entry is None:
            entry = kept.get(line) or {"number": breakpoint_numbers[0] + 1, "line": line}
            try:
                set_terms(entry, wanted.get("condition"), wanted.get("hitCondition"), wanted.get("logMessage"))
            except ErbError as error:
                answers.append({"verified": False, "message": str(error)})
                continue
            breakpoint_numbers[0] = max(breakpoint_numbers[0], entry["number"])
            breakpoints.append(entry)
        answers.append({"id": entry["number"], "line": line, "verified": True})
    # The hook holds so many places: the last breakpoints that don't fit are refused.
    while True:
        try:
            write_stops()
            break
        except ErbError as error:
            if "ERB_DEBUG_STOPS" not in str(error) or not breakpoints:
                # Not running: the load writes them.
                break
            refused = breakpoints.pop()
            for answer in answers:
                if answer.get("id") == refused["number"]:
                    answer.update(verified=False, message=str(error))
    return {"breakpoints": answers}


def dap_faults(request):
    """Whether a fault stops the board, as the editor's checkbox says. Without the hook it can't."""
    if not has_hook():
        return {"verified": False, "message": NO_HOOK}
    set_fault_stops(bool(request["stop"]))
    return {"verified": True}


def dap_step(request):
    kind = STEP_KINDS[request["kind"]]
    if kind == STEP_OUT and (returned[0] or not Runtime().flag("in_vm")):
        # Nothing to return from: on to the app's next statement.
        kind = STEP_INTO
    arm_step(kind)
    return {}


def state_variable(app, state):
    variable = {"name": app.label(state), "value": state_value(app, state), "type": state["type"], "storage": state["storage"]}
    if state["type"] == "list":
        variable["rows"] = read_value("I", app.arena + state["offset"])
    return variable


def dap_variables(request):
    app = App()
    scope = request["scope"]
    if scope == "state":
        return {"variables": [state_variable(app, state) for state in app.panel["states"] if not state["hidden"]]}
    if scope == "rows":
        state = app.state(request["state"])
        address = app.arena + state["offset"]
        count = min(read_value("I", address), MOST_ROWS_LISTED)
        data = read(address + LIST_HEADER_BYTES, count * state["rowBytes"]) if count else b""
        return {
            "variables": [
                {"name": "[%d]" % row, "value": show(read_row(state, data, row * state["rowBytes"], row)), "type": "row"}
                for row in range(count)
            ]
        }
    chain = app.frame_chain() if app.runtime.flag("in_vm") else []
    if not 0 <= request["frame"] < len(chain):
        return {"variables": []}
    function, offset, base = chain[request["frame"]]
    return {
        "variables": [
            {"name": local["name"], "value": local_value(app, local, base), "type": local.get("type") or ""}
            for local in frame_locals(app, function, offset)
        ]
    }


def dap_set(request):
    app = App()
    return {"value": set_state(app, app.state(request["state"]), request["value"])}


def dap_evaluate(request):
    """An expression's value, over a frame's locals and the app's state."""
    app = App()
    value = evaluate_text(app, request["expression"].strip(), request.get("frame", -1))
    answer = {"value": show(value), "type": type_name(value)}
    if isinstance(value, Rows):
        answer.update(rows=value.count, storage=value.state["storage"])
    return answer


def settle_threads():
    """GDB's adapter takes a thread it first sees for a sign that the board runs, and then refuses what needs it
    halted. An RTOS's threads are first seen when they are listed after attaching, so they are listed here, and the
    adapter told the board is halted."""
    gdb.selected_inferior().threads()
    events = sys.modules.get("gdb.dap.events")
    if events is not None and hasattr(events, "inferior_running"):
        events.inferior_running = False


def dap_sync(request):
    """After attaching: the app doesn't count the halt, and the hook holds the breakpoints set before."""
    settle_threads()
    mark_halted()
    if fault_stops[0] and has_hook():
        hook_breakpoint()
    if breakpoints:
        write_stops()
    return {}


DAP_REQUESTS = {
    "info": dap_info,
    "status": dap_status,
    "breakpoints": dap_breakpoints,
    "faults": dap_faults,
    "step": dap_step,
    "variables": dap_variables,
    "set": dap_set,
    "evaluate": dap_evaluate,
    "sync": dap_sync,
}


class ErbDap(gdb.Command):
    """A request from the debug adapter (embedded-react debug-adapter): base64 JSON in, a line of JSON out."""

    def __init__(self):
        super().__init__("erb dap", gdb.COMMAND_SUPPORT)

    def invoke(self, argument, from_tty):
        request = json.loads(base64.b64decode(argument.strip()).decode("utf-8"))
        try:
            answer = DAP_REQUESTS[request["do"]](request)
        except (gdb.GdbError, gdb.error, gdb.MemoryError) as error:
            answer = {"error": str(error)}
        print("@erb " + json.dumps(answer))


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
ErbLocals()
ErbPrint()
ErbErrorCommand()
ErbState()
ErbSet()
ErbWatch()
note_missing_hook()
ErbBreak()
ErbLog()
ErbDelete()
ErbFaults()
ErbStep()
ErbNext()
ErbFinish()
ErbPools()
ErbDap()
ErCommand()
ErNodes()

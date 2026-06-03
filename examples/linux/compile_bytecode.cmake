# Compiles the React bundle to a QuickJS bytecode blob next to the desktop-js executable, but only
# if the bundle has been built. Invoked as a POST_BUILD step with:
#   -DCOMPILE_EXE=...   path to er-bridge-quickjs-compile
#   -DBUNDLE_SRC=...    the esbuild bundle (app.bundle.js)
#   -DBYTECODE_DST=...  output .qbc path next to the exe
#
# The host prefers the .qbc (bytecode) over the .js (source), so this demonstrates the same
# JS_ReadObject load path the MCU hosts use (Flow A — the QuickJS VM still runs the bytecode; this
# is not the Flow B AOT-to-C compiler). If the bundle is missing, the host falls back to
# source/builtin — we don't fail the build.
if (EXISTS "${BUNDLE_SRC}")
    execute_process(
            COMMAND "${COMPILE_EXE}" "${BUNDLE_SRC}" "${BYTECODE_DST}"
            RESULT_VARIABLE rc
    )
    if (rc EQUAL 0)
        message(STATUS "React bytecode compiled -> ${BYTECODE_DST}")
    else ()
        message(WARNING "bytecode compile failed (rc=${rc}); desktop-js will use source/builtin")
    endif ()
endif ()

# Copies the React bundle next to the desktop-js executable, but only if it has been built.
# Invoked as a POST_BUILD step with -DBUNDLE_SRC=... -DBUNDLE_DST=...
#
# The JS bundle is produced out-of-band by `npm run build` in bridges/quickjs/js. If it isn't
# there, the desktop-js target simply falls back to its built-in demo — we don't fail the build.
if (EXISTS "${BUNDLE_SRC}")
    configure_file("${BUNDLE_SRC}" "${BUNDLE_DST}" COPYONLY)
    message(STATUS "React bundle copied -> ${BUNDLE_DST}")
else ()
    message(STATUS "No React bundle at ${BUNDLE_SRC} — desktop-js will use its built-in demo. "
                   "Run 'npm install && npm run build' in bridges/quickjs/js to build it.")
endif ()

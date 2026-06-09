# Copies a config container (app.erpkg) into the "config slot" next to the desktop demo executable,
# but only if one has been packed. Invoked as a POST_BUILD step with -DCONFIG_SRC=... -DCONFIG_DST=...
#
# The container is produced out-of-band by `npm run pack` in bridges/quickjs/js. Like real firmware,
# the demo builds fine without it — at runtime it just shows a "No config loaded" panel until a config
# is placed in the slot. So this never fails the build.
if (EXISTS "${CONFIG_SRC}")
    configure_file("${CONFIG_SRC}" "${CONFIG_DST}" COPYONLY)
    message(STATUS "Config container copied -> ${CONFIG_DST}")
else ()
    message(STATUS "No config container at ${CONFIG_SRC} — the demo will show 'No config loaded'. "
                   "Build one with 'npm install && npm run pack' in bridges/quickjs/js, then rebuild "
                   "(or drop an app.erpkg next to the exe and restart).")
endif ()

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
# RadioSonic effects sandbox: an OUT-OF-TREE component for your own effects.
#
# The firmware core ships bypass and the channel delay. Effects you write live
# here and register through rs_user_effects_init() in student_fx.c. Delete
# this directory and the firmware still builds and runs (bypass + channel
# only), which proves no core code depends on anything in here.
#
# WHOLE_ARCHIVE is REQUIRED: rs_user_effects_init() overrides a weak symbol
# in main, and nothing else references this archive. Without it the linker
# keeps main's weak no-op and silently drops every effect here.
idf_component_register(
    SRCS
        "student_fx.c"
        # "myeffect_fx.c"      # add each effect file here
    REQUIRES main
    # Web UI cards (HTML fragments) for this component's effects. Served from
    # GET /api/fx/<name>/panel via each record's .web_panel; EMBED_TXTFILES
    # NUL-terminates them so they can be used as C strings.
    # EMBED_TXTFILES "my_panel.html"
    WHOLE_ARCHIVE
)

# Same hot-path rule as the core signal path (main/CMakeLists.txt): process()
# runs 48000x/s on core 1, and IDF's default -Os costs real throughput there.
# Uncomment with your first effect and list every effect file in it.
# set_source_files_properties(
#     "myeffect_fx.c"
#     PROPERTIES COMPILE_OPTIONS "-O2"
# )

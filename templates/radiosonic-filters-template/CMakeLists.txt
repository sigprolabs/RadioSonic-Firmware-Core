# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
# RadioSonic effect component: starting skeleton (see README.md).
#
# WHOLE_ARCHIVE is required. rs_user_effects_init() overrides a weak symbol in
# main and nothing else references this component, so without it the linker
# keeps main's empty default and no effect from here is registered.
idf_component_register(
    SRCS "my_filters.c"
    REQUIRES main        # dsp_effects.h (the registry and registration hook), common.h
    # A web card for your effect:  EMBED_TXTFILES "my_panel.html"
    WHOLE_ARCHIVE
)

# process() runs once per sample on the audio core. Build effect sources at
# -O2, as main/CMakeLists.txt does for the core signal path and channel_fx.c.
set_source_files_properties(
    "my_filters.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)

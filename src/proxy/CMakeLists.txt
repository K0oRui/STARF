add_library(version_proxy SHARED version.cpp)
set_target_properties(version_proxy PROPERTIES OUTPUT_NAME "version")

add_library(baselib_proxy SHARED baselib.cpp baselib.def)
set_target_properties(baselib_proxy PROPERTIES OUTPUT_NAME "baselib")

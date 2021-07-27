idf_component_register(
  SRCS "custom_panic.c"
)
target_link_libraries(${COMPONENT_LIB} "-Wl,--wrap=panicHandler" "-Wl,--wrap=xt_unhandled_exception")

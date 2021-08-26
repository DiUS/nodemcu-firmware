if(IDF_TARGET STREQUAL "esp32")
idf_component_register(
  SRCS "lightuart.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES "task"
)
endif()

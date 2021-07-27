idf_component_register(
  SRCS "glue.c" "s4pp-client/s4pp.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES "mbedtls"
)

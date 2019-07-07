COMPONENT_SRCDIRS := . s4pp-client
COMPONENT_OBJS := glue.o s4pp-client/s4pp.o

# yes, isdigit() uses a char array subscript.
s4pp-client/s4pp.o: CFLAGS+=-Wno-char-subscripts

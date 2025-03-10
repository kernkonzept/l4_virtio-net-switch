PKGDIR	?= .
L4DIR	?= $(PKGDIR)/../..

TARGET = lib server

include $(L4DIR)/mk/subdir.mk

# the server needs the lib itself
server: lib

#
# Authors: zhengyu li <zhengyu_li@gmail.com>
#

MESSAGE (STATUS "Using bundled find czmq")

FIND_PATH (
  LIBCZMQ_INCLUDE_DIR
  NAMES czmq.h
  PATHS /usr/include /usr/include/czmq /usr/local/include /usr/local/include/czmq)

FIND_LIBRARY (
  LIBCZMQ_LIBRARY
  NAMES czmq
  PATHS /usr/lib/ /usr/local/lib/ /usr/lib64/ /usr/lcocal/lib64/)

IF (LIBCZMQ_INCLUDE_DIR AND LIBCZMQ_LIBRARY)
  SET (CZMQ_FOUND 1)
  INCLUDE_DIRECTORIES (${LIBCZMQ_INCLUDE_DIR})
ELSE (LIBCZMQ_INCLUDE_DIR AND LIBCZMQ_LIBRARY)
  SET (CZMQ_FOUND 0)
ENDIF (LIBCZMQ_INCLUDE_DIR AND LIBCZMQ_LIBRARY)

MARK_AS_ADVANCED (
  LIBCZMQ_INCLUDE_DIR
  LIBCZMQ_LIBRARY)

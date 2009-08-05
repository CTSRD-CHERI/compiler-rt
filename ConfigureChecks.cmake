INCLUDE( CheckIncludeFile )
INCLUDE( CheckFunctionExists )

SET( PACKAGE ${PACKAGE_NAME} )
SET( VERSION ${PACKAGE_VERSION} )

SET( BINARYDIR ${CMAKE_BINARY_DIR} )
SET( SOURCEDIR ${CMAKE_SOURCE_DIR} )

# HEADER FILES
CHECK_INCLUDE_FILE( sys/byteorder.h HAVE_SYS_BYTEORDER_H )

# FUNCTIONS
#CHECK_FUNCTION_EXISTS( strlcpy HAVE_STRLCPY )

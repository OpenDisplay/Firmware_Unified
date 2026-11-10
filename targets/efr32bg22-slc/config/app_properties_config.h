#ifndef APP_PROPERTIES_CONFIG_H
#define APP_PROPERTIES_CONFIG_H

#include "sl_application_type.h"

#define SL_APPLICATION_SIGNATURE               0
#define SL_APPLICATION_SIGNATURE_LOCATION      0xFFFFFFFFu
#define SL_APPLICATION_TYPE                    APPLICATION_TYPE
/* Set by cmake_gcc/CMakeLists.txt from the firmware version: (major<<16)|(minor<<8)|patch. */
#ifndef SL_APPLICATION_VERSION
#error "SL_APPLICATION_VERSION must come from the build (cmake_gcc/CMakeLists.txt)"
#endif
#define SL_APPLICATION_CAPABILITIES            0u
#define SL_APPLICATION_PRODUCT_ID              { 0 }

#endif

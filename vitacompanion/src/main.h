#pragma once

#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 0
#endif

/* Memory budget (vitacompanion-slim defaults); CMake sets both. */
#ifndef POOL_SIZE
#define POOL_SIZE (1 * 1024 * 1024)
#endif

#ifndef FTP_BUFFER_SIZE
#define FTP_BUFFER_SIZE (64 * 1024)
#endif

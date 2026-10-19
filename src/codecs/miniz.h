/* Game & Watch: only tinfl (the inflater) of miniz */
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_MALLOC
#include "miniz_common.h"
#include "miniz_tinfl.h"

/* Single translation unit for the stb_vorbis implementation. Only the
 * memory API is used (sampleimport.cpp reads files itself). */
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

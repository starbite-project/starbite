/* Force-included before a Wine test file built against the Windows SDK (build.sh): the C99 types Wine's own headers
 * bring in, and the SDK's name for a structure Wine declares in winternl.h. */
#include <stdbool.h>
#include <stdint.h>
#define OBJECT_TYPE_INFORMATION PUBLIC_OBJECT_TYPE_INFORMATION

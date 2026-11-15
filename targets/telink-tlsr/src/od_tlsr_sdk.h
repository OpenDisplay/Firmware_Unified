/* The Telink SDK headers, for a translation unit that also includes shared/ headers.
 * Include it FIRST, before any other header.
 *
 * The SDK defines C99 names itself: `typedef u8 bool`, `typedef u32 size_t` (unsigned int,
 * where the compiler's size_t is unsigned long) and true/false as macros. shared/ uses the
 * standard ones. So the SDK's size_t is renamed while its headers are read, and <stdbool.h>
 * takes the bool names back afterwards. Both size_t types are 32-bit and every bool
 * is passed by value, so the two views of an SDK prototype agree at the ABI.
 *
 * The SDK is built with -fpack-struct (its own project files), and liblt_825x.a with it, so
 * its structs are read here under pack(1): an SDK type has the library's layout in our files
 * too. rf_packet_att_write_t is the case that matters -- unpacked, `value` moves from wire
 * offset 9 to 10. Our own and shared/ structs keep their natural layout. */
#ifndef OD_TLSR_SDK_H
#define OD_TLSR_SDK_H

/* No C11 here (tc32 is GCC 4.5); a negative array size is the portable static assert. */
#define OD_TLSR_SDK_ASSERT(c, name) typedef char od_tlsr_sdk_assert_##name[(c) ? 1 : -1]

#include <stddef.h> /* first: an SDK header includes it too, and must find size_t settled */
#define size_t od_tlsr_sdk_size_t
#pragma pack(push, 1)
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#pragma pack(pop)
#undef size_t

#undef bool
#undef true
#undef false
#include <stdbool.h>

/* The write callbacks read the value at the wire position. */
OD_TLSR_SDK_ASSERT(offsetof(rf_packet_att_write_t, value) == 9u, att_write_value_at_wire_offset_9);

#endif /* OD_TLSR_SDK_H */

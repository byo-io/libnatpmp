#pragma once

#include <stdint.h>

/**
 * @brief Marks a function as part of the shared library's interface
 *
 * The library is built with hidden visibility, so only functions marked with
 * this are exported.
 */
#if defined(__GNUC__) || defined(__clang__)
#define NATPMP_EXPORT __attribute__((visibility("default")))
#else
#define NATPMP_EXPORT
#endif

// The header works from C11 on, while the library itself is C23. This keeps
// C23's checks where they're available, with the same layout either way.
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L) ||              \
    defined(__cplusplus)
/**
 * @brief An enum two bytes wide, e.g. NATPMP_UINT16_ENUM(natpmp_result) {...};
 *        also declares natpmp_result_t
 *
 * Before C23 the type is a plain uint16_t and the values are ordinary enum
 * constants. From C23 on it's an enum with uint16_t as its underlying type,
 * which is compatible with uint16_t.
 */
#define NATPMP_UINT16_ENUM(tag)                                                \
  enum tag : uint16_t;                                                         \
  typedef enum tag tag##_t;                                                    \
  enum tag : uint16_t
#else
#define NATPMP_UINT16_ENUM(tag)                                                \
  typedef uint16_t tag##_t;                                                    \
  enum tag
#endif

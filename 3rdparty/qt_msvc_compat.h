#pragma once

#include <QtCore/qcompilerdetection.h>

// Older Qt SDKs reference stdext iterators removed in MSVC 19.50. Use the
// pointer fallback that Qt already uses with other standard libraries.
#if defined(_MSC_VER) && _MSC_VER >= 1950
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(x) (x)
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(x, n) (x)
#endif

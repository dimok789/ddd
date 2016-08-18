#ifndef _LOADER_FUNCTION_PATCHER_H
#define _LOADER_FUNCTION_PATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "utils/function_patcher.h"

extern hooks_magic_t method_hooks_loader[];
extern u32 method_hooks_size_loader;
extern volatile unsigned int method_calls_loader[];

#ifdef __cplusplus
}
#endif

#endif /* _LOADER_FUNCTION_PATCHER_H */

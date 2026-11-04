/* EmbCC's <uchar.h> for a Darwin target, whose SDK has none: C11's and
 * C23's character types. The conversion functions are not declared --
 * libSystem does not export them -- so a program that calls one fails
 * to compile here rather than to link. This directory is searched before
 * the SDK's, for Darwin targets only (src/driver/main.c). */
#ifndef _EMBCC_DARWIN_UCHAR_H
#define _EMBCC_DARWIN_UCHAR_H
#include <stddef.h>
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
typedef unsigned char char8_t;
#endif
#endif

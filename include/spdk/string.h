/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/** \file
 * String utility functions
 */

#ifndef SPDK_STRING_H
#define SPDK_STRING_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define _SPDK_STRINGIFY(x) #x
#define SPDK_STRINGIFY(x) _SPDK_STRINGIFY(x)

/**
 * sprintf with automatic buffer allocation.
 *
 * The return value is the formatted string, which should be passed to free()
 * when no longer needed.
 *
 * \param format Format for the string to print.
 *
 * \return the formatted string on success, or NULL on failure.
 */
char *spdk_sprintf_alloc(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * vsprintf with automatic buffer allocation.
 *
 * The return value is the formatted string, which should be passed to free()
 * when no longer needed.
 *
 * \param format Format for the string to print.
 * \param args A value that identifies a variable arguments list.
 *
 * \return the formatted string on success, or NULL on failure.
 */
char *spdk_vsprintf_alloc(const char *format, va_list args);

/**
 * Append string using vsprintf with automatic buffer re-allocation.
 *
 * The return value is the formatted string, in which the original string in
 * buffer is unchanged and the specified formatted string is appended.
 *
 * The returned string should be passed to free() when no longer needed.
 *
 * If buffer is NULL, the call is equivalent to spdk_sprintf_alloc().
 * If the call fails, the original buffer is left untouched.
 *
 * \param buffer Buffer which has a formatted string.
 * \param format Format for the string to print.
 *
 * \return the formatted string on success, or NULL on failure.
 */
char *spdk_sprintf_append_realloc(char *buffer, const char *format, ...);

/**
 * Append string using vsprintf with automatic buffer re-allocation.
 * The return value is the formatted string, in which the original string in
 * buffer is unchanged and the specified formatted string is appended.
 *
 * The returned string should be passed to free() when no longer needed.
 *
 * If buffer is NULL, the call is equivalent to spdk_sprintf_alloc().
 * If the call fails, the original buffer is left untouched.
 *
 * \param buffer Buffer which has a formatted string.
 * \param format Format for the string to print.
 * \param args A value that identifies a variable arguments list.
 *
 * \return the formatted string on success, or NULL on failure.
 */
char *spdk_vsprintf_append_realloc(char *buffer, const char *format, va_list args);

/**
 * Convert string to lowercase in place.
 *
 * \param s String to convert to lowercase.
 *
 * \return the converted string.
 */
char *spdk_strlwr(char *s);

/**
 * Parse a delimited string with quote handling.
 *
 * Note that the string will be modified in place to add the string terminator
 * to each field.
 *
 * \param stringp Pointer to starting location in string. *stringp will be updated
 * to point to the start of the next field, or NULL if the end of the string has
 * been reached.
 * \param delim Null-terminated string containing the list of accepted delimiters.
 *
 * \return a pointer to beginning of the current field.
 */
char *spdk_strsepq(char **stringp, const char *delim);

/**
 * Trim whitespace from a string in place.
 *
 * \param s String to trim.
 *
 * \return the trimmed string.
 */
char *spdk_str_trim(char *s);

/**
 * Copy the string version of an error into the user supplied buffer
 *
 * \param errnum Error code.
 * \param buf Pointer to a buffer in which to place the error message.
 * \param buflen The size of the buffer in bytes.
 */
void spdk_strerror_r(int errnum, char *buf, size_t buflen);

/**
 * Return the string version of an error from a static, thread-local buffer. This
 * function is thread safe.
 *
 * \param errnum Error code.
 *
 * \return a pointer to buffer upon success.
 */
const char *spdk_strerror(int errnum);

/**
 * Remove trailing newlines from the end of a string in place.
 *
 * Any sequence of trailing \\r and \\n characters is removed from the end of the
 * string.
 *
 * \param s String to remove newline from.
 *
 * \return the number of characters removed.
 */
size_t spdk_str_chomp(char *s);

/**
 * Copy a string into a fixed-size buffer, padding extra bytes with a specific
 * character.
 *
 * If src is longer than size, only size bytes will be copied.
 *
 * \param dst Pointer to destination fixed-size buffer to fill.
 * \param src Pointer to source null-terminated string to copy into dst.
 * \param size Number of bytes to fill in dst.
 * \param pad Character to pad extra space in dst beyond the size of src.
 */
void spdk_strcpy_pad(void *dst, const char *src, size_t size, int pad);

/**
 * Find the length of a string that has been padded with a specific byte.
 *
 * \param str Right-padded string to find the length of.
 * \param size Size of the full string pointed to by str, including padding.
 * \param pad Character that was used to pad str up to size.
 *
 * \return the length of the non-padded portion of str.
 */
size_t spdk_strlen_pad(const void *str, size_t size, int pad);

/**
 * Parse an IP address into its hostname and port components. This modifies the
 * IP address in place.
 *
 * \param ip A null terminated IP address, including port. Both IPv4 and IPv6
 * are supported.
 * \param host Will point to the start of the hostname within ip. The string will
 * be null terminated.
 * \param port Will point to the start of the port within ip. The string will be
 * null terminated.
 *
 * \return 0 on success. -EINVAL on failure.
 */
int spdk_parse_ip_addr(char *ip, char **host, char **port);

/**
 * Parse a string representing a number possibly followed by a binary prefix.
 *
 * The string can contain a trailing "B" (KB,MB,GB) but it's not necessary.
 * "128K" = 128 * 1024; "2G" = 2 * 1024 * 1024; "2GB" = 2 * 1024 * 1024;
 * Additionally, lowercase "k", "m", "g" are parsed as well. They are processed
 * the same as their uppercase equivalents.
 *
 * \param cap_str Null terminated string.
 * \param cap Pointer where the parsed capacity (in bytes) will be put.
 * \param has_prefix Pointer to a flag that will be set to describe whether given
 * string contains a binary prefix (optional).
 *
 * \return 0 on success, or negative errno on failure.
 */
int spdk_parse_capacity(const char *cap_str, uint64_t *cap, bool *has_prefix);

/**
 * Check if a buffer is all zero (0x00) bytes or not.
 *
 * \param data Buffer to check.
 * \param size Size of data in bytes.
 *
 * \return true if data consists entirely of zeroes, or false if any byte in data
 * is not zero.
 */
bool spdk_mem_all_zero(const void *data, size_t size);

/**
 * Convert the string in nptr to a long integer value according to the given base.
 *
 * spdk_strtol() does the additional error checking and allows only strings that
 * contains only numbers and is positive number or zero. The caller only has to check
 * if the return value is not negative.
 *
 * \param nptr String containing numbers.
 * \param base Base which must be between 2 and 32 inclusive, or be the special value 0.
 *
 * \return positive number or zero on success, or negative errno on failure.
 */
long int spdk_strtol(const char *nptr, int base);

/**
 * Convert the string in nptr to a long long integer value according to the given base.
 *
 * spdk_strtoll() does the additional error checking and allows only strings that
 * contains only numbers and is positive number or zero. The caller only has to check
 * if the return value is not negative.
 *
 * \param nptr String containing numbers.
 * \param base Base which must be between 2 and 32 inclusive, or be the special value 0.
 *
 * \return positive number or zero on success, or negative errno on failure.
 */
long long int spdk_strtoll(const char *nptr, int base);

/**
 * Build a NULL-terminated array of strings from the given string separated by
 * the given chars in delim, as if split by strpbrk(). Empty items are pointers
 * to an empty string.
 *
 * \param str Input string
 * \param delim Separating delimiter set.
 *
 * \return the string array, or NULL on failure.
 */
char **spdk_strarray_from_string(const char *str, const char *delim);

/**
 * Duplicate a NULL-terminated array of strings. Returns NULL on failure.
 * The array, and the strings, are allocated with the standard allocator (e.g.
 * calloc()).
 *
 * \param strarray input array of strings.
 */
char **spdk_strarray_dup(const char **strarray);

/**
 * Free a NULL-terminated array of strings. The array and its strings must have
 * been allocated with the standard allocator (calloc() etc.).
 *
 * \param strarray array of strings.
 */
void spdk_strarray_free(char **strarray);

/**
 * Copy a string into a fixed-size buffer with all occurrences of the search string
 * replaced with the given replace substring. The fixed-size buffer must not be less
 * than the string with the replaced values including the terminating null byte.
 *
 * \param dst Pointer to destination fixed-size buffer to fill.
 * \param size Size of the destination fixed-size buffer in bytes.
 * \param src Pointer to source null-terminated string to copy into dst.
 * \param search The string being searched for.
 * \param replace the replacement substring the replaces the found search substring.
 *
 * \return 0 on success, or negated errno on failure.
 */
int spdk_strcpy_replace(char *dst, size_t size, const char *src, const char *search,
			const char *replace);

#ifdef __cplusplus
}
#endif

#endif

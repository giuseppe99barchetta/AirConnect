#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

/* Comma-separated exact names; whitespace and case are ignored. */
static inline bool DeviceNameExcluded(const char *list, const char *name) {
	if (!list || !*list || !name) return false;
	while (*list) {
		while (*list == ',' || isspace((unsigned char) *list)) list++;
		const char *end = list;
		while (*end && *end != ',') end++;
		while (end > list && isspace((unsigned char) end[-1])) end--;
		if ((size_t) (end - list) == strlen(name) && !strncasecmp(list, name, end - list)) return true;
		list = *end ? end + 1 : end;
	}
	return false;
}

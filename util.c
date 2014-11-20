#ifndef _SANDBLAST_UTIL
#define _SANDBLAST_UTIL

#include <stdio.h>
#include <syslog.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <jansson.h>

#define json_assert_string(thing, name) \
	if (!json_is_string(thing)) \
		die("Incorrect JSON: %s must be a string", name); \

#define str_copy_from_json(target, parent, name) \
	if (1) { \
		json_t *result = json_object_get(parent, name); \
		json_assert_string(result, name); \
		target = copy_string(json_string_value(result)); \
	}

#define json_assert_array(thing, name) \
	if (!json_is_array(thing)) \
		die("Incorrect JSON: %s must be an array", name); \

#define arr_from_json(target, parent, name) \
	if (1) { \
		json_t *result = json_object_get(parent, name); \
		json_assert_array(result, name); \
		target = result; \
	}

#define json_assert_object(thing, name) \
	if (!json_is_object(thing)) \
		die("Incorrect JSON: %s must be an object", name); \

#define obj_from_json(target, parent, name) \
	if (1) { \
		json_t *result = json_object_get(parent, name); \
		json_assert_object(result, name) \
		target = result; \
	}

char *r_asprintf(const char *fmt, ...) {
	va_list vargs; va_start(vargs, fmt);
	char *result;
	vasprintf(&result, fmt, vargs);
	va_end(vargs);
	return result;
}

void s_log(const int priority, const char *fmt, ...) {
	va_list vargs; va_start(vargs, fmt);
	vsyslog(priority, fmt, vargs);
	va_end(vargs);
}

void s_log_errno(const int priority, const char *fmt, ...) {
	va_list vargs; va_start(vargs, fmt);
	char *e = r_asprintf("errno %d (%s)", errno, strerror(errno));
	char *m; vasprintf(&m, fmt, vargs); 
	va_end(vargs);
	s_log(priority, "%s: %s", e, m, NULL);
}

#define info(...) if (1) { s_log(LOG_INFO, __VA_ARGS__, NULL); }
#define die(...) if (1) { s_log(LOG_ERR, __VA_ARGS__, NULL); exit(1); }
#define die_errno(...) if (1) { s_log_errno(LOG_ERR, __VA_ARGS__, NULL); exit(errno); }

sem_t *init_shm_semaphore() {
	sem_t *result = (sem_t*) mmap(NULL, sizeof(*result), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (result == MAP_FAILED)
		die_errno("Could not mmap");
	sem_init(result, 1, 0);
	return result;
}

int *init_shm_int() {
	int *result = (int*) mmap(NULL, sizeof(*result), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (result == MAP_FAILED)
		die_errno("Could not mmap");
	*result = -1;
	return result;
}

struct in_addr parse_ipv4(const char *ip_str) {
	struct in_addr ip_struct;
	if (inet_pton(AF_INET, ip_str, &ip_struct) <= 0)
		die_errno("Cannot parse IPv4 address");
	return ip_struct;
}

char *copy_string(const char *original) {
	size_t len = strlen(original) + 1;
	char *result = malloc(len);
	strlcpy(result, original, len);
	return result;
}

// For names of environment variables
char *copy_uppercase_and_underscore(const char *original) {
	size_t len = strlen(original) + 1;
	char *result = malloc(len);
	for (size_t i = 0; i < len; i++) {
		if (original[i] >= 'a' && original[i] <= 'z')
			result[i] = original[i] - 32;
		else if (original[i] >= 'A' && original[i] <= 'Z')
			result[i] = original[i];
		else
			result[i] = '_';
	}
	result[len - 1] = '\0';
	return result;
}

// The jailname can't contain periods, unlike hostname, which often should
char *hostname_to_jailname(const char *original) {
	size_t len = strlen(original) + 1;
	char *result = malloc(len);
	for (size_t i = 0; i < len; i++)
		result[i] = (original[i] == '.') ? '_' : original[i];
	result[len - 1] = '\0';
	return result;
}


// str.replace('"', '\\"')
char* copy_escape_quotes(const char *original) {
	size_t qcount = 0;
	size_t len = strlen(original) + 1;
	for (size_t i = 0; i < len; i++)
		if (original[i] == '"')
			qcount++;
	char* result = malloc(len + qcount + 1);
	size_t r = 0;
	for (size_t i = 0; i < len; i++) {
		if (original[i] == '"') {
			result[r] = '\\';
			r++;
		}
		result[r] = original[i];
		r++;
	}
	result[r] = '\0';
	return result;
}

#endif

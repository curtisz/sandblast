// The configuration parser runs libucl in a forked process in a sandbox
// (rlimit, Capsicum capability mode)
// Because shared memory can't be passed from the forked process to the parent,
// a large chunk of shared memory is allocated as an arena for the processes to share.

#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/capsicum.h>
#include <ucl.h>
#include "logging.h"
#include "memory.h"
#include "config.h"

#define BUF_SIZE 65536
#define IPV4_ADDRS_LEN 32
#define IPV6_ADDRS_LEN 32

static struct rlimit cpu_lim = { 2, 2 };

static char *ipv4_addrs[IPV4_ADDRS_LEN];
static char *ipv6_addrs[IPV6_ADDRS_LEN];
static void *config_parser_arena;

#define STR_TO_ARENA(dst, src) if (1) { \
	size_t size = strlen((src)) + 1; \
	dst = arena_alloc(config_parser_arena, size); \
	if (strlcpy(dst, (src), size) >= size) die("Config string too long"); }

typedef void (^ucl_callback)(ucl_object_t*);

void ucl_iterate(ucl_object_t *obj, bool expand, ucl_callback cb) {
	ucl_object_iter_t it = ucl_object_iterate_new(obj);
	const ucl_object_t *val;
	while ((val = ucl_object_iterate_safe(it, expand)) != NULL)
		cb(val);
	ucl_object_iterate_free(it);
}

// The jailname can't contain periods, unlike hostname, which often should
char *hostname_to_jailname(const char *str) {
	size_t len = strlen(str) + 1;
	char *result = malloc(len);
	for (size_t i = 0; i < len; i++)
		result[i] = (str[i] == '.') ? '_' : str[i];
	result[len - 1] = '\0';
	return result;
}

void parse_conf(jail_conf_t *jail_conf, uint8_t *buf, size_t len) {
	struct ucl_parser *parser = ucl_parser_new(UCL_PARSER_NO_TIME);
	ucl_parser_add_chunk(parser, buf, len);
	if (ucl_parser_get_error(parser))
		die("Config: Could not parse"); // TODO: output the error
	ucl_object_t *root = ucl_parser_get_object(parser);

	bzero(ipv4_addrs, IPV4_ADDRS_LEN*sizeof(char*));
	bzero(ipv6_addrs, IPV6_ADDRS_LEN*sizeof(char*));
	bzero(jail_conf->limits, sizeof(jail_conf->limits));
	bzero(jail_conf->mounts, sizeof(jail_conf->mounts));

	ucl_iterate(root, true, ^(ucl_object_t *cur) {
		char *key = ucl_object_key(cur);

		if (strcmp(key, "hostname") == 0) {
			STR_TO_ARENA(jail_conf->hostname, ucl_object_tostring_forced(cur));
		} else if (strcmp(key, "jailname") == 0) {
			STR_TO_ARENA(jail_conf->jailname, ucl_object_tostring_forced(cur));
		} else if (strcmp(key, "script") == 0) {
			STR_TO_ARENA(jail_conf->script, ucl_object_tostring_forced(cur));
		} else if (strcmp(key, "ipv4") == 0) {
			__block size_t i = 0;
			ucl_iterate(cur, false, ^(ucl_object_t *val) {
				if (i > IPV4_ADDRS_LEN)
					die("Config: Too many IPv4 addresses");
				ipv4_addrs[i++] = ucl_object_tostring_forced(val);
			});
		} else if (strcmp(key, "ipv6") == 0) {
			__block size_t i = 0;
			ucl_iterate(cur, false, ^(ucl_object_t *val) {
				if (i > IPV6_ADDRS_LEN)
					die("Config: Too many IPv6 addresses");
				ipv6_addrs[i++] = ucl_object_tostring_forced(val);
			});
		} else if (strcmp(key, "resources") == 0) {
			__block size_t i = 0;
			ucl_iterate(cur, true, ^(ucl_object_t *val) {
				if (i > LIMITS_LEN)
					die("Config: Too many limits");
				char *res_key = ucl_object_key(val);
				for (size_t j = 0; j < strlen(res_key); j++)
					if (res_key[j] < 'A' || (res_key[j] > 'Z' && res_key[j] < 'a') || res_key[j] > 'z')
						die("Config: Character '%c' in resource name '%s' isn't allowed", res_key[j], res_key);
				int64_t int_val = -1;
				if (ucl_object_toint_safe(val, &int_val) != true)
					die("Config: Limit for resource '%s' is not a number", res_key);
				if (int_val < 0)
					die("Config: Limit for resource '%s' is less than zero", res_key);
				char *lim_str = arena_alloc(config_parser_arena, 32);
				snprintf(lim_str, 32, "%s:deny=%ld", res_key, int_val);
				jail_conf->limits[i++] = lim_str;
			});

		}

	});

	if (jail_conf->script == NULL)
		die("Config: You forgot to specify the `script`");

	deduplicate_strings(ipv4_addrs, IPV4_ADDRS_LEN);
	char *ip4joined = join_strings(ipv4_addrs, IPV4_ADDRS_LEN, ',');
	if (strlen(ip4joined) > 0)
		STR_TO_ARENA(jail_conf->ipv4, ip4joined);
	free(ip4joined);

	deduplicate_strings(ipv6_addrs, IPV6_ADDRS_LEN);
	char *ip6joined = join_strings(ipv6_addrs, IPV6_ADDRS_LEN, ',');
	if (strlen(ip6joined) > 0)
		STR_TO_ARENA(jail_conf->ipv6, ip6joined);
	free(ip6joined);

	if (jail_conf->jailname == NULL && jail_conf->hostname != NULL)
		jail_conf->jailname = hostname_to_jailname(jail_conf->hostname);

	ucl_parser_free(parser);
	ucl_object_unref(root);
}

size_t read_conf(FILE *file, uint8_t *buf) {
	size_t len = 0;
	while (!feof(file) && len < BUF_SIZE) {
		len += fread(buf + len, 1, BUF_SIZE - len, file);
	}
	if (!feof(file)) {
		fclose(file);
		die("Config file too long");
	}
	fclose(file);
	return len;
}

void enter_sandbox() {
	if (setrlimit(RLIMIT_CPU, &cpu_lim) != 0)
		die_errno("Could not limit CPU for the config parser")
	if (cap_enter() != 0)
		die_errno("Could not enter capability mode for the config parser");
}

jail_conf_t *load_conf(char *filename) {
	config_parser_arena = init_shm_arena(640 * 1024); // Should be enough for anyone ;-)
	jail_conf_t *jail_conf = (jail_conf_t*)arena_alloc(config_parser_arena, sizeof(jail_conf_t));
	pid_t child_pid = fork();
	if (child_pid == -1)
		die_errno("Could not fork");
	if (child_pid <= 0) { // Child
		if (seteuid(getuid()) != 0)
			die_errno("Could not seteuid for the config parser");
		FILE *conf_file = fopen(filename, "r");
		if (conf_file == NULL)
			die_errno("Could not read the config file");
		enter_sandbox();
		uint8_t conf_buf[BUF_SIZE];
		size_t conf_len = read_conf(conf_file, conf_buf);
		parse_conf(jail_conf, conf_buf, conf_len);
		exit(0);
	} else { // Parent
		int status; waitpid(child_pid, &status, 0);
		if (status != 0)
			die("The parser process exited unsuccessfully");
	}
	return jail_conf;
}

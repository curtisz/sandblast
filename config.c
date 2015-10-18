// The configuration parser runs libucl in a forked process in capability mode (Capsicum.)
// Because shared memory can't be passed from the forked process to the parent,
// a large chunk of shared memory is allocated as an arena for the processes to share.

#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/capsicum.h>
#include <ucl.h>
#include "logging.h"
#include "memory.h"
#include "config.h"

#define BUF_SIZE 65536
#define IPV4_ADDRS_LEN 32
#define IPV6_ADDRS_LEN 32

static char *ipv4_addrs[IPV4_ADDRS_LEN];
static char *ipv6_addrs[IPV6_ADDRS_LEN];
static void *config_parser_arena;

#define STR_TO_ARENA(dst, src) if (1) { \
	size_t size = strlen((src)) + 1; \
	dst = arena_alloc(config_parser_arena, size); \
	strlcpy(dst, (src), size); }

typedef void (^ucl_callback)(ucl_object_t*);

void ucl_iterate(ucl_object_t *obj, bool expand, ucl_callback cb) {
	ucl_object_iter_t it = ucl_object_iterate_new(obj);
	const ucl_object_t *val;
	while ((val = ucl_object_iterate_safe(it, expand)) != NULL)
		cb(val);
	ucl_object_iterate_free(it);
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

void read_conf(jail_conf_t *jail_conf, uint8_t *inbuf, size_t len) {
	// Runs in the sandbox!
	struct ucl_parser *parser = ucl_parser_new(UCL_PARSER_NO_TIME);
	ucl_parser_add_chunk(parser, inbuf, len);
	if (ucl_parser_get_error(parser))
		die("Could not parse the config"); // TODO: output the error
	ucl_object_t *root = ucl_parser_get_object(parser);

	bzero(ipv4_addrs, IPV4_ADDRS_LEN*sizeof(char*));
	bzero(ipv6_addrs, IPV6_ADDRS_LEN*sizeof(char*));

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
					die("Too many IPv4 addresses in config");
				ipv4_addrs[i++] = ucl_object_tostring_forced(val);
			});
		} else if (strcmp(key, "ipv6") == 0) {
			__block size_t i = 0;
			ucl_iterate(cur, false, ^(ucl_object_t *val) {
				if (i > IPV6_ADDRS_LEN)
					die("Too many IPv6 addresses in config");
				ipv6_addrs[i++] = ucl_object_tostring_forced(val);
			});
		}

	});

	if (jail_conf->script == NULL)
		die("You forgot to specify the `script` in config");

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

jail_conf_t *parse_config(char *filename) {
	config_parser_arena = init_shm_arena(640 * 1024); // Should be enough for anyone ;-)
	jail_conf_t *jail_conf = (jail_conf_t*)arena_alloc(config_parser_arena, sizeof(jail_conf_t));
	jail_conf->mounts = (mount_t**)arena_alloc(config_parser_arena, MOUNTS_LEN*sizeof(mount_t*));
	pid_t child_pid = fork();
	if (child_pid == -1)
		die_errno("Could not fork");
	if (child_pid > 0) { // Parent
		int status; waitpid(child_pid, &status, 0);
		if (status != 0)
			die("The parser process exited unsuccessfully");
	} else { // Child
		FILE *conf_file = fopen(filename, "r");
		if (conf_file == NULL)
			die_errno("Could not read the config file");
		if (cap_enter() < 0)
			die_errno("Could not enter capability mode");
		size_t r = 0;
		uint8_t inbuf[BUF_SIZE];
		while (!feof(conf_file) && r < sizeof(inbuf))
			r += fread(inbuf + r, 1, sizeof(inbuf) - r, conf_file);
		if (!feof(conf_file)) {
			fclose(conf_file);
			die("Config file too long");
		}
		fclose(conf_file);
		read_conf(jail_conf, inbuf, r);
		exit(0);
	}
	return jail_conf;
}

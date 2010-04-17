/*
    conf.c -- configuration code
    Copyright (C) 1998 Robert van der Meulen
                  1998-2005 Ivo Timmermans
                  2000-2009 Guus Sliepen <guus@tinc-vpn.org>
		  2000 Cris van Pelt

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include "avl_tree.h"
#include "conf.h"
#include "logger.h"
#include "netutl.h"				/* for str2address */
#include "protocol.h"
#include "utils.h"				/* for cp */
#include "xalloc.h"

avl_tree_t *config_tree;

int pinginterval = 0;			/* seconds between pings */
int pingtimeout = 0;			/* seconds to wait for response */
char *confbase = NULL;			/* directory in which all config files are */
char *netname = NULL;			/* name of the vpn network */

static int config_compare(const config_t *a, const config_t *b) {
	int result;

	result = strcasecmp(a->variable, b->variable);

	if(result)
		return result;

	result = a->line - b->line;

	if(result)
		return result;
	else
		return strcmp(a->file, b->file);
}

void init_configuration(avl_tree_t ** config_tree) {
	*config_tree = avl_alloc_tree((avl_compare_t) config_compare, (avl_action_t) free_config);
}

void exit_configuration(avl_tree_t ** config_tree) {
	avl_delete_tree(*config_tree);
	*config_tree = NULL;
}

config_t *new_config(void) {
	return xmalloc_and_zero(sizeof(config_t));
}

void free_config(config_t *cfg) {
	if(cfg->variable)
		free(cfg->variable);

	if(cfg->value)
		free(cfg->value);

	if(cfg->file)
		free(cfg->file);

	free(cfg);
}

void config_add(avl_tree_t *config_tree, config_t *cfg) {
	avl_insert(config_tree, cfg);
}

config_t *lookup_config(avl_tree_t *config_tree, char *variable) {
	config_t cfg, *found;

	cfg.variable = variable;
	cfg.file = "";
	cfg.line = 0;

	found = avl_search_closest_greater(config_tree, &cfg);

	if(!found)
		return NULL;

	if(strcasecmp(found->variable, variable))
		return NULL;

	return found;
}

config_t *lookup_config_next(avl_tree_t *config_tree, const config_t *cfg) {
	avl_node_t *node;
	config_t *found;

	node = avl_search_node(config_tree, cfg);

	if(node) {
		if(node->next) {
			found = node->next->data;

			if(!strcasecmp(found->variable, cfg->variable))
				return found;
		}
	}

	return NULL;
}

bool get_config_bool(const config_t *cfg, bool *result) {
	if(!cfg)
		return false;

	if(!strcasecmp(cfg->value, "yes")) {
		*result = true;
		return true;
	} else if(!strcasecmp(cfg->value, "no")) {
		*result = false;
		return true;
	}

	logger(LOG_ERR, "\"yes\" or \"no\" expected for configuration variable %s in %s line %d",
		   cfg->variable, cfg->file, cfg->line);

	return false;
}

bool get_config_int(const config_t *cfg, int *result) {
	if(!cfg)
		return false;

	if(sscanf(cfg->value, "%d", result) == 1)
		return true;

	logger(LOG_ERR, "Integer expected for configuration variable %s in %s line %d",
		   cfg->variable, cfg->file, cfg->line);

	return false;
}

bool get_config_string(const config_t *cfg, char **result) {
	if(!cfg)
		return false;

	*result = xstrdup(cfg->value);

	return true;
}

bool get_config_address(const config_t *cfg, struct addrinfo **result) {
	struct addrinfo *ai;

	if(!cfg)
		return false;

	ai = str2addrinfo(cfg->value, NULL, 0);

	if(ai) {
		*result = ai;
		return true;
	}

	logger(LOG_ERR, "Hostname or IP address expected for configuration variable %s in %s line %d",
		   cfg->variable, cfg->file, cfg->line);

	return false;
}

bool get_config_subnet(const config_t *cfg, subnet_t ** result) {
	subnet_t subnet = {0};

	if(!cfg)
		return false;

	if(!str2net(&subnet, cfg->value)) {
		logger(LOG_ERR, "Subnet expected for configuration variable %s in %s line %d",
			   cfg->variable, cfg->file, cfg->line);
		return false;
	}

	/* Teach newbies what subnets are... */

	if(((subnet.type == SUBNET_IPV4)
		&& !maskcheck(&subnet.net.ipv4.address, subnet.net.ipv4.prefixlength, sizeof(ipv4_t)))
		|| ((subnet.type == SUBNET_IPV6)
		&& !maskcheck(&subnet.net.ipv6.address, subnet.net.ipv6.prefixlength, sizeof(ipv6_t)))) {
		logger(LOG_ERR, "Network address and prefix length do not match for configuration variable %s in %s line %d",
			   cfg->variable, cfg->file, cfg->line);
		return false;
	}

	*(*result = new_subnet()) = subnet;

	return true;
}

/*
  Read exactly one line and strip the trailing newline if any.
*/
static char *readline(FILE * fp, char *buf, size_t buflen) {
	char *newline = NULL;
	char *p;

	if(feof(fp))
		return NULL;

	p = fgets(buf, buflen, fp);

	if(!p)
		return NULL;

	newline = strchr(p, '\n');

	if(!newline)
		return buf;

	*newline = '\0';	/* kill newline */
	if(newline > p && newline[-1] == '\r')	/* and carriage return if necessary */
		newline[-1] = '\0';

	return buf;
}

/*
  Parse a configuration file and put the results in the configuration tree
  starting at *base.
*/
bool read_config_file(avl_tree_t *config_tree, const char *fname) {
	FILE *fp;
	char buffer[MAX_STRING_SIZE];
	char *line;
	char *variable, *value, *eol;
	int lineno = 0;
	int len;
	bool ignore = false;
	config_t *cfg;
	bool result = false;

	fp = fopen(fname, "r");

	if(!fp) {
		logger(LOG_ERR, "Cannot open config file %s: %s", fname, strerror(errno));
		return false;
	}

	for(;;) {
		line = readline(fp, buffer, sizeof buffer);

		if(!line) {
			if(feof(fp))
				result = true;
			break;
		}

		lineno++;

		if(!*line || *line == '#')
			continue;

		if(ignore) {
			if(!strncmp(line, "-----END", 8))
				ignore = false;
			continue;
		}
		
		if(!strncmp(line, "-----BEGIN", 10)) {
			ignore = true;
			continue;
		}

		variable = value = line;

		eol = line + strlen(line);
		while(strchr("\t ", *--eol))
			*eol = '\0';

		len = strcspn(value, "\t =");
		value += len;
		value += strspn(value, "\t ");
		if(*value == '=') {
			value++;
			value += strspn(value, "\t ");
		}
		variable[len] = '\0';

	
		if(!*value) {
			logger(LOG_ERR, "No value for variable `%s' on line %d while reading config file %s",
				   variable, lineno, fname);
			break;
		}

		cfg = new_config();
		cfg->variable = xstrdup(variable);
		cfg->value = xstrdup(value);
		cfg->file = xstrdup(fname);
		cfg->line = lineno;

		config_add(config_tree, cfg);
	}

	fclose(fp);

	return result;
}

bool read_server_config() {
	char *fname;
	bool x;

	xasprintf(&fname, "%s/tinc.conf", confbase);
	x = read_config_file(config_tree, fname);

	if(!x) {				/* System error: complain */
		logger(LOG_ERR, "Failed to read `%s': %s", fname, strerror(errno));
	}

	free(fname);

	return x;
}

FILE *ask_and_open(const char *filename, const char *what) {
	FILE *r;
	char *directory;
	char line[PATH_MAX];
	const char *fn;

	/* Check stdin and stdout */
	if(!isatty(0) || !isatty(1)) {
		/* Argh, they are running us from a script or something.  Write
		   the files to the current directory and let them burn in hell
		   for ever. */
		fn = filename;
	} else {
		/* Ask for a file and/or directory name. */
		fprintf(stdout, "Please enter a file to save %s to [%s]: ",
				what, filename);
		fflush(stdout);

		fn = readline(stdin, line, sizeof line);

		if(!fn) {
			fprintf(stderr, "Error while reading stdin: %s\n",
					strerror(errno));
			return NULL;
		}

		if(!strlen(fn))
			/* User just pressed enter. */
			fn = filename;
	}

#ifdef HAVE_MINGW
	if(fn[0] != '\\' && fn[0] != '/' && !strchr(fn, ':')) {
#else
	if(fn[0] != '/') {
#endif
		/* The directory is a relative path or a filename. */
		char *p;

		directory = get_current_dir_name();
		xasprintf(&p, "%s/%s", directory, fn);
		free(directory);
		fn = p;
	}

	umask(0077);				/* Disallow everything for group and other */

	/* Open it first to keep the inode busy */

	r = fopen(fn, "r+") ?: fopen(fn, "w+");

	if(!r) {
		fprintf(stderr, "Error opening file `%s': %s\n",
				fn, strerror(errno));
		return NULL;
	}

	return r;
}

bool disable_old_keys(FILE *f) {
	char buf[100];
	long pos;
	bool disabled = false;

	rewind(f);
	pos = ftell(f);

	while(fgets(buf, sizeof buf, f)) {
		if(!strncmp(buf, "-----BEGIN RSA", 14)) {	
			buf[11] = 'O';
			buf[12] = 'L';
			buf[13] = 'D';
			fseek(f, pos, SEEK_SET);
			fputs(buf, f);
			disabled = true;
		}
		else if(!strncmp(buf, "-----END RSA", 12)) {	
			buf[ 9] = 'O';
			buf[10] = 'L';
			buf[11] = 'D';
			fseek(f, pos, SEEK_SET);
			fputs(buf, f);
			disabled = true;
		}
		pos = ftell(f);
	}

	return disabled;
}

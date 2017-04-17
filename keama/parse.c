/*
 * Copyright (c) 2017 by Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *   Internet Systems Consortium, Inc.
 *   950 Charter Street
 *   Redwood City, CA 94063
 *   <info@isc.org>
 *   https://www.isc.org/
 *
 */

#include "keama.h"

#include <sys/types.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

static void config_valid_lifetime(struct element *, struct parse *);
static void config_file(struct element *, struct parse *);
static void config_sname(struct element *, struct parse *);
static void config_next_server(struct element *, struct parse *);
static void config_qualifying_suffix(struct element *, struct parse *);
static void config_enable_updates(struct element *, struct parse *);
static void config_local_address(struct element *, struct parse *);
static void config_ddns_update_style(struct element *, struct parse *);
static void config_preferred_lifetime(struct element *, struct parse *);
static void config_match_client_id(struct element *, struct parse *);
static void config_echo_client_id(struct element *, struct parse *);
static void config_timers(struct element *, struct parse *);
static void config_expired_leases_processing(struct element *, struct parse *);

/*
static uint32_t getULong(const unsigned char *buf);
static int32_t getLong(const unsigned char *buf);
static uint32_t getUShort(const unsigned char *buf);
static int32_t getShort(const unsigned char *buf);
static uint32_t getUChar(const unsigned char *obuf);
*/
static void putULong(unsigned char *obuf, uint32_t val);
static void putLong(unsigned char *obuf, int32_t val);
static void putUShort(unsigned char *obuf, uint32_t val);
static void putShort(unsigned char *obuf, int32_t val);
/*
static void putUChar(unsigned char *obuf, uint32_t val);
*/

/*
static isc_boolean_t is_compound_expression(struct element *);
*/
static enum expression_context op_context(enum expr_op);
static int op_precedence(enum expr_op, enum expr_op);
static enum expression_context expression_context(struct element *);

/* Skip to the semicolon ending the current statement.   If we encounter
   braces, the matching closing brace terminates the statement.
*/
void
skip_to_semi(struct parse *cfile)
{
	skip_to_rbrace(cfile, 0);
}

/* Skips everything from the current point upto (and including) the given
 number of right braces.  If we encounter a semicolon but haven't seen a
 left brace, consume it and return.
 This lets us skip over:

   	statement;
	statement foo bar { }
	statement foo bar { statement { } }
	statement}
 
	...et cetera. */
void
skip_to_rbrace(struct parse *cfile, int brace_count)
{
	enum dhcp_token token;
	const char *val;

	do {
		token = peek_token(&val, NULL, cfile);
		if (token == RBRACE) {
			if (brace_count > 0) {
				--brace_count;
			}

			if (brace_count == 0) {
				/* Eat the brace and return. */
				skip_token(&val, NULL, cfile);
				return;
			}
		} else if (token == LBRACE) {
			brace_count++;
		} else if (token == SEMI && (brace_count == 0)) {
			/* Eat the semicolon and return. */
			skip_token(&val, NULL, cfile);
			return;
		} else if (token == EOL) {
			/* EOL only happens when parsing /etc/resolv.conf,
			   and we treat it like a semicolon because the
			   resolv.conf file is line-oriented. */
			skip_token(&val, NULL, cfile);
			return;
		}

		/* Eat the current token */
		token = next_token(&val, NULL, cfile);
	} while (token != END_OF_FILE);
}

void
parse_semi(struct parse *cfile)
{
	enum dhcp_token token;
	const char *val;

	token = next_token(&val, NULL, cfile);
	if (token != SEMI)
		parse_error(cfile, "semicolon expected.");
}

/* string-parameter :== STRING SEMI */

void
parse_string(struct parse *cfile, char **sptr, unsigned *lptr)
{
	const char *val;
	enum dhcp_token token;
	char *s;
	unsigned len;

	token = next_token(&val, &len, cfile);
	if (token != STRING)
		parse_error(cfile, "expecting a string");
	s = (char *)malloc(len + 1);
	parse_error(cfile, "no memory for string %s.", val);
	memcpy(s, val, len + 1);

	parse_semi(cfile);
	if (sptr)
		*sptr = s;
	else
		free(s);
	if (lptr)
		*lptr = len;
}

/*
 * hostname :== IDENTIFIER
 *		| IDENTIFIER DOT
 *		| hostname DOT IDENTIFIER
 */

struct string *
parse_host_name(struct parse *cfile)
{
	const char *val;
	enum dhcp_token token;
	struct string *s = NULL;
	
	/* Read a dotted hostname... */
	do {
		/* Read a token, which should be an identifier. */
		token = peek_token(&val, NULL, cfile);
		if (!is_identifier(token) && token != NUMBER)
			break;
		skip_token(&val, NULL, cfile);

		/* Store this identifier... */
		if (s == NULL)
			s = makeString(-1, val);
		else
			appendString(s, val);
		/* Look for a dot; if it's there, keep going, otherwise
		   we're done. */
		token = peek_token(&val, NULL, cfile);
		if (token == DOT) {
			token = next_token(&val, NULL, cfile);
			appendString(s, val);
		}
	} while (token == DOT);

	return s;
}

/* ip-addr-or-hostname :== ip-address | hostname
   ip-address :== NUMBER DOT NUMBER DOT NUMBER DOT NUMBER
   
   Parse an ip address or a hostname.

   Note that RFC1123 permits hostnames to consist of all digits,
   making it difficult to quickly disambiguate them from ip addresses.
*/

struct string *
parse_ip_addr_or_hostname(struct parse *cfile, isc_boolean_t check_multi)
{
	const char *val;
	enum dhcp_token token;
	unsigned char addr[4];
	unsigned len = sizeof(addr);
	isc_boolean_t ipaddr = ISC_FALSE;
	struct string *bin = NULL;
	char buf[80];

	token = peek_token(&val, NULL, cfile);
	if (token == NUMBER) {
		/*
		 * a hostname may be numeric, but domain names must
		 * start with a letter, so we can disambiguate by
		 * looking ahead a few tokens.  we save the parse
		 * context first, and restore it after we know what
		 * we're dealing with.
		 */
		save_parse_state(cfile);
		skip_token(NULL, NULL, cfile);
		if (next_token(NULL, NULL, cfile) == DOT &&
		    next_token(NULL, NULL, cfile) == NUMBER)
			ipaddr = ISC_TRUE;
		restore_parse_state(cfile);

		if (ipaddr)
			bin = parse_numeric_aggregate(cfile, addr, &len,
						       DOT, 10, 8);
	}

	if ((bin == NULL) && (is_identifier(token) || token == NUMBER)) {
	  	struct string *name;
		struct hostent *h;

		name = parse_host_name(cfile);
		if (name == NULL)
			return NULL;

		/* from do_host_lookup */
		h = gethostbyname(name->content);
		if ((h == NULL) || (h->h_addr_list[0] == NULL))
			parse_error(cfile, "%s: host unknown.", name->content);
		if (check_multi && h->h_addr_list[1]) {
			struct comment *comment;
			char msg[128];

			snprintf(msg, sizeof(msg),
				 "/// %s resolves into multiple addresses",
				name->content);
			comment = createComment(msg);
			TAILQ_INSERT_TAIL(&cfile->comments, comment);
		}
		bin = makeString(4, h->h_addr_list[0]);
	}

	if (bin == NULL) {
		if (token != RBRACE && token != LBRACE)
			token = next_token(&val, NULL, cfile);
		parse_error(cfile, "%s (%d): expecting IP address or hostname",
			    val, token);
	}

	memset(buf, 0, sizeof(buf));
	if (!inet_ntop(AF_INET, bin->content, buf, sizeof(buf)))
		parse_error(cfile, "can't print IP address");                 
        return makeString(-1, buf);
}
	
/*
 * ip-address :== NUMBER DOT NUMBER DOT NUMBER DOT NUMBER
 */

struct string *
parse_ip_addr(struct parse *cfile)
{
	unsigned char addr[4];
	unsigned len = sizeof(addr);

	return parse_numeric_aggregate(cfile, addr, &len, DOT, 10, 8);
}	

/*
 * Return true if every character in the string is hexadecimal.
 */
static isc_boolean_t
is_hex_string(const char *s)
{
	while (*s != '\0') {
		if (!isxdigit((int)*s)) {
			return ISC_FALSE;
		}
		s++;
	}
	return ISC_TRUE;
}

/*
 * ip-address6 :== (complicated set of rules)
 *
 * See section 2.2 of RFC 1884 for details.
 *
 * We are lazy for this. We pull numbers, names, colons, and dots 
 * together and then throw the resulting string at the inet_pton()
 * function.
 */

struct string *
parse_ip6_addr(struct parse *cfile)
{
	enum dhcp_token token;
	const char *val;
	char addr[16];
	int val_len;
	char v6[sizeof("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255")];
	int v6_len;

	/*
	 * First token is non-raw. This way we eat any whitespace before 
	 * our IPv6 address begins, like one would expect.
	 */
	token = peek_token(&val, NULL, cfile);

	/*
	 * Gather symbols.
	 */
	v6_len = 0;
	for (;;) {
		if ((((token == NAME) || (token == NUMBER_OR_NAME)) &&
		     is_hex_string(val)) ||
		    (token == NUMBER) ||
		    (token == TOKEN_ADD) ||
		    (token == DOT) ||
		    (token == COLON)) {

			next_raw_token(&val, NULL, cfile);
			val_len = strlen(val);
			if ((v6_len + val_len) >= sizeof(v6))
				parse_error(cfile, "Invalid IPv6 address.");
			memcpy(v6+v6_len, val, val_len);
			v6_len += val_len;

		} else {
			break;
		}
		token = peek_raw_token(&val, NULL, cfile);
	}
	v6[v6_len] = '\0';

	/*
	 * Use inet_pton() for actual work.
	 */
	if (inet_pton(AF_INET6, v6, addr) <= 0)
		parse_error(cfile, "Invalid IPv6 address.");
	return makeString(16, addr);
}

/*
 * Same as parse_ip6_addr() above, but returns the value as a text
 * rather than in an address binary structure.
 */
struct string *
parse_ip6_addr_txt(struct parse *cfile)
{
	const struct string *bin;
	char buf[80];

	bin = parse_ip6_addr(cfile);
	memset(buf, 0, sizeof(buf));
	if (!inet_ntop(AF_INET6, bin->content, buf, sizeof(buf)))
		parse_error(cfile, "can't print IPv6 address");
	return makeString(-1, buf);
}

/*
 * hardware-parameter :== HARDWARE hardware-type colon-separated-hex-list SEMI
 * hardware-type :== ETHERNET | TOKEN_RING | TOKEN_FDDI | INFINIBAND
 * Note that INFINIBAND may not be useful for some items, such as classification
 * as the hardware address won't always be available.
 */

struct element *
parse_hardware_param(struct parse *cfile)
{
	const char *val;
	enum dhcp_token token;
	isc_boolean_t ether = ISC_FALSE;
	unsigned hlen, i;
	struct string *t, *r;
	struct element *hw;
	char buf[HARDWARE_ADDR_LEN * 4];

	token = next_token(&val, NULL, cfile);
	if (token == ETHERNET)
		ether = ISC_TRUE;
	else {
		r = makeString(-1, val);
		appendString(r, " ");
	}

	/* Parse the hardware address information.   Technically,
	   it would make a lot of sense to restrict the length of the
	   data we'll accept here to the length of a particular hardware
	   address type.   Unfortunately, there are some broken clients
	   out there that put bogus data in the chaddr buffer, and we accept
	   that data in the lease file rather than simply failing on such
	   clients.   Yuck. */
	hlen = 0;
	token = peek_token(&val, NULL, cfile);
	if (token == SEMI)
		parse_error(cfile, "empty hardware address");
	t = parse_numeric_aggregate(cfile, NULL, &hlen, COLON, 16, 8);
	if (t == NULL)
		parse_error(cfile, "can't get hardware address");
	if (hlen > HARDWARE_ADDR_LEN)
		parse_error(cfile, "hardware address too long");
	token = next_token(&val, NULL, cfile);
	if (token != SEMI)
		parse_error(cfile, "expecting semicolon.");

	memset(buf, 0, sizeof(buf));
	for (i = 0; i < hlen; ++i) {
		size_t used;
		if (i == 0) {
			(void)snprintf(buf, sizeof(buf),
				       "%02hhx", t->content[i]);
			continue;
		}
		used = strlen(buf);
		(void)snprintf(buf + used, sizeof(buf) - used,
			       ":%02hhx", t->content[i]);
	}
	if (ether)
		r = makeString(-1, buf);
	else
		appendString(r, buf);
	hw = createString(r);
	TAILQ_CONCAT(&hw->comments, &cfile->comments);
	if (!ether || (hlen != 6)) {
		hw->skip = ISC_TRUE;
		cfile->issue_counter++;
	}
	return hw;
}

/* No BNF for numeric aggregates - that's defined by the caller.  What
   this function does is to parse a sequence of numbers separated by
   the token specified in separator.  If max is zero, any number of
   numbers will be parsed; otherwise, exactly max numbers are
   expected.  Base and size tell us how to internalize the numbers
   once they've been tokenized.

   buf - A pointer to space to return the parsed value, if it is null
   then the function will allocate space for the return.

   max - The maximum number of items to store.  If zero there is no
   maximum.  When buf is null and the function needs to allocate space
   it will do an allocation of max size at the beginning if max is non
   zero.  If max is zero then the allocation will be done later, after
   the function has determined the size necessary for the incoming
   string.

   returns NULL on errors or a pointer to the string structure on success.
 */

struct string *
parse_numeric_aggregate(struct parse *cfile, unsigned char *buf,
			unsigned *max, int separator,
			int base, unsigned size)
{
	const char *val;
	enum dhcp_token token;
	unsigned char *bufp = buf, *s;
	unsigned count = 0;
	struct string *r = NULL, *t;

	if (!bufp && *max) {
		bufp = (unsigned char *)malloc(*max * size / 8);
		if (!bufp)
			parse_error(cfile, "no space for numeric aggregate");
	}
	s = bufp;
	if (!s) {
		r = makeString(0, NULL);
		t = makeString(size / 8, "bigger than needed");
	}

	do {
		if (count) {
			token = peek_token(&val, NULL, cfile);
			if (token != separator) {
				if (!*max)
					break;
				if (token != RBRACE && token != LBRACE)
					token = next_token(&val, NULL, cfile);
				parse_error(cfile, "too few numbers.");
			}
			skip_token(&val, NULL, cfile);
		}
		token = next_token(&val, NULL, cfile);

		if (token == END_OF_FILE)
			parse_error(cfile, "unexpected end of file");

		/* Allow NUMBER_OR_NAME if base is 16. */
		if (token != NUMBER &&
		    (base != 16 || token != NUMBER_OR_NAME))
			parse_error(cfile, "expecting numeric value.");
		/* If we can, convert the number now; otherwise, build
		   a linked list of all the numbers. */
		if (s) {
			convert_num(cfile, s, val, base, size);
			s += size / 8;
		} else {
			convert_num(cfile, (unsigned char *)t->content,
				    val, base, size);
			concatString(r, t);
		}
	} while (++count != *max);

	*max = count;
	if (bufp)
		r = makeString(count * size / 8, (char *)bufp);

	return r;
}

void
convert_num(struct parse *cfile, unsigned char *buf, const char *str,
	    int base, unsigned size)
{
	const unsigned char *ptr = (const unsigned char *)str;
	int negative = 0;
	uint32_t val = 0;
	int tval;
	int max;

	if (*ptr == '-') {
		negative = 1;
		++ptr;
	}

	/* If base wasn't specified, figure it out from the data. */
	if (!base) {
		if (ptr[0] == '0') {
			if (ptr[1] == 'x') {
				base = 16;
				ptr += 2;
			} else if (isascii(ptr[1]) && isdigit(ptr[1])) {
				base = 8;
				ptr += 1;
			} else {
				base = 10;
			}
		} else {
			base = 10;
		}
	}

	do {
		tval = *ptr++;
		/* XXX assumes ASCII... */
		if (tval >= 'a')
			tval = tval - 'a' + 10;
		else if (tval >= 'A')
			tval = tval - 'A' + 10;
		else if (tval >= '0')
			tval -= '0';
		else
			parse_error(cfile, "Bogus number: %s.", str);
		if (tval >= base)
			parse_error(cfile,
				    "Bogus number %s: digit %d not in base %d",
				    str, tval, base);
		val = val * base + tval;
	} while (*ptr);

	if (negative)
		max = (1 << (size - 1));
	else
		max = (1 << (size - 1)) + ((1 << (size - 1)) - 1);
	if (val > max) {
		switch (base) {
		case 8:
			parse_error(cfile,
				    "%s%lo exceeds max (%d) for precision.",
				    negative ? "-" : "",
				    (unsigned long)val, max);
			break;
		case 16:
			parse_error(cfile,
				    "%s%lx exceeds max (%d) for precision.",
				    negative ? "-" : "",
				    (unsigned long)val, max);
			break;
		default:
			parse_error(cfile,
				    "%s%lu exceeds max (%d) for precision.",
				    negative ? "-" : "",
				    (unsigned long)val, max);
			break;
		}
	}

	if (negative) {
		switch (size) {
		case 8:
			*buf = -(unsigned long)val;
			break;
		case 16:
			putShort(buf, -(long)val);
			break;
		case 32:
			putLong(buf, -(long)val);
			break;
		default:
			parse_error (cfile,
				     "Unexpected integer size: %d\n", size);
			break;
		}
	} else {
		switch (size) {
		case 8:
			*buf = (uint8_t)val;
			break;
		case 16:
			putUShort (buf, (uint16_t)val);
			break;
		case 32:
			putULong (buf, val);
			break;
		default:
			parse_error (cfile,
				     "Unexpected integer size: %d\n", size);
		}
	}
}

/*
 * option-name :== IDENTIFIER |
 		   IDENTIFIER . IDENTIFIER
 */

struct option *
parse_option_name(struct parse *cfile,
		  isc_boolean_t allocate,
		  isc_boolean_t *known)
{
	const char *val;
	enum dhcp_token token;
	const char *uname;
	struct space *space;
	struct option *option = NULL;
	unsigned code;

	token = next_token(&val, NULL, cfile);
	if (!is_identifier(token))
		parse_error(cfile,
			    "expecting identifier after option keyword.");
	
	uname = strdup(val);
	if (!uname)
		parse_error(cfile, "no memory for uname information.");
	token = peek_token(&val, NULL, cfile);
	if (token == DOT) {
		/* Go ahead and take the DOT token... */
		skip_token(&val, NULL, cfile);

		/* The next token should be an identifier... */
		token = next_token(&val, NULL, cfile);
		if (!is_identifier(token))
			parse_error(cfile, "expecting identifier after '.'");

		/* Look up the option name hash table for the specified
		   uname. */
		space = space_lookup(uname);
		if (space == NULL)
			parse_error(cfile, "no option space named %s.", uname);
	} else {
		/* Use the default hash table, which contains all the
		   standard dhcp option names. */
		val = uname;
		space = space_lookup("dhcp");
	}

	option = option_lookup_name(space->old, val);

	if (option) {
		if (known && (option->status != isc_dhcp_unknown))
			*known = ISC_TRUE;
	} else if (space == space_lookup("server"))
		parse_error(cfile, "unknown server option %s.", val);

        /* If the option name is of the form unknown-[decimal], use
         * the trailing decimal value to find the option definition.
         * If there is no definition, construct one.  This is to
         * support legacy use of unknown options in config files or
         * lease databases.
         */
	else if (strncasecmp(val, "unknown-", 8) == 0) {
		code = atoi(val + 8);

		/* Option code 0 is always illegal for us, thanks
                 * to the option decoder.
                 */
		if (code == 0)
			parse_error(cfile, "Option code 0 is illegal "
				    "in the %s space.", space->old);
		if ((local_family == AF_INET) && (code == 255))
			parse_error(cfile, "Option code 255 is illegal "
				    "in the %s space.", space->old);

		/* It's odd to think of unknown option codes as
                 * being known, but this means we know what the
                 * parsed name is talking about.
                 */
                if (known)
                        *known = ISC_TRUE;
		option = option_lookup_code(space->old, code);

		/* If we did not find an option of that code,
                 * manufacture an unknown-xxx option definition.
		 */
		if (option == NULL) {
			option = (struct option *)malloc(sizeof(*option));
			/* DHCP code does not check allocation failure? */
			memset(option, 0, sizeof(*option));
			option->name = strdup(val);
			option->space = space;
			option->code = code;
			/* X == binary but we shan't use CSV format */
			option->format = "X";
			push_option(option);
		} else {
			struct comment *comment;
			char msg[256];

			snprintf(msg, sizeof(msg),
				 "/// option %s.%s redefinition",
				 space->name, val);
			comment = createComment(msg);
			TAILQ_INSERT_TAIL(&cfile->comments, comment);
		}
	/* If we've been told to allocate, that means that this
	 * (might) be an option code definition, so we'll create
	 * an option structure and return it for the parent to
	 * decide.
	 */
	} else if (allocate) {
		option = (struct option *)malloc(sizeof(*option));
		/* DHCP code does not check allocation failure? */
		memset(option, 0, sizeof(*option));
		option->name = strdup(val);
		option->space = space;
		push_option(option);
	} else
		parse_error(cfile, "no option named %s in space %s",
			    val, space->old);

	return option;
}

/* IDENTIFIER[WIDTHS] SEMI
 *   WIDTHS ~= LENGTH WIDTH NUMBER
 *             CODE WIDTH NUMBER
 */

void
parse_option_space_decl(struct parse *cfile)
{
	int token;
	const char *val;
	struct element *nu;
	struct element *p;
	struct space *universe;
	int tsize = 1, lsize = 1;

	skip_token(&val, NULL, cfile);  /* Discard the SPACE token,
					   which was checked by the
					   caller. */
	token = next_token(&val, NULL, cfile);
	if (!is_identifier(token))
		parse_error(cfile, "expecting identifier.");
	nu = createMap();
	nu->skip = ISC_TRUE;

	/* Expect it will be usable in Kea */
	universe = (struct space *)malloc(sizeof(*universe));
	if (universe == NULL)
		parse_error(cfile, "No memory for new option space.");
	memset(universe, 0, sizeof(*universe));
	universe->old = strdup(val);
	universe->name = universe->old;
	push_space(universe);
	
	do {
		token = next_token(&val, NULL, cfile);
		switch(token) {
		case SEMI:
			break;

		case CODE:
			if (mapSize(nu) == 0) {
				cfile->issue_counter++;
				mapSet(nu,
				       createString(
					       makeString(-1, universe->old)),
				       "name");
			}
			token = next_token(&val, NULL, cfile);
			if (token != WIDTH)
				parse_error(cfile, "expecting width token.");

			token = next_token(&val, NULL, cfile);
			if (token != NUMBER)
				parse_error(cfile,
					    "expecting number 1, 2, 4.");

			tsize = atoi(val);
			p = createInt(tsize);

			if ((local_family == AF_INET) && (tsize != 1)) {
				struct comment *comment;

				comment = createComment("/// only code width "
							"1 is supported");
				TAILQ_INSERT_TAIL(&p->comments, comment);
			} else if ((local_family == AF_INET6) &&
				   (tsize != 2)) {
				struct comment *comment;

				comment = createComment("/// only code width "
							"2 is supported");
				TAILQ_INSERT_TAIL(&p->comments, comment);
			}
			mapSet(nu, p, "code-width");
			break;

		case LENGTH:
			if (mapSize(nu) == 0) {
				cfile->issue_counter++;
				mapSet(nu,
				       createString(
					       makeString(-1, universe->old)),
				       "name");
			}
			token = next_token(&val, NULL, cfile);
			if (token != WIDTH)
				parse_error(cfile, "expecting width token.");

			token = next_token(&val, NULL, cfile);
			if (token != NUMBER)
				parse_error(cfile, "expecting number 1 or 2.");

			lsize = atoi(val);
			p = createInt(lsize);

			if ((local_family == AF_INET) && (lsize != 1)) {
				struct comment *comment;

				comment = createComment("/// only length "
							"width 1 is "
							"supported");
				TAILQ_INSERT_TAIL(&p->comments, comment);
			} else if ((local_family == AF_INET6) &&
				   (lsize != 2)) {
				struct comment *comment;

				comment = createComment("/// only length "
							"width 2 is "
							"supported");
				TAILQ_INSERT_TAIL(&p->comments, comment);
			}
			mapSet(nu, p, "length-width");
			break;

		case HASH:
			token = next_token(&val, NULL, cfile);
			if (token != SIZE)
				parse_error(cfile, "expecting size token.");

			token = next_token(&val, NULL, cfile);
			if (token != NUMBER)
				parse_error(cfile,
					    "expecting a 10base number");
			break;

		default:
			parse_error(cfile, "Unexpected token.");
		}
	} while (token != SEMI);

	if (mapSize(nu) != 0)
		mapSet(cfile->stack[1], nu, "option-space");
}

/* This is faked up to look good right now.   Ideally, this should do a
   recursive parse and allow arbitrary data structure definitions, but for
   now it just allows you to specify a single type, an array of single types,
   a sequence of types, or an array of sequences of types.

   ocd :== NUMBER EQUALS ocsd SEMI

   ocsd :== ocsd_type |
	    ocsd_type_sequence |
	    ARRAY OF ocsd_simple_type_sequence

   ocsd_type_sequence :== LBRACE ocsd_types RBRACE

   ocsd_simple_type_sequence :== LBRACE ocsd_simple_types RBRACE

   ocsd_types :== ocsd_type |
		  ocsd_types ocsd_type

   ocsd_type :== ocsd_simple_type |
		 ARRAY OF ocsd_simple_type

   ocsd_simple_types :== ocsd_simple_type |
			 ocsd_simple_types ocsd_simple_type

   ocsd_simple_type :== BOOLEAN |
			INTEGER NUMBER |
			SIGNED INTEGER NUMBER |
			UNSIGNED INTEGER NUMBER |
			IP-ADDRESS |
			TEXT |
			STRING |
			ENCAPSULATE identifier */

void
parse_option_code_definition(struct parse *cfile, struct option *option)
{
	const char *val;
	enum dhcp_token token;
	struct element *def;
	unsigned code;
	unsigned arrayp = 0;
	int recordp = 0;
	isc_boolean_t no_more_in_record = ISC_FALSE;
	char *type;
	isc_boolean_t is_signed;
	isc_boolean_t has_encapsulation = ISC_FALSE;
	isc_boolean_t not_supported = ISC_FALSE;
	struct string *encapsulated;
	struct string *datatype;
	struct string *saved;
	struct element *optdef;
	
	/* Put the option in the definition */
	def = createMap();
	mapSet(def,
	       createString(makeString(-1, option->space->name)),
	       "space");
	mapSet(def, createString(makeString(-1, option->name)), "name");
	TAILQ_CONCAT(&def->comments, &cfile->comments);

	/* Parse the option code. */
	token = next_token(&val, NULL, cfile);
	if (token != NUMBER)
		parse_error(cfile, "expecting option code number.");
	TAILQ_CONCAT(&def->comments, &cfile->comments);
	code = atoi(val);
	mapSet(def, createInt(code), "code");

	/* We have the code so we can get the real option now */
	if (option->code == 0) {
		struct option *from_code;

		from_code = option_lookup_code(option->space->old, code);
		if (from_code == NULL)
			option->code = code;
		else
			option->status = from_code->status;
	}

	/* Redefinitions are not allowed */
	if ((option->status == isc_dhcp_unknown) ||
	    (option->status == known)) {
		struct comment *comment;

		comment = createComment("/// Kea does not allow redefinition "
					"of options");
		TAILQ_INSERT_TAIL(&def->comments, comment);
		def->skip = ISC_TRUE;
		cfile->issue_counter++;
	}

	token = next_token(&val, NULL, cfile);
	if (token != EQUAL)
		parse_error(cfile, "expecting \"=\"");
	saved = makeString(0, NULL);

	/* See if this is an array. */
	token = next_token(&val, NULL, cfile);
	if (token == ARRAY) {
		token = next_token(&val, NULL, cfile);
		if (token != OF)
			parse_error(cfile, "expecting \"of\".");
		arrayp = 1;
		token = next_token(&val, NULL, cfile);
		appendString(saved, "array of");
	}

	if (token == LBRACE) {
		recordp = 1;
		token = next_token(&val, NULL, cfile);
		appendString(saved, "[");
	}

	/* At this point we're expecting a data type. */
	datatype = makeString(0, NULL);
    next_type:
	if (saved->length > 0)
		appendString(saved, " ");
	type = NULL;
	if (has_encapsulation)
		parse_error(cfile,
			    "encapsulate must always be the last item.");

	switch (token) {
	case ARRAY:
		if (arrayp)
			parse_error(cfile, "no nested arrays.");
		if (recordp) {
			struct comment *comment;

			comment = createComment("/// unsupported array "
						"inside a record");
			TAILQ_INSERT_TAIL(&def->comments, comment);
			def->skip = ISC_TRUE;
			not_supported = ISC_TRUE;
			cfile->issue_counter++;
		}
		token = next_token(&val, NULL, cfile);
		if (token != OF)
			parse_error(cfile, "expecting \"of\".");
		arrayp = recordp + 1;
		token = next_token(&val, NULL, cfile);
		if ((recordp) && (token == LBRACE))
			parse_error(cfile,
				    "only uniform array inside record.");
		if (token == LBRACE) {
			struct comment *comment;

			comment = createComment("/// unsupported record "
						"inside an array");
			TAILQ_INSERT_TAIL(&def->comments, comment);
			def->skip = ISC_TRUE;
			not_supported = ISC_TRUE;
			cfile->issue_counter++;
		}
		appendString(saved, "array of");
		goto next_type;
	case BOOLEAN:
		type = "boolean";
		break;
	case INTEGER:
		is_signed = ISC_TRUE;
	parse_integer:
		token = next_token(&val, NULL, cfile);
		if (token != NUMBER)
			parse_error(cfile, "expecting number.");
		switch (atoi(val)) {
		case 8:
			type = is_signed ? "int8" : "uint8";
			break;
		case 16:
			type = is_signed ? "int16" : "uint16";
			break;
		case 32:
			type = is_signed ? "int32" : "uint32";
			break;
		default:
			parse_error(cfile,
				    "%s bit precision is not supported.", val);
		}
		break;
	case SIGNED:
		is_signed = ISC_TRUE;
	parse_signed:
		token = next_token(&val, NULL, cfile);
		if (token != INTEGER)
			parse_error(cfile, "expecting \"integer\" keyword.");
		goto parse_integer;
	case UNSIGNED:
		is_signed = ISC_FALSE;
		goto parse_signed;

	case IP_ADDRESS:
		type = "ipv4-address";
		break;
	case IP6_ADDRESS:
		type = "ipv6-address";
		break;
	case DOMAIN_NAME:
		type = "fqdn";
		goto no_arrays;
	case DOMAIN_LIST:
		/* Consume optional compression indicator. */
		token = peek_token(&val, NULL, cfile);
		appendString(saved, "list of ");
		if (token == COMPRESSED) {
			struct comment *comment;

			skip_token(&val, NULL, cfile);
			comment = createComment("/// unsupported "
						"compressed fqdn list");
			TAILQ_INSERT_TAIL(&def->comments, comment);
			def->skip = ISC_TRUE;
			not_supported = ISC_TRUE;
			cfile->issue_counter++;
			type = "compressed fqdn";
			appendString(saved, "compressed ");
		} else
			type = "fqdn";
		if (arrayp)
			parse_error(cfile, "arrays of text strings not %s",
				    "yet supported.");
		arrayp = 1;
		no_more_in_record = ISC_TRUE;
		break;
	case TEXT:
		type = "string";
	no_arrays:
		if (arrayp)
			parse_error(cfile, "arrays of text strings not %s",
				    "yet supported.");
		no_more_in_record = ISC_TRUE;
		break;
	case STRING_TOKEN:
		/* can be binary too */
		type = "string";
		goto no_arrays;

	case ENCAPSULATE:
		token = next_token(&val, NULL, cfile);
		if (!is_identifier(token))
			parse_error(cfile,
				    "expecting option space identifier");
		encapsulated = makeString(-1, val);
		has_encapsulation = ISC_TRUE;
		appendString(saved, "encapsulate ");
		appendString(saved, val);
		break;

	case ZEROLEN:
		type = "empty";
		if (arrayp)
			parse_error(cfile, "array incompatible with zerolen.");
		no_more_in_record = ISC_TRUE;
		break;

	default:
		parse_error(cfile, "unknown data type %s", val);
	}
	appendString(saved, type);
	appendString(datatype, type);

	if (recordp) {
		token = next_token(&val, NULL, cfile);
		if (arrayp > recordp)
			arrayp = 0;
		if (token == COMMA) {
			if (no_more_in_record)
				parse_error(cfile,
					    "%s must be at end of record.",
					    type);
			token = next_token(&val, NULL, cfile);
			appendString(saved, ",");
			appendString(datatype, ", ");
			goto next_type;
		}
		if (token != RBRACE)
			parse_error(cfile, "expecting right brace.");
		appendString(saved, "]");
	}
	parse_semi(cfile);
	if (has_encapsulation && arrayp)
		parse_error(cfile,
			    "Arrays of encapsulations don't make sense.");
	if (arrayp)
		mapSet(def, createBool(arrayp), "array");
	if (recordp) {
		mapSet(def, createString(datatype), "record-types");
		mapSet(def, createString(makeString(-1, "record")), "type");
	} else
		mapSet(def, createString(datatype), "type");
	if (not_supported)
		mapSet(def, createString(saved), "definition");
	if (has_encapsulation)
		mapSet(def, createString(encapsulated), "encapsulate");

	optdef = mapGet(cfile->stack[1], "option-def");
	if (optdef == NULL) {
		optdef = createList();
		mapSet(cfile->stack[1], optdef, "option-def");
	}
	listPush(optdef, def);
}

/*
 * base64 :== NUMBER_OR_STRING
 */

struct string *
parse_base64(struct parse *cfile)
{
	const char *val;
	unsigned i;
	static unsigned char
		from64[] = {64, 64, 64, 64, 64, 64, 64, 64,  /*  \"#$%&' */
			     64, 64, 64, 62, 64, 64, 64, 63,  /* ()*+,-./ */
			     52, 53, 54, 55, 56, 57, 58, 59,  /* 01234567 */
			     60, 61, 64, 64, 64, 64, 64, 64,  /* 89:;<=>? */
			     64, 0, 1, 2, 3, 4, 5, 6,	      /* @ABCDEFG */
			     7, 8, 9, 10, 11, 12, 13, 14,     /* HIJKLMNO */
			     15, 16, 17, 18, 19, 20, 21, 22,  /* PQRSTUVW */
			     23, 24, 25, 64, 64, 64, 64, 64,  /* XYZ[\]^_ */
			     64, 26, 27, 28, 29, 30, 31, 32,  /* 'abcdefg */
			     33, 34, 35, 36, 37, 38, 39, 40,  /* hijklmno */
			     41, 42, 43, 44, 45, 46, 47, 48,  /* pqrstuvw */
			     49, 50, 51, 64, 64, 64, 64, 64}; /* xyz{|}~  */
	struct string *t;
	struct string *r;
	isc_boolean_t valid_base64;
	
	r = makeString(0, NULL);

	/* It's possible for a + or a / to cause a base64 quantity to be
	   tokenized into more than one token, so we have to parse them all
	   in before decoding. */
	do {
		unsigned l;

		(void)next_token(&val, &l, cfile);
		t = makeString(l, val);
		concatString(r, t);
		(void)peek_token(&val, NULL, cfile);
		valid_base64 = ISC_TRUE;
		for (i = 0; val[i]; i++) {
			/* Check to see if the character is valid.  It
			   may be out of range or within the right range
			   but not used in the mapping */
			if (((val[i] < ' ') || (val[i] > 'z')) ||
			    ((from64[val[i] - ' '] > 63) && (val[i] != '='))) {
				valid_base64 = ISC_FALSE;
				break; /* no need to continue for loop */
			}
		}
	} while (valid_base64);

	return r;
}

/*
 * colon-separated-hex-list :== NUMBER |
 *				NUMBER COLON colon-separated-hex-list
 */

struct string *
parse_cshl(struct parse *cfile)
{
	uint8_t ibuf;
	char tbuf[4];
	isc_boolean_t first = ISC_TRUE;
	struct string *data;
	enum dhcp_token token;
	const char *val;

	data = makeString(0, NULL);

	for (;;) {
		token = next_token(&val, NULL, cfile);
		if (token != NUMBER && token != NUMBER_OR_NAME)
			parse_error(cfile, "expecting hexadecimal number.");
		convert_num(cfile, &ibuf, val, 16, 8);
		if (first)
			snprintf(tbuf, sizeof(tbuf), "%02hhx", ibuf);
		else
			snprintf(tbuf, sizeof(tbuf), ":%02hhx", ibuf);
		first = ISC_FALSE;
		appendString(data, tbuf);

		token = peek_token(&val, NULL, cfile);
		if (token != COLON)
			break;
		skip_token(&val, NULL, cfile);
	}

	return data;
}

/*
 * executable-statements :== executable-statement executable-statements |
 *			     executable-statement
 *
 * executable-statement :==
 *	IF if-statement |
 * 	ADD class-name SEMI |
 *	BREAK SEMI |
 *	OPTION option-parameter SEMI |
 *	SUPERSEDE option-parameter SEMI |
 *	PREPEND option-parameter SEMI |
 *	APPEND option-parameter SEMI
 */

isc_boolean_t
parse_executable_statements(struct element *statements,
			    struct parse *cfile, isc_boolean_t *lose,
			    enum expression_context case_context)
{
	if (statements->type != ELEMENT_LIST)
		parse_error(cfile, "statements is not a list?");
	for (;;) {
		struct element *statement;

		statement = createMap();
		TAILQ_CONCAT(&statement->comments, &cfile->comments);
		if (!parse_executable_statement(statement, cfile,
						lose, case_context))
			break;
		TAILQ_CONCAT(&statement->comments, &cfile->comments);
		listPush(statements, statement);
	}
	if (!*lose)
		return ISC_TRUE;
		
	return ISC_FALSE;
}

isc_boolean_t
parse_executable_statement(struct element *result,
			   struct parse *cfile, isc_boolean_t *lose,
			   enum expression_context case_context)
{
	enum dhcp_token token;
	const char *val;
	struct element *st;
	struct option *option;
	struct element *var;
	struct element *pri;
	struct element *expr;
	isc_boolean_t known;
	int flag;
	int i;
	struct element *zone;
	struct string *s;

	token = peek_token(&val, NULL, cfile);
	switch (token) {
	case DB_TIME_FORMAT:
		skip_token(&val, NULL, cfile);
		token = next_token(&val, NULL, cfile);
		if (token == DEFAULT)
			s = makeString(-1, val);
		else if (token == LOCAL)
			s = makeString(-1, val);
		else
			parse_error(cfile, "Expecting 'local' or 'default'.");

		token = next_token(&val, NULL, cfile);
		if (token != SEMI)
			parse_error(cfile, "Expecting a semicolon.");
		st = createString(s);
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "db-time-format");

		/* We're done here. */
		return ISC_TRUE;

	case IF:
		skip_token(&val, NULL, cfile);
		return parse_if_statement(result, cfile, lose);

	case TOKEN_ADD:
		skip_token(&val, NULL, cfile);
		token = next_token(&val, NULL, cfile);
		if (token != STRING)
			parse_error(cfile, "expecting class name.");
		s = makeString(-1, val);
		parse_semi(cfile);
		st = createString(s);
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "add-class");
		break;

	case BREAK:
		skip_token(&val, NULL, cfile);
		s = makeString(-1, val);
		parse_semi(cfile);
		st = createNull();
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "break");
		break;

	case SEND:
		skip_token(&val, NULL, cfile);
		known = ISC_FALSE;
	        option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		return parse_option_statement(result, cfile, option,
					      send_option_statement);

	case SUPERSEDE:
	case OPTION:
		skip_token(&val, NULL, cfile);
		known = ISC_FALSE;
		option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		return parse_option_statement(result, cfile, option,
					      supersede_option_statement);

	case ALLOW:
		flag = 1;
		goto pad;
	case DENY:
		flag = 0;
		goto pad;
	case IGNORE:
		flag = 2;
	pad:
		skip_token(&val, NULL, cfile);
		st = parse_allow_deny(cfile, flag);
		mapSet(result, st, "server-control");
		break;

	case DEFAULT:
		skip_token(&val, NULL, cfile);
		token = peek_token(&val, NULL, cfile);
		if (token == COLON)
			goto switch_default;
		known = ISC_FALSE;
		option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		return parse_option_statement(result, cfile, option,
					      default_option_statement);
	case PREPEND:
		skip_token(&val, NULL, cfile);
		known = ISC_FALSE;
                option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		return parse_option_statement(result, cfile, option,
					      prepend_option_statement);
	case APPEND:
		skip_token(&val, NULL, cfile);
		known = ISC_FALSE;
                option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		return parse_option_statement(result, cfile, option,
					      append_option_statement);

	case ON:
		skip_token(&val, NULL, cfile);
		return parse_on_statement(result, cfile, lose);
			
	case SWITCH:
		skip_token(&val, NULL, cfile);
		return parse_switch_statement(result, cfile, lose);

	case CASE:
		skip_token(&val, NULL, cfile);
		if (case_context == context_any)
			parse_error(cfile,
				    "case statement in inappropriate scope.");
		return parse_case_statement(result,
					    cfile, lose, case_context);

	switch_default:
		skip_token(&val, NULL, cfile);
		if (case_context == context_any)
			parse_error(cfile, "switch default statement in %s",
				    "inappropriate scope.");
		s = makeString(-1, "default");
		st = createNull();
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "default");
		return ISC_TRUE;
			
	case DEFINE:
	case TOKEN_SET:
		skip_token(&val, NULL, cfile);
		if (token == DEFINE)
			flag = 1;
		else
			flag = 0;

		token = next_token(&val, NULL, cfile);
		if (token != NAME && token != NUMBER_OR_NAME)
			parse_error(cfile,
				    "%s can't be a variable name", val);
		st = createMap();
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, flag ? "define" : "set");
		var = createString(makeString(-1, val));
		mapSet(st, var, "name");
		token = next_token(&val, NULL, cfile);

		if (token == LPAREN) {
			struct string *value;

			value = makeString(0, NULL);
			do {
				token = next_token(&val, NULL, cfile);
				if (token == RPAREN)
					break;
				if (token != NAME && token != NUMBER_OR_NAME)
					parse_error(cfile,
						    "expecting argument name");
				if (value->length > 0)
					appendString(value, ", ");
				appendString(value, val);
				token = next_token(&val, NULL, cfile);
			} while (token == COMMA);

			if (token != RPAREN) {
				parse_error(cfile, "expecting right paren.");
			badx:
				skip_to_semi(cfile);
				*lose = ISC_TRUE;
				return ISC_FALSE;
			}
			mapSet(st, createString(value), "arguments");

			token = next_token(&val, NULL, cfile);
			if (token != LBRACE)
				parse_error(cfile, "expecting left brace.");

			expr = createList();
			if (!parse_executable_statements(expr, cfile,
							 lose, case_context)) {
				if (*lose)
					goto badx;
			}
			mapSet(st, expr, "function-body");

			token = next_token(&val, NULL, cfile);
			if (token != RBRACE)
				parse_error(cfile, "expecting rigt brace.");
		} else {
			if (token != EQUAL)
				parse_error(cfile,
					    "expecting '=' in %s statement.",
					    flag ? "define" : "set");

			expr = createMap();
			if (!parse_expression(expr, cfile, lose, context_any,
					      NULL, expr_none)) {
				if (!*lose)
					parse_error(cfile,
						    "expecting expression.");
				else
					*lose = ISC_TRUE;
				skip_to_semi(cfile);
				return ISC_FALSE;
			}
			mapSet(st, expr, "value");
			parse_semi(cfile);
		}
		break;

	case UNSET:
		skip_token(&val, NULL, cfile);
		token = next_token(&val, NULL, cfile);
		if (token != NAME && token != NUMBER_OR_NAME)
			parse_error(cfile, "%s can't be a variable name", val);

		st = createMap();
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "unset");
		var = createString(makeString(-1, val));
		mapSet(st, var, "name");
		parse_semi(cfile);
		break;

	case EVAL:
		skip_token(&val, NULL, cfile);
		expr = createMap();

		if (!parse_expression(expr, cfile, lose,
				      context_data, /* XXX */
				      NULL, expr_none)) {
			if (!*lose)
				parse_error(cfile,
					    "expecting data expression.");
			else
				*lose = ISC_TRUE;
			skip_to_semi(cfile);
			return ISC_FALSE;
		}
		mapSet(result, expr, "eval");
		parse_semi(cfile);
		break;

	case EXECUTE:
		parse_error(cfile, "ENABLE_EXECUTE is not portable");

	case RETURN:
		skip_token(&val, NULL, cfile);

		expr = createMap();

		if (!parse_expression(expr, cfile, lose, context_data,
				      NULL, expr_none)) {
			if (!*lose)
				parse_error(cfile,
					    "expecting data expression.");
			else
				*lose = ISC_TRUE;
			skip_to_semi(cfile);
			return ISC_FALSE;
		}
		mapSet(result, expr, "return");
		parse_semi(cfile);
		break;

	case LOG:
		skip_token(&val, NULL, cfile);

		st = createMap();
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "log");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			parse_error(cfile, "left parenthesis expected.");

		token = peek_token(&val, NULL, cfile);
		i = 1;
		if (token == FATAL)
			s = makeString(-1, val);
		else if (token == ERROR)
			s = makeString(-1, val);
		else if (token == TOKEN_DEBUG)
			s = makeString(-1, val);
		else if (token == INFO)
			s = makeString(-1, val);
		else {
			s = makeString(-1, "DEBUG");
			i = 0;
		}
		if (i) {
			skip_token(&val, NULL, cfile);
			token = next_token(&val, NULL, cfile);
			if (token != COMMA)
				parse_error(cfile, "comma expected.");
		}
		pri = createString(s);
		mapSet(st, pri, "priority");

		expr = createMap();
		if (!parse_data_expression(expr, cfile, lose)) {
			skip_to_semi(cfile);
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			parse_error(cfile, "right parenthesis expected.");

		token = next_token(&val, NULL, cfile);
		if (token != SEMI)
			parse_error (cfile, "semicolon expected.");
		break;

	case PARSE_VENDOR_OPT:
		/* The parse-vendor-option; The statement has no arguments.
		 * We simply set up the statement and when it gets executed it
		 * will find all information it needs in the packet and options.
		 */
		skip_token(&val, NULL, cfile);
		parse_semi(cfile);

		st = createNull();
		st->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, st, "parse-vendor-option");
		break;

		/* Not really a statement, but we parse it here anyway
		   because it's appropriate for all DHCP agents with
		   parsers. */
	case ZONE:
		skip_token(&val, NULL, cfile);
		zone = createMap();
		zone->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(result, zone, "zone");

		s = parse_host_name(cfile);
		if (s == NULL) {
			parse_error(cfile, "expecting hostname.");
		badzone:
			*lose = ISC_TRUE;
			skip_to_semi(cfile);
			return ISC_FALSE;
		}
		if (s->content[s->length - 1] != '.')
			appendString(s, ".");
		mapSet(zone, createString(s), "name");
		if (!parse_zone(zone, cfile))
			goto badzone;
		return ISC_TRUE;
		
		/* Also not really a statement, but same idea as above. */
	case KEY:
		skip_token(&val, NULL, cfile);
		if (!parse_key(result, cfile)) {
			/* Kea TODO */
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		return ISC_TRUE;

	default:
		if (is_identifier(token)) {
			/* the config universe is the server one */
			option = option_lookup_name("server", val);
			if (option) {
				skip_token(&val, NULL, cfile);
				result->skip = ISC_TRUE;
				cfile->issue_counter++;
				return parse_config_statement
					      (result, cfile, option,
					       supersede_option_statement);
			}
		}

		if (token == NUMBER_OR_NAME || token == NAME) {
			/* This is rather ugly.  Since function calls are
			   data expressions, fake up an eval statement. */
			expr = createMap();

			if (!parse_expression(expr, cfile, lose, context_data,
					      NULL, expr_none)) {
				if (!*lose)
					parse_error(cfile, "expecting "
						    "function call.");
				else
					*lose = ISC_TRUE;
				skip_to_semi(cfile);
				return ISC_FALSE;
			}
			mapSet(result, expr, "eval");
			parse_semi(cfile);
			break;
		}

		*lose = ISC_FALSE;
		return ISC_FALSE;
	}

	return ISC_TRUE;
}

/* zone-statements :== zone-statement |
		       zone-statement zone-statements
   zone-statement :==
	PRIMARY ip-addresses SEMI |
	SECONDARY ip-addresses SEMI |
	PRIMARY6 ip-address6 SEMI |
	SECONDARY6 ip-address6 SEMI |
	key-reference SEMI
   ip-addresses :== ip-addr-or-hostname |
		  ip-addr-or-hostname COMMA ip-addresses
   key-reference :== KEY STRING |
		    KEY identifier */

isc_boolean_t
parse_zone(struct element *zone, struct parse *cfile)
{
	int token;
	const char *val;
	struct element *values;
	struct string *key_name;
	isc_boolean_t done = ISC_FALSE;

	token = next_token(&val, NULL, cfile);
	if (token != LBRACE)
		parse_error(cfile, "expecting left brace");

	do {
	    token = peek_token(&val, NULL, cfile);
	    switch (token) {
	    case PRIMARY:
		    if (mapContains(zone, "primary"))
			    parse_error(cfile, "more than one primary.");
		    values = createList();
		    mapSet(zone, values, "primary");
		    goto consemup;
		    
	    case SECONDARY:
		    if (mapContains(zone, "secondary"))
			    parse_error(cfile, "more than one secondary.");
		    values = createList();
		    mapSet(zone, values, "secondary");
	    consemup:
		    skip_token(&val, NULL, cfile);
		    do {
			    struct string *value;

			    value = parse_ip_addr_or_hostname(cfile,
							      ISC_FALSE);
			    if (value == NULL)
				parse_error(cfile,
					   "expecting IP addr or hostname.");
			    listPush(values, createString(value));
			    token = next_token(&val, NULL, cfile);
		    } while (token == COMMA);
		    if (token != SEMI)
			    parse_error(cfile, "expecting semicolon.");
		    break;

	    case PRIMARY6:
		    if (mapContains(zone, "primary6"))
			    parse_error(cfile, "more than one primary6.");
		    values = createList();
		    mapSet(zone, values, "primary6");
		    goto consemup6;

	    case SECONDARY6:
		    if (mapContains(zone, "secondary6"))
			    parse_error(cfile, "more than one secondary6.");
		    values = createList();
		    mapSet(zone, values, "secondary6");
	    consemup6:
		    skip_token(&val, NULL, cfile);
		    do {
			    struct string *addr;

			    addr = parse_ip6_addr_txt(cfile);
			    if (addr == NULL)
				    parse_error(cfile, "expecting IPv6 addr.");
			    listPush(values, createString(addr));
			    token = next_token(&val, NULL, cfile);
		    } while (token == COMMA);
		    if (token != SEMI)
			    parse_error(cfile, "expecting semicolon.");
		    break;

	    case KEY:
		    skip_token(&val, NULL, cfile);
		    token = peek_token(&val, NULL, cfile);
		    if (token == STRING) {
			    skip_token(&val, NULL, cfile);
			    key_name = makeString(-1, val);
		    } else {
			    key_name = parse_host_name(cfile);
			    if (!key_name)
				    parse_error(cfile, "expecting key name.");
		    }
		    if (mapContains(zone, "key"))
			    parse_error(cfile, "Multiple key definitions");
		    mapSet(zone, createString(key_name), "key");
		    parse_semi(cfile);
		    break;
		    
	    default:
		    done = 1;
		    break;
	    }
	} while (!done);

	token = next_token(&val, NULL, cfile);
	if (token != RBRACE)
		parse_error(cfile, "expecting right brace.");
	return (1);
}

/* key-statements :== key-statement |
		      key-statement key-statements
   key-statement :==
	ALGORITHM host-name SEMI |
	secret-definition SEMI
   secret-definition :== SECRET base64val |
			 SECRET STRING

   Kea: where to put this? It is a D2 value */

isc_boolean_t
parse_key(struct element* result, struct parse *cfile)
{
	int token;
	const char *val;
	isc_boolean_t done = ISC_FALSE;
	struct element *key;
	struct string *alg;
	struct string *sec;
	struct element *keys;
	char *s;

	key = createMap();
	key->skip = ISC_TRUE;
	cfile->issue_counter++;

	token = peek_token(&val, NULL, cfile);
	if (token == STRING) {
		skip_token(&val, NULL, cfile);
		mapSet(key, createString(makeString(-1, val)), "name");
	} else {
		struct string *name;

		name = parse_host_name(cfile);
		if (name == NULL)
			parse_error(cfile, "expecting key name.");
		mapSet(key, createString(name), "name");
	}

	token = next_token(&val, NULL, cfile);
	if (token != LBRACE)
		parse_error(cfile, "expecting left brace");

	do {
		token = next_token(&val, NULL, cfile);
		switch (token) {
		case ALGORITHM:
			if (mapContains(key, "algorithm"))
				parse_error(cfile, "key: too many algorithms");
			alg = parse_host_name(cfile);
			if (alg == NULL)
				parse_error(cfile,
					    "expecting key algorithm name.");
			parse_semi(cfile);
			/* If the algorithm name isn't an FQDN, tack on
			   the .SIG-ALG.REG.NET. domain. */
			s = strrchr(alg->content, '.');
			if (!s)
				appendString(alg, ".SIG-ALG.REG.INT.");
			/* If there is no trailing '.', hack one in. */
			else 
				appendString(alg, ".");
			mapSet(key, createString(alg), "algorithm");
			break;

		case SECRET:
			if (mapContains(key, "secret"))
				parse_error(cfile, "key: too many secrets");

			sec = parse_base64(cfile);
			if (sec == NULL) {
				skip_to_rbrace(cfile, 1);
				return ISC_FALSE;
			}
			mapSet(key, createString(sec), "secret");

			parse_semi(cfile);
			break;

		default:
			done = ISC_TRUE;
			break;
		}
	} while (!done);
	if (token != RBRACE)
		parse_error(cfile, "expecting right brace.");
	/* Allow the BIND 8 syntax, which has a semicolon after each
	   closing brace. */
	token = peek_token(&val, NULL, cfile);
	if (token == SEMI)
		skip_token(&val, NULL, cfile);

	/* Remember the key. */
	keys = mapGet(result, "tsig-keys");
	if (keys == NULL) {
		keys = createList();
		mapSet(result, keys, "tsig-keys");
	}
	listPush(keys, key);
	return ISC_TRUE;
}

/*
 * on-statement :== event-types LBRACE executable-statements RBRACE
 * event-types :== event-type OR event-types |
 *		   event-type
 * event-type :== EXPIRY | COMMIT | RELEASE
 */

isc_boolean_t
parse_on_statement(struct element *result,
		   struct parse *cfile,
		   isc_boolean_t *lose)
{
	enum dhcp_token token;
	const char *val;
	struct element *statement;
	struct string *cond;
	struct element *body;

	statement = createMap();
	statement->skip = ISC_TRUE;
	cfile->issue_counter++;
	mapSet(result, statement, "on");

	cond = makeString(0, NULL);
	do {
		token = next_token(&val, NULL, cfile);
		switch (token) {
		case EXPIRY:
		case COMMIT:
		case RELEASE:
		case TRANSMISSION:
			appendString(cond, val);      
			break;

		default:
			parse_error(cfile, "expecting a lease event type");
		}
		token = next_token(&val, NULL, cfile);
		if (token == OR)
			appendString(cond, " or ");
	} while (token == OR);
		
	mapSet(statement, createString(cond), "condition");

	/* Semicolon means no statements. */
	if (token == SEMI)
		return ISC_TRUE;

	if (token != LBRACE)
		parse_error(cfile, "left brace expected.");

	body = createList();
	if (!parse_executable_statements(body, cfile, lose, context_any)) {
		if (*lose) {
			/* Try to even things up. */
			do {
				token = next_token(&val, NULL, cfile);
			} while (token != END_OF_FILE && token != RBRACE);
			return ISC_FALSE;
		}
	}
	mapSet(statement, body, "body");
	token = next_token(&val, NULL, cfile);
	if (token != RBRACE)
		parse_error(cfile, "right brace expected.");
	return ISC_TRUE;
}

/*
 * switch-statement :== LPAREN expr RPAREN LBRACE executable-statements RBRACE
 *
 */

isc_boolean_t
parse_switch_statement(struct element *result,
		       struct parse *cfile,
		       isc_boolean_t *lose)
{
	enum dhcp_token token;
	const char *val;
	struct element *statement;
	struct element *cond;
	struct element *body;

	statement = createMap();
	statement->skip = ISC_TRUE;
	cfile->issue_counter++;
	mapSet(result, statement, "switch");

	token = next_token(&val, NULL, cfile);
	if (token != LPAREN) {
		parse_error(cfile, "expecting left brace.");
		*lose = ISC_TRUE;
		skip_to_semi(cfile);
		return ISC_FALSE;
	}

	cond = createMap();
	if (!parse_expression(cond, cfile, lose, context_data_or_numeric,
			      NULL, expr_none)) {
		if (!*lose)
			parse_error(cfile,
				    "expecting data or numeric expression.");
		return ISC_FALSE;
	}
	mapSet(statement, cond, "condition");

	token = next_token(&val, NULL, cfile);
	if (token != RPAREN)
		parse_error(cfile, "right paren expected.");

	token = next_token(&val, NULL, cfile);
	if (token != LBRACE)
		parse_error(cfile, "left brace expected.");

	body = createList();
	if (!parse_executable_statements(body, cfile, lose,
	      (is_data_expression(cond) ? context_data : context_numeric))) {
		if (*lose) {
			skip_to_rbrace(cfile, 1);
			return ISC_FALSE;
		}
	}
	mapSet(statement, body, "body");
	token = next_token(&val, NULL, cfile);
	if (token != RBRACE)
		parse_error(cfile, "right brace expected.");
	return ISC_TRUE;
}

/*
 * case-statement :== CASE expr COLON
 *
 */

isc_boolean_t
parse_case_statement(struct element *result,
		     struct parse *cfile,
		     isc_boolean_t *lose,
		     enum expression_context case_context)
{
	enum dhcp_token token;
	const char *val;
	struct element *expr;

	expr = createMap();
	if (!parse_expression(expr, cfile, lose, case_context,
			      NULL, expr_none))
	{
		if (!*lose)
			parse_error(cfile, "expecting %s expression.",
				    (case_context == context_data
				     ? "data" : "numeric"));
		*lose = ISC_TRUE;
		skip_to_semi(cfile);
		return ISC_FALSE;
	}

	token = next_token(&val, NULL, cfile);
	if (token != COLON)
		parse_error(cfile, "colon expected.");
	mapSet(result, expr, "case");
	return ISC_TRUE;
}

/*
 * if-statement :== boolean-expression LBRACE executable-statements RBRACE
 *						else-statement
 *
 * else-statement :== <null> |
 *		      ELSE LBRACE executable-statements RBRACE |
 *		      ELSE IF if-statement |
 *		      ELSIF if-statement
 */

isc_boolean_t
parse_if_statement(struct element *result,
		   struct parse *cfile,
		   isc_boolean_t *lose)
{
	enum dhcp_token token;
	const char *val;
	isc_boolean_t parenp;
	struct element *statement;
	struct element *cond;
	struct element *branch;

	statement = createMap();
	statement->skip = ISC_TRUE;
	cfile->issue_counter++;

	mapSet(result, statement, "if");

	token = peek_token(&val, NULL, cfile);
	if (token == LPAREN) {
		parenp = ISC_TRUE;
		skip_token(&val, NULL, cfile);
	} else
		parenp = ISC_FALSE;

	cond = createMap();
	if (!parse_boolean_expression(cond, cfile, lose)) {
		if (!*lose)
			parse_error(cfile, "boolean expression expected.");
		*lose = ISC_TRUE;
		return ISC_FALSE;
	}
	mapSet(statement, cond, "condition");
	if (parenp) {
		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			parse_error(cfile, "expecting right paren.");
	}
	token = next_token(&val, NULL, cfile);
	if (token != LBRACE)
		parse_error(cfile, "left brace expected.");
	branch = createList();
	if (!parse_executable_statements(branch, cfile, lose, context_any)) {
		if (*lose) {
			/* Try to even things up. */
			do {
				token = next_token(&val, NULL, cfile);
			} while (token != END_OF_FILE && token != RBRACE);
			return ISC_FALSE;
		}
	}
	mapSet(statement, branch, "then");
	token = next_token(&val, NULL, cfile);
	if (token != RBRACE)
		parse_error(cfile, "right brace expected.");
	token = peek_token(&val, NULL, cfile);
	if (token == ELSE) {
		skip_token(&val, NULL, cfile);
		token = peek_token(&val, NULL, cfile);
		if (token == IF) {
			skip_token(&val, NULL, cfile);
			branch = createMap();
			if (!parse_if_statement(branch, cfile, lose)) {
				if (!*lose)
					parse_error(cfile,
						    "expecting if statement");
				*lose = ISC_TRUE;
				return ISC_FALSE;
			}
		} else if (token != LBRACE)
			parse_error(cfile, "left brace or if expected.");
		else {
			skip_token(&val, NULL, cfile);
			branch = createList();
			if (!parse_executable_statements(branch, cfile,
							 lose, context_any))
				return ISC_FALSE;
			token = next_token(&val, NULL, cfile);
			if (token != RBRACE)
				parse_error(cfile, "right brace expected.");
		}
		mapSet(statement, branch, "else");
	} else if (token == ELSIF) {
		skip_token(&val, NULL, cfile);
		branch = createMap();
		if (!parse_if_statement(branch, cfile, lose)) {
			if (!*lose)
				parse_error(cfile,
					    "expecting conditional.");
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		mapSet(statement, branch, "else");
	}
	
	return ISC_TRUE;
}

/*
 * boolean_expression :== CHECK STRING |
 *  			  NOT boolean-expression |
 *			  data-expression EQUAL data-expression |
 *			  data-expression BANG EQUAL data-expression |
 *			  data-expression REGEX_MATCH data-expression |
 *			  boolean-expression AND boolean-expression |
 *			  boolean-expression OR boolean-expression
 *			  EXISTS OPTION-NAME
 */
   			  
isc_boolean_t
parse_boolean_expression(struct element *expr,
			 struct parse *cfile,
			 isc_boolean_t *lose)
{
	/* Parse an expression... */
	if (!parse_expression(expr, cfile, lose, context_boolean,
			      NULL, expr_none))
		return ISC_FALSE;

	if (!is_boolean_expression(expr) &&
	    !mapContains(expr, "variable-reference") &&
	    !mapContains(expr, "funcall"))
		parse_error(cfile, "Expecting a boolean expression.");
	return ISC_TRUE;
}

/* boolean :== ON SEMI | OFF SEMI | TRUE SEMI | FALSE SEMI */

isc_boolean_t
parse_boolean(struct parse *cfile)
{
	const char *val;
	isc_boolean_t rv;

        (void)next_token(&val, NULL, cfile);
	if (!strcasecmp (val, "true")
	    || !strcasecmp (val, "on"))
		rv = ISC_TRUE;
	else if (!strcasecmp (val, "false")
		 || !strcasecmp (val, "off"))
		rv = ISC_FALSE;
	else
		parse_error(cfile,
			    "boolean value (true/false/on/off) expected");
	parse_semi(cfile);
	return rv;
}

/*
 * data_expression :== SUBSTRING LPAREN data-expression COMMA
 *					numeric-expression COMMA
 *					numeric-expression RPAREN |
 *		       CONCAT LPAREN data-expression COMMA 
 *					data-expression RPAREN
 *		       SUFFIX LPAREN data_expression COMMA
 *		       		     numeric-expression RPAREN |
 *		       LCASE LPAREN data_expression RPAREN |
 *		       UCASE LPAREN data_expression RPAREN |
 *		       OPTION option_name |
 *		       HARDWARE |
 *		       PACKET LPAREN numeric-expression COMMA
 *				     numeric-expression RPAREN |
 *		       V6RELAY LPAREN numeric-expression COMMA
 *				      data-expression RPAREN |
 *		       STRING |
 *		       colon_separated_hex_list
 */

isc_boolean_t
parse_data_expression(struct element *expr,
		      struct parse *cfile,
		      isc_boolean_t *lose)
{
	/* Parse an expression... */
	if (!parse_expression(expr, cfile, lose, context_data,
			      NULL, expr_none))
		return ISC_FALSE;

	if (!is_data_expression(expr) &&
	    !mapContains(expr, "variable-reference") &&
	    !mapContains(expr, "funcall"))
		parse_error(cfile, "Expecting a data expression.");
	return ISC_TRUE;
}

/*
 * numeric-expression :== EXTRACT_INT LPAREN data-expression
 *					     COMMA number RPAREN |
 *			  NUMBER
 */

isc_boolean_t
parse_numeric_expression(struct element *expr,
			 struct parse *cfile,
			 isc_boolean_t *lose)
{
	/* Parse an expression... */
	if (!parse_expression(expr, cfile, lose, context_numeric,
			      NULL, expr_none))
		return ISC_FALSE;

	if (!is_numeric_expression(expr) &&
	    !mapContains(expr, "variable-reference") &&
	    !mapContains(expr, "funcall"))
		parse_error(cfile, "Expecting a numeric expression.");
	return ISC_TRUE;
}

/* Parse a subexpression that does not contain a binary operator. */

isc_boolean_t
parse_non_binary(struct element *expr,
		 struct parse *cfile,
		 isc_boolean_t *lose,
		 enum expression_context context)
{
	enum dhcp_token token;
	const char *val;
	struct element *nexp;
	struct element *arg;
	struct element *chain;
	struct string *data;
	struct comment *comment;
	struct option *option;
	isc_boolean_t known;
	unsigned len;

	token = peek_token(&val, NULL, cfile);

	/* Check for unary operators... */
	switch (token) {
	case CHECK:
		skip_token(&val, NULL, cfile);
		token = next_token(&val, NULL, cfile);
		if (token != STRING)
			parse_error(cfile, "string expected.");
		nexp = createString(makeString(-1, val));
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "check");
		break;

	case TOKEN_NOT:
		skip_token(&val, NULL, cfile);
		nexp = createMap();

		if (!parse_non_binary(nexp, cfile, lose, context_boolean)) {
			if (!*lose)
				parse_error(cfile, "expression expected");
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		if (!is_boolean_expression(nexp))
			parse_error(cfile, "boolean expression expected");
		if (!nexp->skip) {
			nexp->skip = ISC_TRUE;
			cfile->issue_counter++;
		}
		mapSet(expr, nexp, "not");
		break;

	case LPAREN:
		skip_token(&val, NULL, cfile);
		if (!parse_expression(expr, cfile, lose, context,
				      NULL, expr_none)) {
			if (!*lose)
				parse_error(cfile, "expression expected");
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			parse_error(cfile, "right paren expected");
		break;

	case EXISTS:
		skip_token(&val, NULL, cfile);
		known = ISC_FALSE;
		option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;;
		}
		nexp = createMap();
		/* push infos to get it back trying to reduce it */
		mapSet(nexp,
		       createString(makeString(-1, option->space->old)),
		       "universe");
		mapSet(nexp,
		       createString(makeString(-1, option->name)),
		       "name");
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "exists");
		break;

	case STATIC:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "static");
		break;

	case KNOWN:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "known");
		break;

	case SUBSTRING:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "substring");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN) {
		nolparen:
			parse_error(cfile, "left parenthesis expected.");
		}

		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose)) {
		nodata:
			if (!*lose)
				parse_error(cfile,
					    "expecting data expression.");
			return ISC_FALSE;
		}
		mapSet(nexp, arg, "expression");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA) {
		nocomma:
			parse_error(cfile, "comma expected.");
		}

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose)) {
		nonum:
			if (!*lose)
				parse_error(cfile,
					    "expecting numeric expression.");
			return ISC_FALSE;
		}
		mapSet(nexp, arg, "offset");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nonum;
		mapSet(nexp, arg, "length");

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN) {
		norparen:
			parse_error(cfile, "right parenthesis expected.");
		}
		break;

	case SUFFIX:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "suffix");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "expression");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nonum;
		mapSet(nexp, arg, "length");

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;

	case LCASE:
		skip_token(&val, NULL, cfile);
		nexp = createMap();

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		if (!parse_data_expression(nexp, cfile, lose))
			goto nodata;

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		if (!nexp->skip) {
			nexp->skip = ISC_TRUE;
			cfile->issue_counter++;
		}
		mapSet(expr, nexp, "lowercase");
		break;

	case UCASE:
		skip_token(&val, NULL, cfile);
		nexp = createMap();

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		if (!parse_data_expression(nexp, cfile, lose))
			goto nodata;

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		if (!nexp->skip) {
			nexp->skip = ISC_TRUE;
			cfile->issue_counter++;
		}
		mapSet(expr, nexp, "uppercase");
		break;

	case CONCAT:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "concat");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "left");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

	concat_another:
		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose))
			goto nodata;

		token = next_token(&val, NULL, cfile);

		if (token == COMMA) {
			chain = createMap();
			mapSet(nexp, chain, "right");
			nexp = createMap();
			mapSet(chain, nexp, "concat");
			mapSet(nexp, arg, "left");
			goto concat_another;
		}
		mapSet(nexp, arg, "right");

		if (token != RPAREN)
			goto norparen;
		break;

	case BINARY_TO_ASCII:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "binary-to-ascii");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "base");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "width");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "separator");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "buffer");

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;

	case REVERSE:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "reverse");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		arg = createMap();
		if (!(parse_numeric_expression(arg, cfile, lose)))
			goto nodata;
		mapSet(nexp, arg, "width");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!(parse_data_expression(arg, cfile, lose)))
			goto nodata;
		mapSet(nexp, arg, "buffer");

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;

	case PICK:
		/* pick (a, b, c) actually produces an internal representation
		   that looks like pick (a, pick (b, pick (c, nil))). */
		skip_token(&val, NULL, cfile);
		nexp = createList();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "pick-first-value");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		do {
			arg = createMap();
			if (!(parse_data_expression(arg, cfile, lose)))
				goto nodata;
			listPush(nexp, arg);

			token = next_token(&val, NULL, cfile);
		} while (token == COMMA);

		if (token != RPAREN)
			goto norparen;
		break;

	case OPTION:
	case CONFIG_OPTION:
		skip_token(&val, NULL, cfile);
		known = ISC_FALSE;
		option = parse_option_name(cfile, ISC_FALSE, &known);
		if (option == NULL) {
			*lose = ISC_TRUE;
			return ISC_FALSE;
		}
		nexp = createMap();
		mapSet(nexp,
		       createString(makeString(-1, option->space->old)),
		       "universe");
		mapSet(nexp,
		       createString(makeString(-1, option->name)),
		       "name");
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp,
		       token == OPTION ? "option" : "config-option");
		break;

	case HARDWARE:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "hardware");
		break;

	case LEASED_ADDRESS:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "leased-address");
		break;

	case CLIENT_STATE:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "client-state");
		break;

	case FILENAME:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "filename");
		break;

	case SERVER_NAME:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "server-name");
		break;

	case LEASE_TIME:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "lease-time");
		break;

	case TOKEN_NULL:
		skip_token(&val, NULL, cfile);
		/* can look at context to return directly ""? */
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "null");
		break;

	case HOST_DECL_NAME:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "host-decl-name");
		break;

	case PACKET:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "packet");

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nonum;
		mapSet(nexp, arg, "offset");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nonum;
		mapSet(nexp, arg, "length");

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;
		
	case STRING:
		skip_token(&val, &len, cfile);
		resetString(expr, makeString(len, val));
		break;

	case EXTRACT_INT:
		skip_token(&val, NULL, cfile);	
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			parse_error(cfile, "left parenthesis expected.");

		if (!parse_data_expression(nexp, cfile, lose)) {
			if (!*lose)
				parse_error(cfile,
					    "expecting data expression.");
			return ISC_FALSE;
		}

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			parse_error(cfile, "comma expected.");

		token = next_token(&val, NULL, cfile);
		if (token != NUMBER)
			parse_error(cfile, "number expected.");
		switch (atoi(val)) {
		case 8:
			mapSet(expr, nexp, "extract-int8");
			break;

		case 16:
			mapSet(expr, nexp, "extract-int16");
			break;

		case 32:
			mapSet(expr, nexp, "extract-int32");
			break;

		default:
			parse_error(cfile, "unsupported integer size %s", val);
		}

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			parse_error(cfile, "right parenthesis expected.");
		break;
	
	case ENCODE_INT:
		skip_token(&val, NULL, cfile);	
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;

		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			parse_error(cfile, "left parenthesis expected.");

		if (!parse_numeric_expression(nexp, cfile, lose))
			parse_error(cfile, "expecting numeric expression.");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			parse_error(cfile, "comma expected.");

		token = next_token(&val, NULL, cfile);
		if (token != NUMBER)
			parse_error(cfile, "number expected.");
		switch (atoi(val)) {
		case 8:
			mapSet(expr, nexp, "encode-int8");
			break;

		case 16:
			mapSet(expr, nexp, "encode-int16");
			break;

		case 32:
			mapSet(expr, nexp, "encode-int32");
			break;

		default:
			parse_error(cfile, "unsupported integer size %s", val);
		}

		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			parse_error(cfile, "right parenthesis expected.");
		break;
	
	case NUMBER:
		/* If we're in a numeric context, this should just be a
		   number, by itself. */
		if (context == context_numeric ||
		    context == context_data_or_numeric) {
			skip_token(&val, NULL, cfile);
			resetInt(expr, atoi(val));
			break;
		}

	case NUMBER_OR_NAME:
		data = parse_cshl(cfile);
		if (data == NULL)
			return ISC_FALSE;
		resetString(expr, data);
		break;

	case NS_FORMERR:
		skip_token(&val, NULL, cfile);
#ifndef FORMERR
#define FORMERR 1
#endif
		resetInt(expr, FORMERR);
		comment = createComment("/// constant FORMERR(1)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_NOERROR:
		skip_token(&val, NULL, cfile);
#ifndef ISC_R_SUCCESS
#define ISC_R_SUCCESS 0
#endif
		resetInt(expr, ISC_R_SUCCESS);
		comment = createComment("/// constant ISC_R_SUCCESS(0)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_NOTAUTH:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_NOTAUTH
#define DHCP_R_NOTAUTH ((6 << 16) + 21)
#endif
		resetInt(expr, DHCP_R_NOTAUTH);
		comment = createComment("/// constant DHCP_R_NOTAUTH(393237)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_NOTIMP:
		skip_token(&val, NULL, cfile);
#ifndef ISC_R_NOTIMPLEMENTED
#define ISC_R_NOTIMPLEMENTED 27
#endif
		resetInt(expr, ISC_R_NOTIMPLEMENTED);
		comment = createComment("/// constant ISC_R_NOTIMPLEMENTED(27)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_NOTZONE:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_NOTZONE
#define DHCP_R_NOTZONE ((6 << 16) + 22)
#endif
		resetInt(expr, DHCP_R_NOTZONE);
		comment = createComment("/// constant DHCP_R_NOTZONE(393238)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_NXDOMAIN:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_NXDOMAIN
#define DHCP_R_NXDOMAIN ((6 << 16) + 15)
#endif
		resetInt(expr, DHCP_R_NXDOMAIN);
		comment = createComment("/// constant DHCP_R_NXDOMAIN(393231)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_NXRRSET:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_NXRRSET
#define DHCP_R_NXRRSET ((6 << 16) + 20)
#endif
		resetInt(expr, DHCP_R_NXRRSET);
		comment = createComment("/// constant DHCP_R_NXRRSET(393236)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_REFUSED:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_REFUSED
#define DHCP_R_REFUSED ((6 << 16) + 17)
#endif
		resetInt(expr, DHCP_R_REFUSED);
		comment = createComment("/// constant DHCP_R_REFUSED(393233)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_SERVFAIL:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_SERVFAIL
#define DHCP_R_SERVFAIL ((6 << 16) + 14)
#endif
		resetInt(expr, DHCP_R_SERVFAIL);
		comment = createComment("/// constant DHCP_R_SERVFAIL(393230)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_YXDOMAIN:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_YXDOMAIN
#define DHCP_R_YXDOMAIN ((6 << 16) + 18)
#endif
		resetInt(expr, DHCP_R_YXDOMAIN);
		comment = createComment("/// constant DHCP_R_YXDOMAIN(393234)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case NS_YXRRSET:
		skip_token(&val, NULL, cfile);
#ifndef DHCP_R_YXRRSET
#define DHCP_R_YXRRSET ((6 << 16) + 19)
#endif
		resetInt(expr, DHCP_R_YXRRSET);
		comment = createComment("/// constant DHCP_R_YXRRSET(393235)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case BOOTING:
		skip_token(&val, NULL, cfile);
#ifndef S_INIT
#define S_INIT 2
#endif
		resetInt(expr, S_INIT);
		comment = createComment("/// constant S_INIT(2)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case REBOOT:
		skip_token(&val, NULL, cfile);
#ifndef S_REBOOTING
#define S_REBOOTING 1
#endif
		resetInt(expr, S_REBOOTING);
		comment = createComment("/// constant S_REBOOTING(1)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case SELECT:
		skip_token(&val, NULL, cfile);
#ifndef S_SELECTING
#define S_SELECTING 3
#endif
		resetInt(expr, S_SELECTING);
		comment = createComment("/// constant S_SELECTING(3)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case REQUEST:
		skip_token(&val, NULL, cfile);
#ifndef S_REQUESTING
#define S_REQUESTING 4
#endif
		resetInt(expr, S_REQUESTING);
		comment = createComment("/// constant S_REQUESTING(4)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case BOUND:
		skip_token(&val, NULL, cfile);
#ifndef S_BOUND
#define S_BOUND 5
#endif
		resetInt(expr, S_BOUND);
		comment = createComment("/// constant S_BOUND(5)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case RENEW:
		skip_token(&val, NULL, cfile);
#ifndef S_RENEWING
#define S_RENEWING 6
#endif
		resetInt(expr, S_RENEWING);
		comment = createComment("/// constant S_RENEWING(6)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case REBIND:
		skip_token(&val, NULL, cfile);
#ifndef S_REBINDING
#define S_REBINDING 7
#endif
		resetInt(expr, S_REBINDING);
		comment = createComment("/// constant S_REBINDING(7)");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		break;

	case DEFINED:
		skip_token(&val, NULL, cfile);
		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		token = next_token(&val, NULL, cfile);
		if (token != NAME && token != NUMBER_OR_NAME)
			parse_error(cfile, "%s can't be a variable name", val);

		nexp = createString(makeString(-1, val));
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "variable-exists");
		token = next_token(&val, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;

		/* This parses 'gethostname()'. */
	case GETHOSTNAME:
		skip_token(&val, NULL, cfile);
		nexp = createNull();
		nexp->skip = ISC_TRUE;
                cfile->issue_counter++;
		mapSet(expr, nexp, "gethostname");

		token = next_token(NULL, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		token = next_token(NULL, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;

	case GETHOSTBYNAME:
		skip_token(&val, NULL, cfile);
		token = next_token(NULL, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		/* The argument is a quoted string. */
		token = next_token(&val, NULL, cfile);
		if (token != STRING)
			parse_error(cfile, "Expecting quoted literal: "
				    "\"foo.example.com\"");
		nexp = createString(makeString(-1, val));
		nexp->skip = ISC_TRUE;
                cfile->issue_counter++;
                mapSet(expr, nexp, "gethostbyname");

		token = next_token(NULL, NULL, cfile);
		if (token != RPAREN)
			goto norparen;
		break;

	case V6RELAY:
		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "v6relay");
		
		token = next_token(&val, NULL, cfile);
		if (token != LPAREN)
			goto nolparen;

		arg = createMap();
		if (!parse_numeric_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "relay");

		token = next_token(&val, NULL, cfile);
		if (token != COMMA)
			goto nocomma;

		arg = createMap();
		if (!parse_data_expression(arg, cfile, lose))
			goto nodata;
		mapSet(nexp, arg, "relay-option");

		token = next_token(&val, NULL, cfile);

		if (token != RPAREN)
			goto norparen;
		break;

		/* Not a valid start to an expression... */
	default:
		if (token != NAME && token != NUMBER_OR_NAME)
			return ISC_FALSE;

		skip_token(&val, NULL, cfile);

		/* Save the name of the variable being referenced. */
		data = makeString(-1, val);

		/* Simple variable reference, as far as we can tell. */
		token = peek_token(&val, NULL, cfile);
		if (token != LPAREN) {
			nexp = createString(data);
			nexp->skip = ISC_TRUE;
			cfile->issue_counter++;
			mapSet(expr, nexp, "variable-reference");
			break;
		}

		skip_token(&val, NULL, cfile);
		nexp = createMap();
		nexp->skip = ISC_TRUE;
		cfile->issue_counter++;
		mapSet(expr, nexp, "funcall");
		chain = createString(data);
		mapSet(nexp, chain, "name");

		/* Now parse the argument list. */
		chain = createList();
		do {
			arg = createMap();
			if (!parse_expression(arg, cfile, lose, context_any,
					      NULL, expr_none)) {
				if (!*lose)
					parse_error(cfile,
						    "expecting expression.");
				skip_to_semi(cfile);
				return ISC_FALSE;
			}
			listPush(chain, arg);
			token = next_token(&val, NULL, cfile);
		} while (token == COMMA);
		if (token != RPAREN)
			parse_error (cfile, "Right parenthesis expected.");
		mapSet(nexp, chain, "arguments");
		break;
	}
	return ISC_TRUE;
}

/* Parse an expression. */

isc_boolean_t
parse_expression(struct element *expr, struct parse *cfile,
		 isc_boolean_t *lose, enum expression_context context,
		 struct element *lhs, enum expr_op binop)
{
	enum dhcp_token token;
	const char *val;
	struct element *rhs, *tmp;
	enum expr_op next_op;
	enum expression_context
		lhs_context = context_any,
		rhs_context = context_any;
	const char *binop_name;

new_rhs:
	rhs = createMap();
	if (!parse_non_binary(rhs, cfile, lose, context)) {
		/* If we already have a left-hand side, then it's not
		   okay for there not to be a right-hand side here, so
		   we need to flag it as an error. */
		if (lhs)
			if (!*lose)
				parse_error(cfile,
					    "expecting right-hand side.");
		return ISC_FALSE;
	}

	/* At this point, rhs contains either an entire subexpression,
	   or at least a left-hand-side.   If we do not see a binary token
	   as the next token, we're done with the expression. */

	token = peek_token(&val, NULL, cfile);
	switch (token) {
	case BANG:
		skip_token(&val, NULL, cfile);
		token = peek_token(&val, NULL, cfile);
		if (token != EQUAL)
			parse_error(cfile, "! in boolean context without =");
		next_op = expr_not_equal;
		context = expression_context(rhs);
		break;

	case EQUAL:
		next_op = expr_equal;
		context = expression_context(rhs);
		break;

	case TILDE:
		skip_token(&val, NULL, cfile);
		token = peek_token(&val, NULL, cfile);

		if (token == TILDE)
			next_op = expr_iregex_match;
		else if (token == EQUAL)
			next_op = expr_regex_match;
		else
			parse_error(cfile, "expecting ~= or ~~ operator");

		context = expression_context(rhs);
		break;

	case AND:
		next_op = expr_and;
		context = expression_context(rhs);
		break;

	case OR:
		next_op = expr_or;
		context = expression_context(rhs);
		break;

	case PLUS:
		next_op = expr_add;
		context = expression_context(rhs);
		break;

	case MINUS:
		next_op = expr_subtract;
		context = expression_context(rhs);
		break;

	case SLASH:
		next_op = expr_divide;
		context = expression_context(rhs);
		break;

	case ASTERISK:
		next_op = expr_multiply;
		context = expression_context(rhs);
		break;

	case PERCENT:
		next_op = expr_remainder;
		context = expression_context(rhs);
		break;

	case AMPERSAND:
		next_op = expr_binary_and;
		context = expression_context(rhs);
		break;

	case PIPE:
		next_op = expr_binary_or;
		context = expression_context(rhs);
		break;

	case CARET:
		next_op = expr_binary_xor;
		context = expression_context(rhs);
		break;

	default:
		next_op = expr_none;
	}

	/* If we have no lhs yet, we just parsed it. */
	if (!lhs) {
		/* If there was no operator following what we just parsed,
		   then we're done - return it. */
		if (next_op == expr_none) {
			resetBy(expr, rhs);
			return ISC_TRUE;
		}
		
		lhs = rhs;
		rhs = NULL;
		binop = next_op;
		skip_token(&val, NULL, cfile);
		goto new_rhs;
	}

	/* If the next binary operator is of greater precedence than the
	 * current operator, then rhs we have parsed so far is actually
	 * the lhs of the next operator.  To get this value, we have to
	 * recurse.
	 */
	if (binop != expr_none && next_op != expr_none &&
	    op_precedence(binop, next_op) < 0) {

		/* Eat the subexpression operator token, which we pass to
		 * parse_expression...we only peek()'d earlier.
		 */
		skip_token(&val, NULL, cfile);

		/* Continue parsing of the right hand side with that token. */
		tmp = rhs;
		rhs = createMap();
		if (!parse_expression(rhs, cfile, lose, op_context(next_op),
				      tmp, next_op)) {
			if (!*lose)
				parse_error(cfile,
					    "expecting a subexpression");
			return ISC_FALSE;
		}
		next_op = expr_none;
	}

	binop_name = "none";
	if (binop != expr_none) {
		rhs_context = expression_context(rhs);
		lhs_context = expression_context(lhs);

		if ((rhs_context != context_any) && 
		    (lhs_context != context_any) &&
		    (rhs_context != lhs_context))
			parse_error(cfile, "illegal expression relating "
				    "different types");

		switch (binop) {
		case expr_not_equal:
			binop_name = "not-equal";
			goto data_numeric;
		case expr_equal:
			binop_name = "equal";
		data_numeric:
			if ((rhs_context != context_data_or_numeric) &&
			    (rhs_context != context_data) &&
			    (rhs_context != context_numeric) &&
			    (rhs_context != context_any))
				parse_error(cfile, "expecting data/numeric "
					    "expression");
			break;

		case expr_iregex_match:
			binop_name = "iregex-match";
			break;

		case expr_regex_match:
			binop_name = "regex-match";
			if (expression_context(rhs) != context_data)
				parse_error(cfile,
					    "expecting data expression");
			break;

		case expr_and:
			binop_name = "and";
			goto boolean;
		case expr_or:
			binop_name = "or";
		boolean:
			if ((rhs_context != context_boolean) &&
			    (rhs_context != context_any))
				parse_error(cfile,
					    "expecting boolean expressions");
				break;

		case expr_add:
			binop_name = "add";
			goto numeric;
		case expr_subtract:
			binop_name = "subtract";
			goto numeric;
		case expr_divide:
			binop_name = "divide";
			goto numeric;
		case expr_multiply:
			binop_name = "multiply";
			goto numeric;
		case expr_remainder:
			binop_name = "remainder";
			goto numeric;
		case expr_binary_and:
			binop_name = "binary-and";
			goto numeric;
		case expr_binary_or:
			binop_name = "binary-or";
			goto numeric;
		case expr_binary_xor:
			binop_name = "binary-xor";
		numeric:
			if ((rhs_context != context_numeric) &&
			    (rhs_context != context_any))
				parse_error(cfile,
					    "expecting numeric expressions");
			break;

		default:
			break;
		}
	}

	/* Now, if we didn't find a binary operator, we're done parsing
	   this subexpression, so combine it with the preceding binary
	   operator and return the result. */
	if (next_op == expr_none) {
		tmp = createMap();
		tmp->skip = ISC_TRUE;
		mapSet(expr, tmp, binop_name);
		/* All the binary operators' data union members
		   are the same, so we'll cheat and use the member
		   for the equals operator. */
		mapSet(tmp, lhs, "left");
		mapSet(tmp, rhs, "right");
		return ISC_TRUE;;
	}

	/* Eat the operator token - we now know it was a binary operator... */
	skip_token(&val, NULL, cfile);

	/* Now combine the LHS and the RHS using binop. */
	tmp = createMap();
	tmp->skip = ISC_TRUE;
	
	/* Store the LHS and RHS. */
	mapSet(tmp, lhs, "left");
	mapSet(tmp, rhs, "right");

	lhs = createMap();
	mapSet(lhs, tmp, binop_name);
	
	tmp = NULL;
	rhs = NULL;

	binop = next_op;
	goto new_rhs;
}	

isc_boolean_t
parse_option_data(struct element *expr,
		  struct parse *cfile,
		  struct option *option)
{
	const char *val;
	enum dhcp_token token;
	unsigned len;
	struct string *data;
	struct string *saved;
	struct string *item;
	struct element *elem;
	struct comment *comment;
	isc_boolean_t canon_bool = ISC_FALSE;
	isc_boolean_t has_ignore = ISC_FALSE;

	data = makeString(0, NULL);
	saved = makeString(0, NULL);

	for (;;) {
		token = peek_token(&val, NULL, cfile);
		if (token == END_OF_FILE)
			parse_error(cfile, "unexpected end of file");
		if (token == SEMI)
			break;
		if (token == COMMA) {
			skip_token(&val, NULL, cfile);
			appendString(data, ", ");
			appendString(saved, ",");
			continue;
		}
		skip_token(&val, &len, cfile);
		item = makeString(len, val);
		concatString(saved, item);
		/* Kea todo: handle ISC DHCP binary format */
		if (is_identifier(token)) {
			if ((len == 3) && (memcmp(val, "off", 3) == 0)) {
				val = "false";
				len = 5;
				canon_bool = ISC_TRUE;
			} else if (token == ON) {
				val = "true";
				len = 4;
				canon_bool = ISC_TRUE;
			} else if (token == IGNORE)
				has_ignore = ISC_TRUE;
		}
		item = makeString(len, val);
		concatString(data, item);
	}
				
	if (canon_bool) {
		comment = createComment("/// canonized booleans to "
					" lowercase true or false");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
	}
	if (has_ignore) {
		comment = createComment("/// 'ignore' pseudo-boolean is used");
		TAILQ_INSERT_TAIL(&expr->comments, comment);
		expr->skip = ISC_TRUE;
		cfile->issue_counter++;
	}

	if (canon_bool || has_ignore) {
		elem = createString(saved);
		elem->skip = ISC_TRUE;
		mapSet(expr, elem, "original-data");
	}
	mapSet(expr, createString(data), "data");

        return ISC_TRUE;
}

/* option-statement :== identifier DOT identifier <syntax> SEMI
		      | identifier <syntax> SEMI

   Option syntax is handled specially through format strings, so it
   would be painful to come up with BNF for it.   However, it always
   starts as above and ends in a SEMI. */

isc_boolean_t
parse_option_statement(struct element *result,
		       struct parse *cfile,
		       struct option *option,
		       enum statement_op op)
{
	const char *val;
	enum dhcp_token token;
	struct element *expr;
	struct element *opt_data;
	struct element *opt_data_list;
	isc_boolean_t lose;
	size_t where;

	if (option->space == space_lookup("server"))
		return parse_config_statement(result, cfile, option, op);

	opt_data = createMap();
	TAILQ_CONCAT(&opt_data->comments, &cfile->comments);
	mapSet(opt_data,
	       createString(makeString(-1, option->space->name)), "space");
	mapSet(opt_data, createString(makeString(-1, option->name)), "name");
	mapSet(opt_data, createInt(option->code), "code");
	if (option->status == kea_unknown) {
		opt_data->skip = ISC_TRUE;
		cfile->issue_counter++;
	}
	if (op != supersede_option_statement) {
		struct comment *comment;

		comment = createComment("/// Kea does not support "
					"option data set variants");
		TAILQ_INSERT_TAIL(&opt_data->comments, comment);
	}

	token = peek_token(&val, NULL, cfile);
	/* We should keep a list of defined empty options */
	if ((token == SEMI) && (option->format[0] != 'Z')) {
		/* Eat the semicolon... */
		/*
		 * XXXSK: I'm not sure why we should ever get here, but we 
		 * 	  do during our startup. This confuses things if
		 * 	  we are parsing a zero-length option, so don't
		 * 	  eat the semicolon token in that case.
		 */
		skip_token(&val, NULL, cfile);
	} else if (token == EQUAL) {
		/* Eat the equals sign. */
		skip_token(&val, NULL, cfile);

		/* Parse a data expression and use its value for the data. */
		expr = createMap();
		if (!parse_data_expression(expr, cfile, &lose)) {
			/* In this context, we must have an executable
			   statement, so if we found something else, it's
			   still an error. */
			if (!lose)
				parse_error(cfile,
					    "expecting a data expression.");
			return ISC_FALSE;
		}
		mapSet(opt_data, createBool(ISC_FALSE), "csv-format");
		/* Stringify scalar expressions */
		if (expr->type == ELEMENT_BOOLEAN) {
			if (boolValue(expr))
				resetString(expr, makeString(-1, "true"));
			else
				resetString(expr, makeString(-1, "false"));
		} else if (expr->type == ELEMENT_INTEGER) {
			char buf[80];

			snprintf(buf, sizeof(buf), "%lld",
				 (long long)intValue(expr));
			resetString(expr, makeString(-1, buf));
		}
		if (expr->type == ELEMENT_STRING)
			mapSet(opt_data, expr, "data");
		else {
			opt_data->skip = ISC_TRUE;
			cfile->issue_counter++;
			mapSet(opt_data, expr, "expression");
		}
	} else {
		if (!parse_option_data(opt_data, cfile, option))
			return ISC_FALSE;
	}

	parse_semi(cfile);

	if (result != NULL) {
		opt_data->skip = ISC_TRUE;
		mapSet(result, opt_data, "option");
		return ISC_TRUE;
	}

	for (where = cfile->stack_top; where > 0; --where) {
		if (cfile->stack[where]->kind == PARAMETER)
			continue;
		if ((local_family == AF_INET) &&
		    (cfile->stack[where]->kind == POOL_DECL))
			continue;
		break;
	}

	opt_data_list = mapGet(cfile->stack[where], "option-data");
	if (opt_data_list == NULL) {
		opt_data_list = createList();
		mapSet(cfile->stack[where], opt_data_list, "option-data");
	}
	listPush(opt_data_list, opt_data);

	return ISC_TRUE;
}

/* Specialized version of parse_option_data working on config
 * options which are scalar (I6LSBtTfUXdNxxx.) only. */

isc_boolean_t
parse_config_data(struct element *expr,
		  struct parse *cfile,
		  struct option *option)
{
	const char *val;
	enum dhcp_token token;
	struct string *data;
	struct element *elem;
	unsigned len;
	uint32_t u32;
	uint16_t u16;
	uint8_t u8;

	token = peek_token(&val, NULL, cfile);

	if (token == END_OF_FILE)
		parse_error(cfile, "unexpected end of file");
	if (token == SEMI)
		parse_error(cfile, "empty config option");
	if (token == COMMA)
		parse_error(cfile, "multiple value config option");

	/* from parse_option_token */

	switch (option->format[0]) {
	case 'U': /* universe */
		token = next_token(&val, &len, cfile);
		if (!is_identifier(token))
			parse_error(cfile, "expecting identifier.");
		elem = createString(makeString(len, val));
		break;

	case 'X': /* string or binary */
		token = next_token(&val, &len, cfile);
		if (token == NUMBER_OR_NAME || token == NUMBER)
			data = parse_cshl(cfile);
		else if (token == STRING)
			data = makeString(len, val);
		else
			parse_error(cfile, "expecting string "
				    "or hexadecimal data.");
		elem = createString(data);
		break;

	case 'd': /* FQDN */
		data = parse_host_name(cfile);
		if (data == NULL)
			parse_error(cfile, "not a valid domain name.");
		elem = createString(data);
		break;

	case 't': /* text */
		token = next_token(&val, &len, cfile);
		elem = createString(makeString(len, val));
		break;

	case 'N': /* enumeration */
		token = next_token(&val, &len, cfile);
		if (!is_identifier(token))
			parse_error(cfile, "identifier expected");
		elem = createString(makeString(len, val));
		break;

	case 'I': /* IP address or hostname. */
		data = parse_ip_addr_or_hostname(cfile, ISC_FALSE);
		if (data == NULL)
			parse_error(cfile, "expecting IP address of hostname");
		elem = createString(data);
		break;

	case '6': /* IPv6 address. */
		data = parse_ip6_addr_txt(cfile);
		if (data == NULL)
			parse_error(cfile, "expecting IPv6 address");
		elem = createString(data);
		break;

	case 'T': /* Lease interval. */
		token = next_token(&val, NULL, cfile);
		if (token != INFINITE)
                        goto check_number;
		elem = createInt(-1);
		break;

	case 'L':  /* Unsigned 32-bit integer... */
		token = next_token(&val, NULL, cfile);
	check_number:
		if ((token != NUMBER) && (token != NUMBER_OR_NAME))
			parse_error(cfile, "expecting number.");
		convert_num(cfile, (unsigned char *)&u32, val, 0, 32);
		elem = createInt(ntohl(u32));
		break;

	case 'S': /* Unsigned 16-bit integer. */
		token = next_token(&val, NULL, cfile);
		if ((token != NUMBER) && (token != NUMBER_OR_NAME))
                        parse_error(cfile, "expecting number.");
		convert_num(cfile, (unsigned char *)&u16, val, 0, 16);
		elem = createInt(ntohs(u16));
		break;

	case 'B': /* Unsigned 8-bit integer. */
		token = next_token(&val, NULL, cfile);
                if ((token != NUMBER) && (token != NUMBER_OR_NAME))
                        parse_error(cfile, "expecting number.");
                convert_num(cfile, (unsigned char *)&u8, val, 0, 8);
		elem = createInt(ntohs(u8));
		break;

	case 'f':
		token = next_token(&val, NULL, cfile);
		if (!is_identifier(token))
			parse_error(cfile, "expecting boolean.");
		if ((strcasecmp(val, "true") == 0) ||
		    (strcasecmp(val, "on") == 0))
			elem = createBool(ISC_TRUE);
		else if ((strcasecmp(val, "false") == 0) ||
			 (strcasecmp(val, "off") == 0))
			elem = createBool(ISC_FALSE);
		else if (strcasecmp(val, "ignore") == 0) {
			elem = createNull();
			elem->skip = ISC_TRUE;
		} else
			parse_error(cfile, "expecting boolean.");
		break;

	default:
		parse_error(cfile, "Bad format '%c' in parse_config_data.",
			    option->format[0]);
	}
				
	mapSet(expr, elem, "value");

        return ISC_TRUE;
}

/* Specialized version of parse_option_statement for config options */

isc_boolean_t
parse_config_statement(struct element *result,
		       struct parse *cfile,
		       struct option *option,
		       enum statement_op op)
{
	const char *val;
	enum dhcp_token token;
	struct comments *comments;
	struct element *expr;
	struct element *config;
	struct element *config_list;
	isc_boolean_t lose;
	size_t where;

	config = createMap();
	TAILQ_CONCAT(&config->comments, &cfile->comments);
	comments = get_config_comments(option->code);
	TAILQ_CONCAT(&config->comments, comments);
	mapSet(config, createString(makeString(-1, option->name)), "name");
	mapSet(config, createInt(option->code), "code");
	if (option->status == kea_unknown) {
		config->skip = ISC_TRUE;
		cfile->issue_counter++;
	}
	if (op != supersede_option_statement) {
		struct comment *comment;

		comment = createComment("/// Kea does not support "
					"option data set variants");
		TAILQ_INSERT_TAIL(&config->comments, comment);
	}

	token = peek_token(&val, NULL, cfile);
	/* We should keep a list of defined empty options */
	if ((token == SEMI) && (option->format[0] != 'Z')) {
		/* Eat the semicolon... */
		/*
		 * XXXSK: I'm not sure why we should ever get here, but we 
		 * 	  do during our startup. This confuses things if
		 * 	  we are parsing a zero-length option, so don't
		 * 	  eat the semicolon token in that case.
		 */
		skip_token(&val, NULL, cfile);
	} else if (token == EQUAL) {
		/* Eat the equals sign. */
		skip_token(&val, NULL, cfile);

		/* Parse a data expression and use its value for the data. */
		expr = createMap();
		if (!parse_data_expression(expr, cfile, &lose)) {
			/* In this context, we must have an executable
			   statement, so if we found something else, it's
			   still an error. */
			if (!lose)
				parse_error(cfile,
					    "expecting a data expression.");
			return ISC_FALSE;
		}
		mapSet(config, expr, "value");
	} else {
		if (!parse_config_data(config, cfile, option))
			return ISC_FALSE;
	}

	parse_semi(cfile);

	if (result != NULL) {
		config->skip = ISC_TRUE;
		mapSet(result, config, "config");
		return ISC_TRUE;
	}

	for (where = cfile->stack_top; where > 0; --where) {
		if ((cfile->stack[where]->kind == PARAMETER) ||
		    (cfile->stack[where]->kind == POOL_DECL))
			continue;
		break;
	}

	if (option->status != special) {
		config_list = mapGet(cfile->stack[where], "config");
		if (config_list == NULL) {
			config_list = createList();
			config_list->skip = ISC_TRUE;
			mapSet(cfile->stack[where], config_list, "config");
		}
		listPush(config_list, config);
		return ISC_TRUE;
	}

	/* deal with all special cases */

	switch (option->code) {
	case 1: /* default-lease-time */
		config_valid_lifetime(config, cfile);
		break;
	case 15: /* filename */
		config_file(config, cfile);
		/* Kea todo: DHCPv4 file (vs boot-file-name option) */
		break;
	case 16: /* server-name */
		config_sname(config, cfile);
		break;
	case 17: /* next-server */
		config_next_server(config, cfile);
		break;
	case 18: /* authoritative */
		parse_error(cfile, "authoritative is a statement, "
			    "here it is used as a config option");
	case 23: /* ddns-domainname */
		config_qualifying_suffix(config, cfile);
		break;
	case 30: /* ddns-updates */
		config_enable_updates(config, cfile);
		break;
	case 35: /* local-address */
		config_local_address(config, cfile);
		break;
	case 39: /* ddns-update-style */
		config_ddns_update_style(config, cfile);
		break;
	case 53: /* preferred-lifetime */
		config_preferred_lifetime(config, cfile);
		break;
	case 82: /* ignore-client-uids */
		config_match_client_id(config, cfile);
		break;
	case 85: /* echo-client-id */
		config_echo_client_id(config, cfile);
		break;
	case 88: /* dhcpv6-set-tee-times */
		config_timers(config, cfile);
		break;
	case 89: /* abandon-lease-time */
		config_expired_leases_processing(config, cfile);
		break;
	default:
		parse_error(cfile, "unsupported config option %s (%u)",
			    option->name, option->code);
	}

	return ISC_TRUE;
}

static void
config_valid_lifetime(struct element *config, struct parse *cfile)
{
	struct element *value;
	struct comment *comment;
	size_t where;
	isc_boolean_t pop_from_pool = ISC_FALSE;

	value = mapGet(config, "value");

	for (where = cfile->stack_top; where > 0; --where) {
		int kind = cfile->stack[where]->kind;

		if (kind == PARAMETER)
			continue;
		if ((kind == ROOT_GROUP) ||
		    (kind == SHARED_NET_DECL) ||
		    (kind == SUBNET_DECL) ||
		    (kind == GROUP_DECL))
			break;
		if (kind == POOL_DECL) {
			pop_from_pool = ISC_TRUE;
			continue;
		}
		comment = createComment("/// valid-lifetime in unsupported "
					"scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
		value->skip = ISC_TRUE;
		cfile->issue_counter++;
		break;
	}
	if (pop_from_pool) {
		comment= createComment("/// valid-lifetime moved from "
				       "an internal pool scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
	}
	mapSet(cfile->stack[where], value, "valid-lifetime");
}

static void
config_file(struct element *config, struct parse *cfile)
{
	struct element *value;
	struct comment *comment;
	size_t where;
	isc_boolean_t popped = ISC_FALSE;

	if (local_family != AF_INET)
		parse_error(cfile, "boot-file-name is DHCPv4 only");

	value = mapGet(config, "value");

	for (where = cfile->stack_top; where > 0; --where) {
		int kind = cfile->stack[where]->kind;

		if (kind == PARAMETER)
			continue;
		if ((kind == HOST_DECL) ||
		    (kind == CLASS_DECL) ||
		    (kind == GROUP_DECL))
			break;
		if (kind == ROOT_GROUP) {
			popped = ISC_TRUE;
			break;
		}
	}
	if (popped) {
		comment = createComment("/// boot-file-name was defined in "
					"an unsupported scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
		value->skip = ISC_TRUE;
		cfile->issue_counter++;
	}
	mapSet(cfile->stack[where], value, "boot-file-name");
}

static void
config_sname(struct element *config, struct parse *cfile)
{
	struct element *value;
	struct comment *comment;
	size_t where;
	isc_boolean_t popped = ISC_FALSE;

	if (local_family != AF_INET)
		parse_error(cfile, "server-hostname is DHCPv4 only");

	value = mapGet(config, "value");

	for (where = cfile->stack_top; where > 0; --where) {
		int kind = cfile->stack[where]->kind;

		if (kind == PARAMETER)
			continue;
		if ((kind == HOST_DECL) ||
		    (kind == CLASS_DECL) ||
		    (kind == GROUP_DECL))
			break;
		if (kind == ROOT_GROUP) {
			popped = ISC_TRUE;
			break;
		}
	}
	if (popped) {
		comment = createComment("/// server-hostname was defined in "
					"an unsupported scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
		value->skip = ISC_TRUE;
		cfile->issue_counter++;
	}
	mapSet(cfile->stack[where], value, "server-hostname");
}

static void
config_next_server(struct element *config, struct parse *cfile)
{
	struct element *value;
	struct comment *comment;
	size_t where;
	isc_boolean_t popped = ISC_FALSE;

	if (local_family != AF_INET)
		parse_error(cfile, "next-server is DHCPv4 only");

	value = mapGet(config, "value");

	for (where = cfile->stack_top; where > 0; --where) {
		int kind = cfile->stack[where]->kind;

		if (kind == PARAMETER)
			continue;
		if ((kind == ROOT_GROUP) ||
		    (kind == HOST_DECL) ||
		    (kind == CLASS_DECL) ||
		    (kind == GROUP_DECL))
			break;
		popped = ISC_TRUE;
	}
	if (popped) {
		comment = createComment("/// next-server moved from "
					"an internal unsupported scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
	}
	mapSet(cfile->stack[where], value, "next-server");
}

static void
config_qualifying_suffix(struct element *config, struct parse *cfile)
{
	/* Kea todo */
}

static void
config_enable_updates(struct element *config, struct parse *cfile)
{
	/* Kea todo */ /* check scope */
}

static void
config_local_address(struct element *config, struct parse *cfile)
{
	/* Kea todo */
}

static void
config_ddns_update_style(struct element *config, struct parse *cfile)
{
	/* Kea todo */ /* check standard/none and fails on others */
}

static void
config_preferred_lifetime(struct element *config, struct parse *cfile)
{
	struct element *value;
	struct comment *comment;
	size_t where;
	isc_boolean_t pop_from_pool = ISC_FALSE;

	if (local_family != AF_INET6)
		parse_error(cfile, "preferred-lifetime is DHCPv6 only");

	value = mapGet(config, "value");

	for (where = cfile->stack_top; where > 0; --where) {
		int kind = cfile->stack[where]->kind;

		if (kind == PARAMETER)
			continue;
		if ((kind == ROOT_GROUP) ||
		    (kind == SHARED_NET_DECL) ||
		    (kind == SUBNET_DECL) ||
		    (kind == GROUP_DECL))
			break;
		if (kind == POOL_DECL) {
			pop_from_pool = ISC_TRUE;
			continue;
		}
		comment = createComment("/// preferred-lifetime in "
					"unsupported scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
		value->skip = ISC_TRUE;
		cfile->issue_counter++;
		break;
	}
	if (pop_from_pool) {
		comment= createComment("/// preferred-lifetime moved from "
				       "an internal pool scope");
		TAILQ_INSERT_TAIL(&value->comments, comment);
	}
	mapSet(cfile->stack[where], value, "preferred-lifetime");
}

static void
config_match_client_id(struct element *config, struct parse *cfile)
{
	/* Kea todo */
	if (local_family != AF_INET)
		parse_error(cfile, "match-client-id is DHCPv4 only");
	/* global and subnet4 */
}

static void
config_echo_client_id(struct element *config, struct parse *cfile)
{
	/* Kea todo */
	if (local_family != AF_INET)
		parse_error(cfile, "echo-client-id is DHCPv4 only");
	/* global only */
}

static void
config_timers(struct element *config, struct parse *cfile)
{
	/* Kea todo */
}

static void
config_expired_leases_processing(struct element *config, struct parse *cfile)
{
	/* Kea todo */
}

/* parse_error moved to keama.c */

/* From omapi/convert.c */
/*
static uint32_t
getULong(const unsigned char *buf)
{
	uint32_t ibuf;

	memcpy(&ibuf, buf, sizeof(uint32_t));
	return ntohl(ibuf);
}

static int32_t
getLong(const unsigned char *buf)
{
	int32_t ibuf;

	memcpy(&ibuf, buf, sizeof(int32_t));
	return ntohl(ibuf);
}

static uint32_t
getUShort(const unsigned char *buf)
{
	unsigned short ibuf;

	memcpy(&ibuf, buf, sizeof(uint16_t));
	return ntohs(ibuf);
}

static int32_t
getShort(const unsigned char *buf)
{
	short ibuf;

	memcpy(&ibuf, buf, sizeof(int16_t));
	return ntohs(ibuf);
}

static uint32_t
getUChar(const unsigned char *obuf)
{
	return obuf[0];
}
*/
static void
putULong(unsigned char *obuf, uint32_t val)
{
	uint32_t tmp = htonl(val);
	memcpy(obuf, &tmp, sizeof(tmp));
}

static void
putLong(unsigned char *obuf, int32_t val)
{
	int32_t tmp = htonl(val);
	memcpy(obuf, &tmp, sizeof(tmp));
}

static void
putUShort(unsigned char *obuf, uint32_t val)
{
	uint16_t tmp = htons(val);
	memcpy(obuf, &tmp, sizeof(tmp));
}

static void
putShort(unsigned char *obuf, int32_t val)
{
	int16_t tmp = htons(val);
	memcpy(obuf, &tmp, sizeof(tmp));
}
/*
static void
putUChar(unsigned char *obuf, uint32_t val)
{
	*obuf = val;
}
*/
/* From common/tree.c */

isc_boolean_t
is_boolean_expression(struct element *expr)
{
	return ((expr->type == ELEMENT_BOOLEAN) ||
		mapContains(expr, "check") ||
		mapContains(expr, "exists") ||
		mapContains(expr, "variable-exists") ||
		mapContains(expr, "equal") ||
		mapContains(expr, "not-equal") ||
		mapContains(expr, "regex-match") ||
		mapContains(expr, "iregex-match") ||
		mapContains(expr, "and") ||
		mapContains(expr, "or") ||
		mapContains(expr, "not") ||
		mapContains(expr, "known") ||
		mapContains(expr, "static"));
}

isc_boolean_t
is_data_expression(struct element *expr)
{
	return ((expr->type == ELEMENT_INTEGER) ||
		(expr->type == ELEMENT_STRING) ||
		mapContains(expr, "substring") ||
		mapContains(expr, "suffix") ||
		mapContains(expr, "lowercase") ||
		mapContains(expr, "uppercase") ||
		mapContains(expr, "option") ||
		mapContains(expr, "hardware") ||
		mapContains(expr, "packet") ||
		mapContains(expr, "concat") ||
		mapContains(expr, "encapsulate") ||
		mapContains(expr, "encode-int8") ||
		mapContains(expr, "encode-int16") ||
		mapContains(expr, "encode-int32") ||
		mapContains(expr, "gethostbyname") ||
		mapContains(expr, "binary-to-ascii") ||
		mapContains(expr, "filename") ||
		mapContains(expr, "server-name") ||
		mapContains(expr, "reverse") ||
		mapContains(expr, "pick-first-value") ||
		mapContains(expr, "host-decl-name") ||
		mapContains(expr, "leased-address") ||
		mapContains(expr, "config-option") ||
		mapContains(expr, "null") ||
		mapContains(expr, "gethostname") ||
	        mapContains(expr, "v6relay"));
}

isc_boolean_t
is_numeric_expression(struct element *expr)
{
	return ((expr->type == ELEMENT_INTEGER) ||
		mapContains(expr, "extract-int8") ||
		mapContains(expr, "extract-int16") ||
		mapContains(expr, "extract-int32") ||
		mapContains(expr, "lease-time") ||
		mapContains(expr, "add") ||
		mapContains(expr, "subtract") ||
		mapContains(expr, "multiply") ||
		mapContains(expr, "divide") ||
		mapContains(expr, "remainder") ||
		mapContains(expr, "binary-and") ||
		mapContains(expr, "binary-or") ||
		mapContains(expr, "binary-xor") ||
		mapContains(expr, "client-state"));
}
/*
static isc_boolean_t
is_compound_expression(struct element *expr)
{
	return (mapContains(expr, "substring") ||
		mapContains(expr, "suffix") ||
		mapContains(expr, "option") ||
		mapContains(expr, "concat") ||
		mapContains(expr, "encode-int8") ||
		mapContains(expr, "encode-int16") ||
		mapContains(expr, "encode-int32") ||
		mapContains(expr, "binary-to-ascii") ||
		mapContains(expr, "reverse") ||
		mapContains(expr, "pick-first-value") ||
		mapContains(expr, "config-option") ||
		mapContains(expr, "extract-int8") ||
		mapContains(expr, "extract-int16") ||
		mapContains(expr, "extract-int32") ||
		mapContains(expr, "v6relay"));
}
*/
static enum expression_context
op_context(enum expr_op op)
{
	switch (op) {
/* XXX Why aren't these specific? */
	case expr_none:
	case expr_match:
	case expr_static:
	case expr_check:
	case expr_substring:
	case expr_suffix:
	case expr_lcase:
	case expr_ucase:
	case expr_concat:
	case expr_encapsulate:
	case expr_host_lookup:
	case expr_not:
	case expr_option:
	case expr_hardware:
	case expr_packet:
	case expr_const_data:
	case expr_extract_int8:
	case expr_extract_int16:
	case expr_extract_int32:
	case expr_encode_int8:
	case expr_encode_int16:
	case expr_encode_int32:
	case expr_const_int:
	case expr_exists:
	case expr_variable_exists:
	case expr_known:
	case expr_binary_to_ascii:
	case expr_reverse:
	case expr_filename:
	case expr_sname:
	case expr_pick_first_value:
	case expr_host_decl_name:
	case expr_config_option:
	case expr_leased_address:
	case expr_lease_time:
	case expr_null:
	case expr_variable_reference:
	case expr_ns_add:
	case expr_ns_delete:
	case expr_ns_exists:
	case expr_ns_not_exists:
	case expr_dns_transaction:
	case expr_arg:
	case expr_funcall:
	case expr_function:
	case expr_gethostname:
	case expr_v6relay:
	case expr_concat_dclist:
		return context_any;

	case expr_equal:
	case expr_not_equal:
	case expr_regex_match:
	case expr_iregex_match:
		return context_data;

	case expr_and:
		return context_boolean;

	case expr_or:
		return context_boolean;

	case expr_add:
	case expr_subtract:
	case expr_multiply:
	case expr_divide:
	case expr_remainder:
	case expr_binary_and:
	case expr_binary_or:
	case expr_binary_xor:
	case expr_client_state:
		return context_numeric;
	}
	return context_any;
}

static int
op_val(enum expr_op op)
{
	switch (op) {
	case expr_none:
	case expr_match:
	case expr_static:
	case expr_check:
	case expr_substring:
	case expr_suffix:
	case expr_lcase:
	case expr_ucase:
	case expr_concat:
	case expr_encapsulate:
	case expr_host_lookup:
	case expr_not:
	case expr_option:
	case expr_hardware:
	case expr_packet:
	case expr_const_data:
	case expr_extract_int8:
	case expr_extract_int16:
	case expr_extract_int32:
	case expr_encode_int8:
	case expr_encode_int16:
	case expr_encode_int32:
	case expr_const_int:
	case expr_exists:
	case expr_variable_exists:
	case expr_known:
	case expr_binary_to_ascii:
	case expr_reverse:
	case expr_filename:
	case expr_sname:
	case expr_pick_first_value:
	case expr_host_decl_name:
	case expr_config_option:
	case expr_leased_address:
	case expr_lease_time:
	case expr_dns_transaction:
	case expr_null:
	case expr_variable_reference:
	case expr_ns_add:
	case expr_ns_delete:
	case expr_ns_exists:
	case expr_ns_not_exists:
	case expr_arg:
	case expr_funcall:
	case expr_function:
	/* XXXDPN: Need to assign sane precedences to these. */
	case expr_binary_and:
	case expr_binary_or:
	case expr_binary_xor:
	case expr_client_state:
	case expr_gethostname:
	case expr_v6relay:
	case expr_concat_dclist:
		return 100;

	case expr_equal:
	case expr_not_equal:
	case expr_regex_match:
	case expr_iregex_match:
		return 4;

	case expr_or:
	case expr_and:
		return 3;

	case expr_add:
	case expr_subtract:
		return 2;

	case expr_multiply:
	case expr_divide:
	case expr_remainder:
		return 1;
	}
	return 100;
}

static int
op_precedence(enum expr_op op1, enum expr_op op2)
{
	return op_val(op1) - op_val(op2);
}

static enum expression_context
expression_context(struct element *expr)
{
	if (is_data_expression(expr))
		return context_data;
	if (is_numeric_expression(expr))
		return context_numeric;
	if (is_boolean_expression(expr))
		return context_boolean;
	return context_any;
}

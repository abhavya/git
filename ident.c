/*
 * ident.c
 *
 * create git identifier lines of the form "name <email> date"
 *
 * Copyright (C) 2005 Linus Torvalds
 */
#include "cache.h"

static char git_default_date[50];

#ifdef NO_GECOS_IN_PWENT
#define get_gecos(ignored) "&"
#else
#define get_gecos(struct_passwd) ((struct_passwd)->pw_gecos)
#endif

static void copy_gecos(const struct passwd *w, char *name, size_t sz)
{
	char *src, *dst;
	size_t len, nlen;

	nlen = strlen(w->pw_name);

	/* Traditionally GECOS field had office phone numbers etc, separated
	 * with commas.  Also & stands for capitalized form of the login name.
	 */

	for (len = 0, dst = name, src = get_gecos(w); len < sz; src++) {
		int ch = *src;
		if (ch != '&') {
			*dst++ = ch;
			if (ch == 0 || ch == ',')
				break;
			len++;
			continue;
		}
		if (len + nlen < sz) {
			/* Sorry, Mr. McDonald... */
			*dst++ = toupper(*w->pw_name);
			memcpy(dst, w->pw_name + 1, nlen - 1);
			dst += nlen - 1;
			len += nlen;
		}
	}
	if (len < sz)
		name[len] = 0;
	else
		die("Your parents must have hated you!");

}

static int add_mailname_host(char *buf, size_t len)
{
	FILE *mailname;

	mailname = fopen("/etc/mailname", "r");
	if (!mailname) {
		if (errno != ENOENT)
			warning("cannot open /etc/mailname: %s",
				strerror(errno));
		return -1;
	}
	if (!fgets(buf, len, mailname)) {
		if (ferror(mailname))
			warning("cannot read /etc/mailname: %s",
				strerror(errno));
		fclose(mailname);
		return -1;
	}
	/* success! */
	fclose(mailname);
	return 0;
}

static void add_domainname(char *buf, size_t len)
{
	struct hostent *he;
	size_t namelen;
	const char *domainname;

	if (gethostname(buf, len)) {
		warning("cannot get host name: %s", strerror(errno));
		strlcpy(buf, "(none)", len);
		return;
	}
	namelen = strlen(buf);
	if (memchr(buf, '.', namelen))
		return;

	he = gethostbyname(buf);
	buf[namelen++] = '.';
	buf += namelen;
	len -= namelen;
	if (he && (domainname = strchr(he->h_name, '.')))
		strlcpy(buf, domainname + 1, len);
	else
		strlcpy(buf, "(none)", len);
}

static void copy_email(const struct passwd *pw)
{
	/*
	 * Make up a fake email address
	 * (name + '@' + hostname [+ '.' + domainname])
	 */
	size_t len = strlen(pw->pw_name);
	if (len > sizeof(git_default_email)/2)
		die("Your sysadmin must hate you!");
	memcpy(git_default_email, pw->pw_name, len);
	git_default_email[len++] = '@';

	if (!add_mailname_host(git_default_email + len,
				sizeof(git_default_email) - len))
		return;	/* read from "/etc/mailname" (Debian) */
	add_domainname(git_default_email + len,
			sizeof(git_default_email) - len);
}

static void setup_ident(const char **name, const char **emailp)
{
	struct passwd *pw = NULL;

	/* Get the name ("gecos") */
	if (!*name && !git_default_name[0]) {
		pw = getpwuid(getuid());
		if (!pw)
			die("You don't exist. Go away!");
		copy_gecos(pw, git_default_name, sizeof(git_default_name));
	}
	if (!*name)
		*name = git_default_name;

	if (!*emailp && !git_default_email[0]) {
		const char *email = getenv("EMAIL");

		if (email && email[0]) {
			strlcpy(git_default_email, email,
				sizeof(git_default_email));
			user_ident_explicitly_given |= IDENT_MAIL_GIVEN;
		} else {
			if (!pw)
				pw = getpwuid(getuid());
			if (!pw)
				die("You don't exist. Go away!");
			copy_email(pw);
		}
	}
	if (!*emailp)
		*emailp = git_default_email;

	/* And set the default date */
	if (!git_default_date[0])
		datestamp(git_default_date, sizeof(git_default_date));
}

static int add_raw(char *buf, size_t size, int offset, const char *str)
{
	size_t len = strlen(str);
	if (offset + len > size)
		return size;
	memcpy(buf + offset, str, len);
	return offset + len;
}

static int crud(unsigned char c)
{
	return  c <= 32  ||
		c == '.' ||
		c == ',' ||
		c == ':' ||
		c == ';' ||
		c == '<' ||
		c == '>' ||
		c == '"' ||
		c == '\\' ||
		c == '\'';
}

/*
 * Copy over a string to the destination, but avoid special
 * characters ('\n', '<' and '>') and remove crud at the end
 */
static int copy(char *buf, size_t size, int offset, const char *src)
{
	size_t i, len;
	unsigned char c;

	/* Remove crud from the beginning.. */
	while ((c = *src) != 0) {
		if (!crud(c))
			break;
		src++;
	}

	/* Remove crud from the end.. */
	len = strlen(src);
	while (len > 0) {
		c = src[len-1];
		if (!crud(c))
			break;
		--len;
	}

	/*
	 * Copy the rest to the buffer, but avoid the special
	 * characters '\n' '<' and '>' that act as delimiters on
	 * an identification line
	 */
	for (i = 0; i < len; i++) {
		c = *src++;
		switch (c) {
		case '\n': case '<': case '>':
			continue;
		}
		if (offset >= size)
			return size;
		buf[offset++] = c;
	}
	return offset;
}

/*
 * Reverse of fmt_ident(); given an ident line, split the fields
 * to allow the caller to parse it.
 * Signal a success by returning 0, but date/tz fields of the result
 * can still be NULL if the input line only has the name/email part
 * (e.g. reading from a reflog entry).
 */
int split_ident_line(struct ident_split *split, const char *line, int len)
{
	const char *cp;
	size_t span;
	int status = -1;

	memset(split, 0, sizeof(*split));

	split->name_begin = line;
	for (cp = line; *cp && cp < line + len; cp++)
		if (*cp == '<') {
			split->mail_begin = cp + 1;
			break;
		}
	if (!split->mail_begin)
		return status;

	for (cp = split->mail_begin - 2; line < cp; cp--)
		if (!isspace(*cp)) {
			split->name_end = cp + 1;
			break;
		}
	if (!split->name_end)
		return status;

	for (cp = split->mail_begin; cp < line + len; cp++)
		if (*cp == '>') {
			split->mail_end = cp;
			break;
		}
	if (!split->mail_end)
		return status;

	for (cp = split->mail_end + 1; cp < line + len && isspace(*cp); cp++)
		;
	if (line + len <= cp)
		goto person_only;
	split->date_begin = cp;
	span = strspn(cp, "0123456789");
	if (!span)
		goto person_only;
	split->date_end = split->date_begin + span;
	for (cp = split->date_end; cp < line + len && isspace(*cp); cp++)
		;
	if (line + len <= cp || (*cp != '+' && *cp != '-'))
		goto person_only;
	split->tz_begin = cp;
	span = strspn(cp + 1, "0123456789");
	if (!span)
		goto person_only;
	split->tz_end = split->tz_begin + 1 + span;
	return 0;

person_only:
	split->date_begin = NULL;
	split->date_end = NULL;
	split->tz_begin = NULL;
	split->tz_end = NULL;
	return 0;
}

static const char *env_hint =
"\n"
"*** Please tell me who you are.\n"
"\n"
"Run\n"
"\n"
"  git config --global user.email \"you@example.com\"\n"
"  git config --global user.name \"Your Name\"\n"
"\n"
"to set your account\'s default identity.\n"
"Omit --global to set the identity only in this repository.\n"
"\n";

const char *fmt_ident(const char *name, const char *email,
		      const char *date_str, int flag)
{
	static char buffer[1000];
	char date[50];
	int i;
	int error_on_no_name = (flag & IDENT_ERROR_ON_NO_NAME);
	int warn_on_no_name = (flag & IDENT_WARN_ON_NO_NAME);
	int name_addr_only = (flag & IDENT_NO_DATE);

	setup_ident(&name, &email);

	if (!*name) {
		struct passwd *pw;

		if ((warn_on_no_name || error_on_no_name) &&
		    name == git_default_name && env_hint) {
			fputs(env_hint, stderr);
			env_hint = NULL; /* warn only once */
		}
		if (error_on_no_name)
			die("empty ident %s <%s> not allowed", name, email);
		pw = getpwuid(getuid());
		if (!pw)
			die("You don't exist. Go away!");
		strlcpy(git_default_name, pw->pw_name,
			sizeof(git_default_name));
		name = git_default_name;
	}

	strcpy(date, git_default_date);
	if (!name_addr_only && date_str && date_str[0]) {
		if (parse_date(date_str, date, sizeof(date)) < 0)
			die("invalid date format: %s", date_str);
	}

	i = copy(buffer, sizeof(buffer), 0, name);
	i = add_raw(buffer, sizeof(buffer), i, " <");
	i = copy(buffer, sizeof(buffer), i, email);
	if (!name_addr_only) {
		i = add_raw(buffer, sizeof(buffer), i,  "> ");
		i = copy(buffer, sizeof(buffer), i, date);
	} else {
		i = add_raw(buffer, sizeof(buffer), i, ">");
	}
	if (i >= sizeof(buffer))
		die("Impossibly long personal identifier");
	buffer[i] = 0;
	return buffer;
}

const char *fmt_name(const char *name, const char *email)
{
	return fmt_ident(name, email, NULL, IDENT_ERROR_ON_NO_NAME | IDENT_NO_DATE);
}

const char *git_author_info(int flag)
{
	return fmt_ident(getenv("GIT_AUTHOR_NAME"),
			 getenv("GIT_AUTHOR_EMAIL"),
			 getenv("GIT_AUTHOR_DATE"),
			 flag);
}

const char *git_committer_info(int flag)
{
	if (getenv("GIT_COMMITTER_NAME"))
		user_ident_explicitly_given |= IDENT_NAME_GIVEN;
	if (getenv("GIT_COMMITTER_EMAIL"))
		user_ident_explicitly_given |= IDENT_MAIL_GIVEN;
	return fmt_ident(getenv("GIT_COMMITTER_NAME"),
			 getenv("GIT_COMMITTER_EMAIL"),
			 getenv("GIT_COMMITTER_DATE"),
			 flag);
}

int user_ident_sufficiently_given(void)
{
#ifndef WINDOWS
	return (user_ident_explicitly_given & IDENT_MAIL_GIVEN);
#else
	return (user_ident_explicitly_given == IDENT_ALL_GIVEN);
#endif
}

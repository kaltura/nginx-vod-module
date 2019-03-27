/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 * Copyright (C) 2018 Rafik Mikhael <rmikhael@ellation.com>
 *
 * This file was part of libass. Modified and updated for use with nginx-vod-module.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <float.h>
#include "ass_format.h"

#define ASS_STYLES_ALLOC 20
#define ASS_SIZE_MAX ((size_t)-1)
#define ass_atof(STR) (ass_strtod((STR),NULL))

static const unsigned char lowertab[] =
{
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
	0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
	0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
	0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x61,
	0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
	0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62,
	0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d,
	0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
	0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
	0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
	0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
	0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
	0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
	0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
	0xfd, 0xfe, 0xff
};
static int ass_strcasecmp(const char *s1, const char *s2)
{
	unsigned char a, b;

	do
	{
		a = lowertab[(unsigned char)*s1++];
		b = lowertab[(unsigned char)*s2++];
	} while (a && a == b);

	return a - b;
}
static int ass_strncasecmp(const char *s1, const char *s2, size_t n)
{
	unsigned char a, b;
	const char *last = s1 + n;

	do
	{
		a = lowertab[(unsigned char)*s1++];
		b = lowertab[(unsigned char)*s2++];
	} while (s1 < last && a && a == b);

	return a - b;
}

void skip_spaces(char **str)
{
	char *p = *str;
	while ((*p == ' ') || (*p == '\t'))
		++p;
	*str = p;
}

void rskip_spaces(char **str, char *limit)
{
	char *p = *str;
	while ((p > limit) && ((p[-1] == ' ') || (p[-1] == '\t')))
		--p;
	*str = p;
}

static inline int ass_isspace(int c)
{
	return	c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
			c == '\f' || c == '\r';
}

static inline int ass_isdigit(int c)
{
	return c >= '0' && c <= '9';
}


static
const size_t max_exponent = 511;/* Largest possible base 10 exponent.  Any
								 * exponent larger than this will already
								 * produce underflow or overflow, so there's
								 * no need to worry about additional digits.
								 */

static
const double powersOf10[] =		/* Table giving binary powers of 10.  Entry */
{								/* is 10^2^i.  Used to convert decimal */
	10.,						/* exponents into floating-point numbers. */
	100.,
	1.0e4,
	1.0e8,
	1.0e16,
	1.0e32,
	1.0e64,
	1.0e128,
	1.0e256
};

static
const double negPowOf10[] =		/* Table giving negative binary powers */
{								/* of 10.  Entry is 10^-2^i. */
	0.1,						/* Used to convert decimal exponents */
	0.01,						/* into floating-point numbers. */
	1.0e-4,
	1.0e-8,
	1.0e-16,
	1.0e-32,
	1.0e-64,
	1.0e-128,
	1.0e-256
};

/*
 *----------------------------------------------------------------------
 *
 * strtod --
 *
 * This procedure converts a floating-point number from an ASCII
 * decimal representation to internal double-precision format.
 *
 * Results:
 * The return value is the double-precision floating-point
 * representation of the characters in string.  If end_ptr isn't
 * NULL, then *end_ptr is filled in with the address of the
 * next character after the last one that was part of the
 * floating-point number.
 *
 * Side effects:
 * None.
 *
 *----------------------------------------------------------------------
 */

double
ass_strtod(
	const char *string, /* A decimal ASCII floating-point number,
						 * optionally preceded by white space.
						 * Must have form "-I.FE-X", where I is the
						 * integer part of the mantissa, F is the
						 * fractional part of the mantissa, and X
						 * is the exponent.  Either of the signs
						 * may be "+", "-", or omitted.  Either I
						 * or F may be omitted, or both.  The decimal
						 * point isn't necessary unless F is present.
						 * The "E" may actually be an "e".  E and X
						 * may both be omitted (but not just one).
						 */
	char **end_ptr		/* If non-NULL, store terminating character's
						 * address here. */
	)
{
	int sign, frac_exp_sign, exp_sign;
	double fraction, dbl_exp;
	const double *d;
	register const char *p;
	register int c;
	size_t exp = 0;		/* Exponent read from "EX" field. */
	size_t frac_exp;	/* Exponent that derives from the fractional
						 * part.  Under normal circumstatnces, it is
						 * the negative of the number of digits in F.
						 * However, if I is very long, the last digits
						 * of I get dropped (otherwise a long I with a
						 * large negative exponent could cause an
						 * unnecessary overflow on I alone).  In this
						 * case, frac_exp is incremented one for each
						 * dropped digit. */
	size_t mant_size;	/* Number of digits in mantissa. */
	size_t dec_pt;		/* Number of mantissa digits BEFORE decimal
						 * point. */
	size_t lead_zeros;	/* Number of leading zeros in mantissa. */
	const char *p_exp;	/* Temporarily holds location of exponent
						 * in string. */

	/*
	 * Strip off leading blanks and check for a sign.
	 */

	p = string;
	while (ass_isspace(*p))
	{
		p += 1;
	}
	if (*p == '-')
	{
		sign = 1;
		p += 1;
	}
	else
	{
		if (*p == '+')
		{
			p += 1;
		}
		sign = 0;
	}

	/*
	 * Count the number of digits in the mantissa (including the decimal
	 * point), and also locate the decimal point.
	 */

	dec_pt = -1;
	lead_zeros = -1;
	for (mant_size = 0; ; mant_size += 1)
	{
		c = *p;
		if (!ass_isdigit(c)) {
			if ((c != '.') || (dec_pt != (size_t) -1))
			{
				break;
			}
			dec_pt = mant_size;
		} else if ((c != '0') && (lead_zeros == (size_t) -1))
		{
			lead_zeros = mant_size;
		}
		p += 1;
	}

	/*
	 * Now suck up the digits in the mantissa.  Use two integers to
	 * collect 9 digits each (this is faster than using floating-point).
	 * If the mantissa has more than 18 digits, ignore the extras, since
	 * they can't affect the value anyway.
	 */

	if (lead_zeros == (size_t) -1)
	{
		lead_zeros = mant_size;
	}
	p_exp  = p;
	p -= mant_size - lead_zeros;
	if (dec_pt == (size_t) -1)
	{
		dec_pt = mant_size;
	} else {
		mant_size -= 1;	  /* One of the digits was the point. */
		if (dec_pt < lead_zeros)
		{
			lead_zeros -= 1;
		}
	}
	if (mant_size - lead_zeros > 18)
	{
		mant_size = lead_zeros + 18;
	}
	if (dec_pt < mant_size)
	{
		frac_exp_sign = 1;
		frac_exp = mant_size - dec_pt;
	}
	else
	{
		frac_exp_sign = 0;
		frac_exp = dec_pt - mant_size;
	}
	if (mant_size == 0)
	{
		fraction = 0.0;
		p = string;
		goto done;
	}
	else
	{
		int frac1, frac2, m;
		mant_size -= lead_zeros;
		m = mant_size;
		frac1 = 0;
		for ( ; m > 9; m -= 1)
		{
			c = *p;
			p += 1;
			if (c == '.') {
				c = *p;
				p += 1;
			}
			frac1 = 10*frac1 + (c - '0');
		}
		frac2 = 0;
		for (; m > 0; m -= 1)
		{
			c = *p;
			p += 1;
			if (c == '.') {
				c = *p;
				p += 1;
			}
			frac2 = 10*frac2 + (c - '0');
		}
		fraction = (1.0e9 * frac1) + frac2;
	}

	/*
	 * Skim off the exponent.
	 */

	p = p_exp;
	if ((*p == 'E') || (*p == 'e'))
	{
		size_t expLimit;	/* If exp > expLimit, appending another digit
						 	 * to exp is guaranteed to make it too large.
						 	 * If exp == expLimit, this may depend on
						 	 * the exact digit, but in any case exp with
						 	 * the digit appended and frac_exp added will
						 	 * still fit in size_t, even if it does
						 	 * exceed max_exponent. */
		int expWraparound = 0;
		p += 1;
		if (*p == '-')
		{
			exp_sign = 1;
			p += 1;
		} else {
			if (*p == '+')
			{
				p += 1;
			}
			exp_sign = 0;
		}
		if (exp_sign == frac_exp_sign)
		{
			if (max_exponent < frac_exp)
			{
				expLimit = 0;
			}
			else
			{
				expLimit = (max_exponent - frac_exp) / 10;
			}
		}
		else
		{
			expLimit = frac_exp / 10 + (frac_exp % 10 + max_exponent) / 10;
		}
		while (ass_isdigit(*p))
		{
			if ((exp > expLimit) || expWraparound)
			{
				do
				{
					p += 1;
				} while (ass_isdigit(*p));
				goto exp_overflow;
			} else if (exp > ((size_t) -1 - (*p - '0')) / 10)
			{
				expWraparound = 1;
			}
			exp = exp * 10 + (*p - '0');
			p += 1;
		}
		if (exp_sign == frac_exp_sign)
		{
			exp = frac_exp + exp;
		}
		else if ((frac_exp <= exp) || expWraparound)
		{
			exp = exp - frac_exp;
		}
		else
		{
			exp = frac_exp - exp;
			exp_sign = frac_exp_sign;
		}
	}
	else
	{
		exp = frac_exp;
		exp_sign = frac_exp_sign;
	}

	/*
	 * Generate a floating-point number that represents the exponent.
	 * Do this by processing the exponent one bit at a time to combine
	 * many powers of 2 of 10. Then combine the exponent with the
	 * fraction.
	 */

	if (exp > max_exponent)
	{
exp_overflow:
		exp = max_exponent;
		if (fraction != 0.0)
		{
			errno = ERANGE;
		}
	}
	/* Prefer positive powers of 10 for increased precision, especially
	 * for small powers that are represented exactly in floating-point. */
	if ((exp <= DBL_MAX_10_EXP) || !exp_sign)
	{
		d = powersOf10;
	}
	else
	{
		/* The floating-point format supports more negative exponents
		 * than positive, or perhaps the result is a subnormal number. */
		if (exp > -DBL_MIN_10_EXP)
		{
			/* The result might be a valid subnormal number, but the
			 * exponent underflows.  Tweak fraction so that it is below
			 * 1.0 first, so that if the exponent still underflows after
			 * that, the result is sure to underflow as well. */
			exp -= mant_size;
			dbl_exp = 1.0;
			for (d = powersOf10; mant_size != 0; mant_size >>= 1, d += 1) {
				if (mant_size & 01) {
					dbl_exp *= *d;
				}
			}
			fraction /= dbl_exp;
		}
		d = negPowOf10;
		exp_sign = 0;
	}
	dbl_exp = 1.0;
	for (; exp != 0; exp >>= 1, d += 1) {
		if (exp & 01)
		{
			dbl_exp *= *d;
		}
	}
	if (exp_sign)
	{
		fraction /= dbl_exp;
	}
	else
	{
		fraction *= dbl_exp;
	}

done:
	if (end_ptr != NULL)
	{
		*end_ptr = (char *) p;
	}

	if (sign)
	{
		return -fraction;
	}
	return fraction;
}

void mystrtoi32(char **p, uint32_t *res)
{
	unsigned long temp_res = strtoul(*p, p, 16);
	*res = FFMINMAX(temp_res, 0, 0xFFFFFFFF);
}

uint32_t parse_color_tag(char *str)
{
	uint32_t color = 0;

	while (*str == '&' || *str == 'H' || *str == 'h')
		++str;

	mystrtoi32(&str, &color);
	return color;
}

// Return a boolean value for a string
char parse_bool(char *str)
{
	skip_spaces(&str);
	return !ass_strncasecmp(str, "yes", 3) || strtol(str, NULL, 10) > 0;
}

ass_track_t * ass_alloc_track(request_context_t *request_context)
{
	ass_track_t *track = vod_calloc(request_context->pool, sizeof(ass_track_t));
	if (!track)
		return NULL;

	track->play_res_x = 1920;
	track->play_res_y = 1080;
	track->language = strdup("English (US)");
	track->title = strdup("English (US)");

	return track;
}

int ass_alloc_style(ass_track_t *track)
{
	int sid;

	if (track->n_styles == track->max_styles)
	{
		track->max_styles += ASS_STYLES_ALLOC;
		track->styles =
			(ass_style_t *) realloc(track->styles,
									sizeof(ass_style_t) *
									track->max_styles);
	}

	sid = track->n_styles++;
	vod_memzero(track->styles + sid, sizeof(ass_style_t));
	return sid;
}

int ass_alloc_event(ass_track_t *track)
{
	int eid;

	if (track->n_events == track->max_events)
	{
		track->max_events = track->max_events * 2 + 1;
		track->events =
			(ass_event_t *) realloc(track->events,
									sizeof(ass_event_t) *
									track->max_events);
	}

	eid = track->n_events++;
	vod_memzero(track->events + eid, sizeof(ass_event_t));
	return eid;
}
void ass_free_event(ass_track_t *track, int eid)
{
	ass_event_t *event = track->events + eid;

	free(event->name);
	free(event->effect);
	free(event->text);
}
void ass_free_style(ass_track_t *track, int sid)
{
	ass_style_t *style = track->styles + sid;

	free(style->name);
	free(style->font_name);
}

void ass_free_track(vod_pool_t* pool, ass_track_t *track)
{
	int i;

	free(track->style_format);
	free(track->event_format);
	free(track->language);
	free(track->title);
	if (track->styles)
	{
		for (i = 0; i < track->n_styles; ++i)
			ass_free_style(track, i);
	}
	free(track->styles);
	if (track->events)
	{
		for (i = 0; i < track->n_events; ++i)
			ass_free_event(track, i);
	}
	free(track->events);
	free(track->name);
	vod_free(pool, track);
}



/**
 * \brief Set up default style
 * \param style style to edit to defaults
 * The parameters are mostly taken directly from VSFilter source for
 * best compatibility.
 */
static void set_default_style(ass_style_t *style, bool_t alloc_names)
{
	if (alloc_names)
	{
		style->name				= strdup("Default");
		style->font_name		= strdup("Arial");
	}
	style->font_size			= 18;
	style->primary_colour		= 0x00ffffff;
	style->secondary_colour		= 0x00ffff00;
	style->outline_colour		= 0x00000000;
	style->back_colour			= 0x80000000;
	style->bold					= FALSE;
	style->italic				= FALSE;
	style->underline			= FALSE;
	style->strike_out			= FALSE;
	style->right_to_left_language = FALSE;
	style->output_in_cur_segment  = FALSE;
	style->scale_x				= 100.0;
	style->scale_y				= 100.0;
	style->spacing				= 0.0;
	style->angle				= 0.0;
	style->border_style			= 1;
	style->outline				= 2;
	style->shadow				= 0;
	style->alignment			= 2;
	style->margin_l = style->margin_r = style->margin_v = 20;
	style->encoding				= 0;
}

static long long string2timecode(char *p)
{
	int h, m, s, ms;
	long long tm;
	int res = sscanf(p, "%d:%d:%d.%d", &h, &m, &s, &ms);
	if (res < 4)
	{
		// error msg "Bad timestamp";
		return 0;
	}
	tm = ((h * 60LL + m) * 60 + s) * 1000 + ms * 10LL;
	return tm;
}
/**
 * \brief find style by name
 * \param track track
 * \param name style name
 * \return index in track->styles
 * Returns 0 if no styles found -> expects at least 1 style.
 * Parsing code always adds "Default" style in the beginning.
 */
int lookup_style(ass_track_t *track, char *name)
{
	int i;
	// '*' seem to mean literally nothing;
	// VSFilter removes them as soon as it can
	while (*name == '*')
		++name;
	// VSFilter then normalizes the case of "Default"
	// (only in contexts where this function is called)
	if (!ass_strncasecmp(name, "Default", 7))
		name = "Default";
	for (i = track->n_styles - 1; i >= 0; --i)
	{
		if (strcmp(track->styles[i].name, name) == 0)
			return i;
	}
	i = track->default_style;
	return i;
}

#define NEXT(str,token) \
	token = next_token(&str); \
	if (!token) break;

static char *next_token(char **str)
{
	char *p = *str;
	char *start;
	skip_spaces(&p);
	if (*p == '\0') {
		*str = p;
		return 0;
	}
	start = p;					// start of the token
	for (; (*p != '\0') && (*p != ','); ++p) {
	}
	if (*p == '\0') {
		*str = p;				// eos found, str will point to '\0' at exit
	}
	else
	{
		*p = '\0';
		*str = p + 1;			// ',' found, str will point to the next char (beginning of the next token)
	}
	rskip_spaces(&p, start);	// end of current token: the first space character, or '\0'
	*p = '\0';
	return start;
}

/**
 * \brief Parse the tail of Dialogue line
 * \param track track
 * \param event parsed data goes here
 * \param str string to parse, zero-terminated
*/
static int process_event_tail(ass_track_t *track, ass_event_t *event, char *str)
{
	char *token;
	char *tname;
	char *p = str;
	ass_event_t *target = event;

	char *format = strdup(track->event_format);
	char *q = format;			// format scanning pointer

	if (track->n_styles == 0)
	{
		// add "Default" style to the end
		// will be used if track does not contain a default style (or even does not contain styles at all)
		int sid = ass_alloc_style(track);
		set_default_style(&track->styles[sid], TRUE);
		track->default_style = sid;
	}

	while (1)
	{
		NEXT(q, tname);
		if (!ass_strcasecmp(tname, "Text"))
		{
			char *last;
			event->text = strdup(p);
			if (*event->text != 0) {
				last = event->text + strlen(event->text) - 1;
				if (last >= event->text && *last == '\r')
					*last = 0;
			}
			// need to track the largest end time in all events, since they are not in chronological order
			if (track->max_duration < event->end)
			{
				track->max_duration = event->end;
			}
			free(format);
			return 0;			// "Text" is always the last
		}
		NEXT(p, token);

		if (!ass_strcasecmp(tname, "Start")) {
			target->start = string2timecode(token);
		} else if (!ass_strcasecmp(tname, "End")) {
			target->end = string2timecode(token);
		} else if (!ass_strcasecmp(tname, "MarginL")) {
			target->margin_l	= atoi(token);
		} else if (!ass_strcasecmp(tname, "MarginR")) {
			target->margin_r	= atoi(token);
		} else if (!ass_strcasecmp(tname, "MarginV")) {
			target->margin_v	= atoi(token);
		} else if (!ass_strcasecmp(tname, "Style")) {
			target->style		= lookup_style(track, token);
		}
	}
	free(format);
	return 1;
}

/**
 * \brief converts numpad-style align to align.
 */
int numpad2align(int val)
{
	if (val < -INT_MAX)
		// Pick an alignment somewhat arbitrarily. VSFilter handles
		// INT32_MIN as a mix of 1, 2 and 3, so prefer one of those values.
		val = 2;
	else if (val < 0)
		val = -val;
	int res = ((val - 1) % 3) + 1;  // horizontal alignment
	if (val <= 3)
		res |= VALIGN_SUB;
	else if (val <= 6)
		res |= VALIGN_CENTER;
	else
		res |= VALIGN_TOP;
	return res;
}

/**
 * \brief Parse the style line
 * \param track track
 * \param str string to parse, zero-terminated
 * Allocates a new style struct.
*/
static int process_style(ass_track_t *track, char *str, request_context_t* request_context)
{

	char *token;
	char *tname;
	char *p = str;
	char *format;
	char *q;						// format scanning pointer
	int sid;
	ass_style_t *style;
	ass_style_t *target;

	if (!track->style_format)
	{
		// no style format header
		// probably an ancient script version
		if (track->track_type == TRACK_TYPE_SSA)
			track->style_format =
				strdup
				("Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour, Bold, Italic, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding");
		else
			track->style_format =
				strdup
				("Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding");
	}

	q = format = strdup(track->style_format);

	// Add default style first
	if (track->n_styles == 0)
	{
		// will be used if track does not contain a default style (or even does not contain styles at all)
		int sid = ass_alloc_style(track);
		set_default_style(&track->styles[sid], TRUE);
		track->default_style = sid;
	}

	sid = ass_alloc_style(track);

	style = track->styles + sid;
	target = style;

	set_default_style(target, FALSE);

	while (1)
	{
		NEXT(q, tname);
		NEXT(p, token);

		if (!ass_strcasecmp(tname, "Name")) {
			if (target->name != NULL) free(target->name);
			target->name = strdup(token);
		} else if (!ass_strcasecmp(tname, "Fontname")) {
			if (target->font_name != NULL) free(target->font_name);
			target->font_name = strdup(token);
		} else if (!ass_strcasecmp(tname, "Fontsize")) {
			target->font_size			= atoi(token);
		} else if (!ass_strcasecmp(tname, "PrimaryColour")) {
			target->primary_colour		= parse_color_tag(token);
		} else if (!ass_strcasecmp(tname, "OutlineColour")) {
			target->outline_colour 		= parse_color_tag(token);
		} else if (!ass_strcasecmp(tname, "BackColour")) {
			target->back_colour			= parse_color_tag(token);

		} else if (!ass_strcasecmp(tname, "Bold")) {
			target->bold				= atoi(token);
		} else if (!ass_strcasecmp(tname, "Italic")) {
			target->italic				= atoi(token);
		} else if (!ass_strcasecmp(tname, "Underline")) {
			target->underline			= atoi(token);
		} else if (!ass_strcasecmp(tname, "StrikeOut")) {
			target->strike_out			= atoi(token);
		} else if (!ass_strcasecmp(tname, "BorderStyle")) {
			target->border_style		= atoi(token);
		} else if (!ass_strcasecmp(tname, "Alignment")) {
			target->alignment			= atoi(token);
		} else if (!ass_strcasecmp(tname, "MarginL")) {
			target->margin_l			= atoi(token);
		} else if (!ass_strcasecmp(tname, "MarginR")) {
			target->margin_r			= atoi(token);
		} else if (!ass_strcasecmp(tname, "MarginV")) {
			target->margin_v			= atoi(token);
		} else if (!ass_strcasecmp(tname, "Outline")) {
			target->outline				= atoi(token);

		/*
		} else if (!ass_strcasecmp(tname, "SecondaryColour")) {
			target->secondary_colour	= parse_color_tag(token);
		} else if (!ass_strcasecmp(tname, "Shadow")) {
			target->shadow				= atoi(token);
		} else if (!ass_strcasecmp(tname, "Spacing")) {
				target->spacing			= ass_atof(token);
		} else if (!ass_strcasecmp(tname, "Angle")) {
				target->angle			= ass_atof(token);
		} else if (!ass_strcasecmp(tname, "ScaleX")) {
				target->scale_x			= ass_atof(token);
		} else if (!ass_strcasecmp(tname, "ScaleY")) {
				target->scale_y			= ass_atof(token);
		*/
		}
	}

	if (strcmp(style->name, "Default") == 0)
		track->default_style = sid;
	// SSA uses BackColour for both outline and shadow
	// this will destroy SSA's TertiaryColour, but i'm not going to use it anyway
	if (track->track_type == TRACK_TYPE_SSA)
		style->outline_colour = style->back_colour;
	if (track->track_type == TRACK_TYPE_ASS)
	{
		style->alignment = numpad2align(style->alignment);
	}
	// VSFilter compatibility
	else if (style->alignment == 8)
		style->alignment = 3;
	else if (style->alignment == 4)
		style->alignment = 11;

	style->scale_x		= FFMAX(style->scale_x, 0.) / 100.;
	style->scale_y		= FFMAX(style->scale_y, 0.) / 100.;
	style->spacing		= FFMAX(style->spacing, 0.);
	style->angle		= FFMAX(style->spacing, 0.);
	style->outline		= FFMAX(style->outline, 0);
	style->shadow		= FFMAX(style->shadow,  0);
	style->bold			= !!style->bold;
	style->italic		= !!style->italic;
	style->underline	= !!style->underline;
	style->strike_out	= !!style->strike_out;
	if (!style->name)
		style->name		= strdup("Default");
	if (!style->font_name)
		style->font_name = strdup("Arial");


	// For now, right_to_left_language is TRUE only for Arabic language. In future, it will be enabled for many others.
	if ( !ass_strncasecmp(style->font_name, "Adobe Arabic", 12) )
	{
		style->right_to_left_language = TRUE;
	}
	else
	{
		style->right_to_left_language = track->right_to_left_language;
	}
	free(format);
	return 0;

}

static int process_styles_line(ass_track_t *track, char *str, request_context_t* request_context)
{
	if (!strncmp(str, "Format:", 7))
	{
		char *p = str + 7;
		skip_spaces(&p);
		free(track->style_format);
		track->style_format = strdup(p);
	}
	else if (!strncmp(str, "Style:", 6))
	{
		char *p = str + 6;
		skip_spaces(&p);
		process_style(track, p, request_context);
	}
	return 0;
}

static int process_info_line(ass_track_t *track, char *str, request_context_t* request_context)
{
	if (!strncmp(str, "PlayResX:", 9))
	{
		track->play_res_x = atoi(str + 9);
	}
	else if (!strncmp(str, "PlayResY:", 9))
	{
		track->play_res_y = atoi(str + 9);
	}
	else if (!strncmp(str, "Timer:", 6))
	{
		track->timer = ass_atof(str + 6);
	}
	else if (!strncmp(str, "WrapStyle:", 10))
	{
		track->wrap_style = atoi(str + 10);
	}
	else if (!strncmp(str, "ScaledBorderAndShadow:", 22))
	{
		track->scaled_border_and_shadow = parse_bool(str + 22);
	}
	else if (!strncmp(str, "Kerning:", 8))
	{
		track->kerning = parse_bool(str + 8);
	}
	else if (!strncmp(str, "YCbCr Matrix:", 13))
	{
		// ignore for now
	}
	else if (!strncmp(str, "Language:", 9))
	{
		// This field is not part of the ASS/SSA specs
		char *p = str + 9;
		char *strt, *end;
		while (*p && ass_isspace(*p)) p++;
		strt = p;
		while (*p && !ass_isspace(*p)) p++;
		end = p;
		free(track->language);
		track->language = strndup(strt, FFMAX(TITLE_BYTES_CONSIDERED, (size_t)(end-strt)));
	} else if (!strncmp(str, "Title:", 6))
	{
		char *p = str + 6;
		char *strt, *end;
		while (*p && ass_isspace(*p)) p++;
		strt = p;
		while (*p && !ass_isspace(*p)) p++;
		end = p;
		free(track->title);
		track->title = strndup(strt, FFMAX(TITLE_BYTES_CONSIDERED, (size_t)(end-strt)));
		// Title: ﺎﻠﻋﺮﺒﻳﺓ
		track->right_to_left_language = ((TITLE_BYTES_CONSIDERED == (end-strt)) && (strt[0] == (char)0xD8) && (strt[13] == (char)0xA9));
	}
	return 0;
}

static void event_format_fallback(ass_track_t *track)
{
	track->state = PST_EVENTS;
	if (track->track_type == TRACK_TYPE_SSA)
		track->event_format = strdup("Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text");
	else
		track->event_format = strdup("Layer, Start, End, Style, Actor, MarginL, MarginR, MarginV, Effect, Text");
}

static int process_events_line(ass_track_t *track, char *str, request_context_t* request_context)
{
	if (!strncmp(str, "Format:", 7))
	{
		char *p = str + 7;
		skip_spaces(&p);
		free(track->event_format);
		track->event_format = strdup(p);

	}
	else if (!strncmp(str, "Dialogue:", 9))
	{
		// This should never be reached for embedded subtitles.
		// They have slightly different format and are parsed in ass_process_chunk,
		// called directly from demuxer
		int eid;
		ass_event_t *event;

		str += 9;
		skip_spaces(&str);

		eid = ass_alloc_event(track);
		event = track->events + eid;

		// We can't parse events without event_format
		if (!track->event_format)
			event_format_fallback(track);

		process_event_tail(track, event, str);
	}
	else if (!strncmp(str, "Comment:", 8))
	{
		// Ignore and do nothing, this is just a comment line
	}
	else
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"Event line not understood: %s", str);
	}
	return 0;
}

/**
 * \brief Parse a header line
 * \param track track
 * \param str string to parse, zero-terminated
*/
static int process_line(ass_track_t *track, char *str, request_context_t* request_context)
{
	int retval = 0;
	if (!ass_strncasecmp(str, "[Script Info]", 13))
	{
		track->state = PST_INFO;
	}
	else if (!ass_strncasecmp(str, "[V4 Styles]", 11))
	{
		track->state = PST_STYLES;
		track->track_type = TRACK_TYPE_SSA;
	}
	else if (!ass_strncasecmp(str, "[V4+ Styles]", 12))
	{
		track->state = PST_STYLES;
		track->track_type = TRACK_TYPE_ASS;
	}
	else if (!ass_strncasecmp(str, "[Events]", 8))
	{
		track->state = PST_EVENTS;
	}
	else if (!ass_strncasecmp(str, "[Fonts]", 7))
	{
		track->state = PST_FONTS;
	}
	else
	{
		switch (track->state)
		{
		case PST_INFO:
			retval |= process_info_line(track, str, request_context);
			break;
		case PST_STYLES:
			retval |= process_styles_line(track, str, request_context);
			break;
		case PST_EVENTS:
			retval |= process_events_line(track, str, request_context);
			break;
		case PST_FONTS:
			// ignore for now
			break;
		default:
			break;
		}
	}
	return 0;
}

/**
 * \brief Process all text in an ASS/SSA file
 * \param track output ass_track_t pointer
 * \param str utf-8 string to parse, zero-terminated
*/
static int process_text(ass_track_t *track, char *str, request_context_t* request_context)
{
	int retval = 0;
	char *p = str;

	while (1)
	{
		char *q;
		while (1)
		{
			if ((*p == '\r') || (*p == '\n'))
				++p;
			else if (p[0] == '\xef' && p[1] == '\xbb' && p[2] == '\xbf')
				p += 3;		 // U+FFFE (BOM)
			else
				break;
		}
		for (q = p; ((*q != '\0') && (*q != '\r') && (*q != '\n')); ++q)
		{
		};
		if (q == p)
			break;
		if (*q != '\0')
			*(q++) = '\0';
		retval |= process_line(track, p, request_context);
		if (*q == '\0')
			break;
		p = q;
	}
	return retval;
}

/*
 * \param buf pointer to subtitle text in utf-8
 */
ass_track_t *parse_memory(char *buf, int len, request_context_t* request_context)
{
	ass_track_t *track;
	int bfailed;
	// copy the input buffer, as the parsing is destructive.
	char *pcopy = vod_alloc(request_context->pool, len+1);
	if (pcopy == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"parse_memory(): vod_alloc failed");
		return NULL;
	}
	vod_memcpy(pcopy, buf, len+1);

	track = ass_alloc_track(request_context);
	if (!track)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"parse_memory(): vod_calloc() failed");
		vod_free(request_context->pool, pcopy);
		return NULL;
	}

	// destructive parsing of pcopy
	bfailed = process_text(track, pcopy, request_context);
	if (bfailed == 1)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"parse_memory(): process_text failed, track_type = %d", track->track_type);

	}
	vod_free(request_context->pool, pcopy); // not needed anymore either way

	if (track->track_type == TRACK_TYPE_UNKNOWN) {
		ass_free_track(request_context->pool, track);
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"parse_memory(): track_type unknown");
		return NULL;
	}

	return track;
}

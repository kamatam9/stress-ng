/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-builtin.h"
#include "core-cpu-cache.h"

#define MIN_RADIXSORT_SIZE	(1 * KB)
#define MAX_RADIXSORT_SIZE	(4 * MB)
#define DEFAULT_RADIXSORT_SIZE	(256 * KB)

static const stress_help_t help[] = {
	{ NULL,	"radixsort N",		"start N workers radix sorting random strings" },
	{ NULL,	"radixsort-method M",	"select sort method [ radixsort-libc | radixsort-nonlibc]" },
	{ NULL,	"radixsort-ops N",	"stop after N radixsort bogo operations" },
	{ NULL,	"radixsort-size N",	"number of strings to sort" },
	{ NULL,	NULL,			NULL }
};

typedef int (*radixsort_func_t)(const unsigned char **base, int nmemb, const unsigned char *table, unsigned endbyte);

typedef struct {
	const char *name;
	const radixsort_func_t radixsort_func;
} stress_radixsort_method_t;

#define STR_SIZE	(8)

static volatile bool do_jmp = true;
static sigjmp_buf jmp_env;

#define IDX(base, i, k) 	(1U + base[(i)][(k)])
#define IDX_T(base, i, k)	(1U + table[base[(i)][(k)]])

static inline void ALWAYS_INLINE radix_count_sort(
	const int size,
	const unsigned short int k,
	const unsigned char *base[size],
	const unsigned char *b[size],
	const unsigned short int lengths[size],
	const unsigned char table[256])
{
	register int i;
	unsigned int c[257];

	(void)shim_memset(c, 0, sizeof(c));

	if (table) {
		for (i = 0; i < size; i++)
			c[(k < lengths[i]) ? IDX_T(base, i, k) : 0]++;

		for (i = 1; i < 257; i++)
			c[i] += c[i - 1];

		for (i = size - 1; i >= 0; i--) {
			register const bool lt = k < lengths[i];
			register const int j = IDX_T(base, i, k);
			register const int l = lt ? j : 0;

			c[l]--;
			b[c[l]] = base[i];
		}
	} else {
		for (i = 0; i < size; i++)
			c[(k < lengths[i]) ? IDX(base, i, k) : 0]++;

		for (i = 1; i < 257; i++)
			c[i] += c[i - 1];

		for (i = size - 1; i >= 0; i--) {
			register const bool lt = k < lengths[i];
			register const int j = IDX(base, i, k);
			register const int l = lt ? j : 0;

			c[l]--;
			b[c[l]] = base[i];
		}
	}
	(void)shim_memcpy((void *)base, (void *)b, sizeof(*base) * size);
}

static inline ALWAYS_INLINE int radix_strlen(const unsigned char *str, unsigned char endbyte)
{
	register const unsigned char *ptr = str;

	while (*ptr != endbyte)
		ptr++;

	return ptr - str;
}

static int radixsort_nonlibc(
	const unsigned char **base,
	int nmemb,
	const unsigned char *table,
	unsigned int endbyte)
{
	const unsigned char **b;
	register int digit;
	unsigned short int *lengths, max;
	register int i;
	unsigned char endchar;

	if (nmemb < 2)
		return 0;

	b = malloc(sizeof(*b) * nmemb);
	if (!b) {
		errno = ENOMEM;
		return -1;
	}
	lengths = malloc(sizeof(*lengths) * nmemb);
	if (!lengths) {
		free(b);
		errno = ENOMEM;
		return -1;
	}

	endchar = (unsigned char)endbyte;
	max = radix_strlen(base[0], endchar);
	lengths[0] = max;
	for (i = 1; i < nmemb; i++) {
		const short int len = radix_strlen(base[i], endchar);

		lengths[i] = len;
		if (len > max)
			max = len;
	}

	for (digit = max - 1; digit >= 0; digit--)
		radix_count_sort(nmemb, digit, base, b, lengths, table);

	free(lengths);
	free(b);
	return 0;
}

static const stress_radixsort_method_t stress_radixsort_methods[] = {
#if defined(HAVE_LIB_BSD)
	{ "radixsort-libc",	radixsort },
#endif
	{ "radixsort-nonlibc",	radixsort_nonlibc },
};

static int stress_set_radixsort_method(const char *opt)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stress_radixsort_methods); i++) {
		if (strcmp(opt, stress_radixsort_methods[i].name) == 0) {
			stress_set_setting("radixsort-method", TYPE_ID_SIZE_T, &i);
			return 0;
		}
	}

	(void)fprintf(stderr, "radixsort-method must be one of:");
	for (i = 0; i < SIZEOF_ARRAY(stress_radixsort_methods); i++) {
		(void)fprintf(stderr, " %s", stress_radixsort_methods[i].name);
	}
	(void)fprintf(stderr, "\n");
	return -1;
}

/*
 *  stress_radixsort_handler()
 *	SIGALRM generic handler
 */
static void MLOCKED_TEXT stress_radixsort_handler(int signum)
{
	(void)signum;

	if (do_jmp) {
		do_jmp = false;
		siglongjmp(jmp_env, 1);		/* Ugly, bounce back */
	}
}

/*
 *  stress_set_radixsort_size()
 *	set radixsort size
 */
static int stress_set_radixsort_size(const char *opt)
{
	uint64_t radixsort_size;

	radixsort_size = stress_get_uint64(opt);
	stress_check_range("radixsort-size", radixsort_size,
		MIN_RADIXSORT_SIZE, MAX_RADIXSORT_SIZE);
	return stress_set_setting("radixsort-size", TYPE_ID_UINT64, &radixsort_size);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_radixsort_method,	stress_set_radixsort_method },
	{ OPT_radixsort_size,	stress_set_radixsort_size },
	{ 0,			NULL }
};

/*
 *  stress_radixsort()
 *	stress radixsort
 */
static int stress_radixsort(stress_args_t *args)
{
	uint64_t radixsort_size = DEFAULT_RADIXSORT_SIZE;
	const unsigned char **data;
	unsigned char *text, *ptr;
	int n, i;
	struct sigaction old_action;
	int ret;
	unsigned char revtable[256];
	size_t radixsort_method = 0;

	radixsort_func_t radixsort_func;

	(void)stress_get_setting("radixsort-method", &radixsort_method);

	radixsort_func = stress_radixsort_methods[radixsort_method].radixsort_func;
	if (args->instance == 0)
		pr_inf("%s: using method '%s'\n",
			args->name, stress_radixsort_methods[radixsort_method].name);

	if (!stress_get_setting("radixsort-size", &radixsort_size)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			radixsort_size = MAX_RADIXSORT_SIZE;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			radixsort_size = MIN_RADIXSORT_SIZE;
	}
	n = (int)radixsort_size;

	text = calloc((size_t)n, STR_SIZE);
	if (!text) {
		pr_inf_skip("%s: calloc failed allocating %d strings, "
			"skipping stressor\n", args->name, n);
		return EXIT_NO_RESOURCE;
	}
	data = calloc((size_t)n, sizeof(*data));
	if (!data) {
		pr_inf_skip("%s: calloc failed allocating %d string pointers, "
			"skipping stressor\n", args->name, n);
		free(text);
		return EXIT_NO_RESOURCE;
	}

	ret = sigsetjmp(jmp_env, 1);
	if (ret) {
		/*
		 * We return here if SIGALRM jmp'd back
		 */
		(void)stress_sigrestore(args->name, SIGALRM, &old_action);
		goto tidy;
	}

	if (stress_sighandler(args->name, SIGALRM, stress_radixsort_handler, &old_action) < 0) {
		free(data);
		free(text);
		return EXIT_FAILURE;
	}

	for (i = 0; i < 256; i++)
		revtable[i] = (unsigned char)(255 - i);

	/* This is very expensive, do it once */
	for (ptr = text, i = 0; i < n; i++, ptr += STR_SIZE) {
		data[i] = ptr;
		stress_rndstr((char *)ptr, STR_SIZE);
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		/* Sort "random" data */
		(void)radixsort_func(data, n, NULL, 0);
		if (!stress_continue_flag())
			break;

		if (g_opt_flags & OPT_FLAGS_VERIFY) {
			for (i = 0; i < n - 1; i++) {
				if (strcmp((const char *)data[i], (const char *)data[i + 1]) > 0) {
					pr_fail("%s: sort error "
						"detected, incorrect ordering "
						"found\n", args->name);
					break;
				}
			}
		}

		/* Reverse sort */
		(void)radixsort_func(data, n, revtable, 0);

		if (g_opt_flags & OPT_FLAGS_VERIFY) {
			for (i = 0; i < n - 1; i++) {
				if (strcmp((const char *)data[i], (const char *)data[i + 1]) < 0) {
					pr_fail("%s: sort error "
						"detected, incorrect ordering "
						"found\n", args->name);
					break;
				}
			}
		}

		/* Randomize first char */
		for (ptr = text, i = 0; i < n; i++, ptr += STR_SIZE)
			*ptr = 'a' + stress_mwc8modn(26);

		stress_bogo_inc(args);
	} while (stress_continue(args));

	do_jmp = false;
	(void)stress_sigrestore(args->name, SIGALRM, &old_action);
tidy:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	free(data);
	free(text);

	return EXIT_SUCCESS;
}

stressor_info_t stress_radixsort_info = {
	.stressor = stress_radixsort,
	.class = CLASS_CPU_CACHE | CLASS_CPU | CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help
};

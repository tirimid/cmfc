#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h>
#include <unistd.h>

#define HS_IS_TEXT(hstate) !HS_IS_RAW(hstate)
#define HS_IS_RAW(hstate) ((hstate) & (HS_LINK_REF | HS_FORCE_RAW))

typedef enum node_type
{
	NT_ROOT = 0,
	NT_TITLE,
	NT_PARAGRAPH,
	NT_U_LIST,
	NT_O_LIST,
	NT_LIST_ITEM,
	NT_IMAGE,
} node_type_t;

typedef enum parse_status
{
	PS_OK = 0,
	PS_ERR,
	PS_SKIP,
} parse_status_t;

typedef enum htmlify_state
{
	HS_NONE = 0,
	HS_LINK_REF = 0x1,
	HS_LINK_TEXT = 0x2,
	HS_CODE = 0x4,
	HS_ITALIC = 0x8,
	HS_BOLD = 0x10,
	HS_FORCE_RAW = 0x20,
} htmlify_state_t;

typedef struct conf
{
	// main configuration data.
	FILE *markup_fp;
	char const *markup_file;
	
	FILE *out_fp;
	char const *out_file;
	
	FILE *style_fp;
	char const *style_file;
	
	// configuration flags.
	bool dump_ast;
} conf_t;

typedef struct file_data
{
	char *markup;
	size_t markup_len;
	
	char *style;
	size_t style_len;
} file_data_t;

typedef struct node
{
	char *data;
	struct node *children;
	size_t nchildren;
	int arg; // type-dependent argument.
	unsigned char type;
} node_t;

typedef struct doc_data
{
	char *title;
	char *author;
	char *created, *revised;
} doc_data_t;

static int conf_read(int argc, char const *argv[]);
static void conf_quit(void);
static int doc_data_verify(void);
static int file_data_read(void);
static void gen_html(void);
static void gen_any_html(node_t const *node);
static void gen_image_html(node_t const *node);
static void gen_o_list_html(node_t const *node);
static void gen_paragraph_html(node_t const *node);
static void gen_title_html(node_t const *node);
static void gen_u_list_html(node_t const *node);
static char *htmlified_substr(char const *s, size_t lb, size_t ub, htmlify_state_t hstate);
static void node_add_child(node_t *node, node_t *child);
static void node_print(FILE *fp, node_t const *node, int depth);
static int parse(void);
static parse_status_t parse_any(node_t *out, size_t *i);
static parse_status_t parse_doc(node_t *out, size_t *i);
static parse_status_t parse_image(node_t *out, size_t *i);
static parse_status_t parse_o_list(node_t *out, size_t *i);
static parse_status_t parse_paragraph(node_t *out, size_t *i);
static parse_status_t parse_title(node_t *out, size_t *i);
static parse_status_t parse_u_list(node_t *out, size_t *i);
static void prog_err(size_t start, char const *msg);
static char const *single_line(char const *s, size_t start);
static void str_dyn_append_s(char **str, size_t *len, size_t *cap, char const *s);
static void str_dyn_append_c(char **str, size_t *len, size_t *cap, char c);
static void usage(char const *name);

static conf_t conf;
static doc_data_t doc_data;
static file_data_t file_data;
static node_t root;

int
main(int argc, char const *argv[])
{
	if (conf_read(argc, argv))
		return 1;
	
	if (file_data_read())
		return 1;
	
	if (parse())
		return 1;
	
	if (doc_data_verify())
		return 1;
	
	if (conf.dump_ast)
	{
		node_print(conf.out_fp, &root, 0);
		return 0;
	}
	
	gen_html();
	
	return 0;
}

static int
conf_read(int argc, char const *argv[])
{
	atexit(conf_quit);
	
	// get option arguments.
	int c;
	while ((c = getopt(argc, (char *const *)argv, "Aho:s:")) != -1)
	{
		switch (c)
		{
		case 'A':
			conf.dump_ast = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'o':
			if (conf.out_fp)
			{
				fprintf(stderr, "err: cannot specify multiple output files!\n");
				return 1;
			}
			
			conf.out_file = optarg;
			conf.out_fp = fopen(optarg, "wb");
			if (!conf.out_fp)
			{
				fprintf(stderr, "err: failed to open output file for writing: %s!\n", optarg);
				return 1;
			}
			
			break;
		case 's':
			if (conf.style_fp)
			{
				fprintf(stderr, "err: cannot specify multiple style files!\n");
				return 1;
			}
			
			conf.style_file = optarg;
			conf.style_fp = fopen(optarg, "rb");
			if (!conf.style_fp)
			{
				fprintf(stderr, "err: failed to open style file for reading: %s!\n", optarg);
				return 1;
			}
			
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	
	// get non-option arguments.
	{
		if (optind != argc - 1)
		{
			fprintf(stderr, "err: expected a single non-option argument!\n");
			return 1;
		}
		
		conf.markup_file = argv[argc - 1];
		conf.markup_fp = fopen(argv[argc - 1], "rb");
		if (!conf.markup_fp)
		{
			fprintf(stderr, "err: failed to open markup file for reading: %s!\n", argv[argc - 1]);
			return 1;
		}
	}
	
	// set unset default configuration.
	{
		if (!conf.out_fp)
		{
			conf.out_file = "stdout";
			conf.out_fp = stdout;
		}
	}
	
	return 0;
}

static void
conf_quit(void)
{
	// close opened files.
	{
		if (conf.markup_fp)
			fclose(conf.markup_fp);
		
		if (conf.out_fp)
			fclose(conf.out_fp);
		
		if (conf.style_fp)
			fclose(conf.style_fp);
	}
}

static int
doc_data_verify(void)
{
	// check for presence of all necessary data.
	{
		if (!doc_data.title)
		{
			fprintf(stderr, "err: document missing a title!\n");
			return 1;
		}
		
		if (!doc_data.author)
		{
			fprintf(stderr, "err: document missing an author!\n");
			return 1;
		}
		
		if (!doc_data.created)
		{
			fprintf(stderr, "err: document missing a creation date!\n");
			return 1;
		}
	}
	
	return 0;
}

static int
file_data_read(void)
{
	// read markup file.
	{
		fseek(conf.markup_fp, 0, SEEK_END);
		long len = ftell(conf.markup_fp);
		if (len == -1)
		{
			fprintf(stderr, "err: failed to get size of markup file: %s!\n", conf.markup_file);
			return 1;
		}
		fseek(conf.markup_fp, 0, SEEK_SET);
		
		file_data.markup_len = len;
		file_data.markup = calloc(len + 1, sizeof(char));
		if (fread(file_data.markup, sizeof(char), len, conf.markup_fp) != len)
		{
			fprintf(stderr, "err: failed to read markup file: %s!\n", conf.markup_file);
			return 1;
		}
	}
	
	// read style file.
	if (conf.style_fp)
	{
		fseek(conf.style_fp, 0, SEEK_END);
		long len = ftell(conf.style_fp);
		if (len == -1)
		{
			fprintf(stderr, "err: failed to get size of style file: %s!\n", conf.style_file);
			return 1;
		}
		fseek(conf.style_fp, 0, SEEK_SET);
		
		file_data.style_len = len;
		file_data.style = calloc(len + 1, sizeof(char));
		if (fread(file_data.style, sizeof(char), len, conf.style_fp) != len)
		{
			fprintf(stderr, "err: failed to read style file: %s!\n", conf.style_file);
			return 1;
		}
	}
	
	return 0;
}

static void
gen_html(void)
{
	// write out preamble, head, document data.
	{
		fprintf(conf.out_fp,
		        "<!DOCTYPE html>\n"
		        "<html>\n"
		        "<head>\n"
		        "<title>%s</title>\n"
		        "<style>%s</style>\n"
		        "</head>\n"
		        "<body>\n"
		        "<div class=\"doc-title\">%s</div>\n"
		        "<div class=\"doc-author\">%s</div>\n"
		        "<div class=\"doc-date\">%s",
		        doc_data.title,
		        conf.style_file ? file_data.style : "",
		        doc_data.title,
		        doc_data.author,
		        doc_data.created);
		
		if (doc_data.revised)
			fprintf(conf.out_fp, " (rev. %s)", doc_data.revised);
		
		fprintf(conf.out_fp, "</div>\n");
	}
	
	// write out document contents.
	{
		for (size_t i = 0; i < root.nchildren; ++i)
			gen_any_html(&root.children[i]);
	}
	
	// write out postamble.
	{
		fprintf(conf.out_fp,
		        "</body>\n"
		        "</html>\n");
	}
}

static void
gen_any_html(node_t const *node)
{
	switch (node->type)
	{
	case NT_TITLE:
		gen_title_html(node);
		break;
	case NT_PARAGRAPH:
		gen_paragraph_html(node);
		break;
	case NT_U_LIST:
		gen_u_list_html(node);
		break;
	case NT_O_LIST:
		gen_o_list_html(node);
		break;
	case NT_IMAGE:
		gen_image_html(node);
		break;
	}
}

static void
gen_image_html(node_t const *node)
{
	fprintf(conf.out_fp, "<img src=\"%s\">\n", node->data);
}

static void
gen_o_list_html(node_t const *node)
{
	int cur_depth = 0;
	for (size_t i = 0; i < node->nchildren; ++i)
	{
		int dd = node->children[i].arg - cur_depth;
		while (dd > 0)
		{
			fprintf(conf.out_fp, "<ol>\n");
			--dd;
		}
		while (dd < 0)
		{
			fprintf(conf.out_fp, "</ol>\n");
			++dd;
		}
		
		fprintf(conf.out_fp, "<li>%s</li>\n", node->children[i].data);
		
		cur_depth = node->children[i].arg;
	}
	
	while (cur_depth > 0)
	{
		fprintf(conf.out_fp, "</ol>\n");
		--cur_depth;
	}
}

static void
gen_paragraph_html(node_t const *node)
{
	fprintf(conf.out_fp, "<p>%s</p>\n", node->data);
}

static void
gen_title_html(node_t const *node)
{
	fprintf(conf.out_fp, "<h%d>%s</h%d>\n", node->arg, node->data, node->arg);
}

static void
gen_u_list_html(node_t const *node)
{
	int cur_depth = 0;
	for (size_t i = 0; i < node->nchildren; ++i)
	{
		int dd = node->children[i].arg - cur_depth;
		while (dd > 0)
		{
			fprintf(conf.out_fp, "<ul>\n");
			--dd;
		}
		while (dd < 0)
		{
			fprintf(conf.out_fp, "</ul>\n");
			++dd;
		}
		
		fprintf(conf.out_fp, "<li>%s</li>\n", node->children[i].data);
		
		cur_depth = node->children[i].arg;
	}
	
	while (cur_depth > 0)
	{
		fprintf(conf.out_fp, "</ul>\n");
		--cur_depth;
	}
}

static char *
htmlified_substr(char const *s, size_t lb, size_t ub, htmlify_state_t hstate)
{
	char *sub = calloc(1, sizeof(char));
	size_t slen = 0, scap = 1;
	
	for (size_t i = lb; i < ub; ++i)
	{
		// handle special character sequences.
		if (HS_IS_TEXT(hstate) && s[i] == '<')
		{
			str_dyn_append_s(&sub, &slen, &scap, "&lt;");
			continue;
		}
		else if (HS_IS_TEXT(hstate) && s[i] == '>')
		{
			str_dyn_append_s(&sub, &slen, &scap, "&gt;");
			continue;
		}
		else if (HS_IS_TEXT(hstate) && s[i] == '&')
		{
			str_dyn_append_s(&sub, &slen, &scap, "&amp;");
			continue;
		}
		else if (s[i] == '"')
		{
			if (HS_IS_RAW(hstate))
				str_dyn_append_s(&sub, &slen, &scap, "%22");
			else
				str_dyn_append_s(&sub, &slen, &scap, "&quot;");
			continue;
		}
		else if (i < ub - 1 && s[i] == '\\')
		{
			// yes, this can allow the user to break out of the imposed
			// sanitization measures.
			// no, it is not worth fixing.
			++i;
			str_dyn_append_c(&sub, &slen, &scap, s[i]);
			continue;
		}
		else if (HS_IS_TEXT(hstate)
		         && i < ub - 1
		         && !strncmp(&s[i], "@[", 2))
		{
			++i;
			str_dyn_append_s(&sub, &slen, &scap, "<a href=\"");
			hstate |= HS_LINK_REF;
			continue;
		}
		else if (hstate & HS_LINK_REF && s[i] == '|')
		{
			hstate &= ~HS_LINK_REF;
			hstate |= HS_LINK_TEXT;
			str_dyn_append_s(&sub, &slen, &scap, "\">");
			continue;
		}
		else if (hstate & HS_LINK_TEXT && s[i] == ']')
		{
			hstate &= ~HS_LINK_TEXT;
			str_dyn_append_s(&sub, &slen, &scap, "</a>");
			continue;
		}
		else if (HS_IS_TEXT(hstate) && s[i] == '`')
		{
			if (hstate & HS_CODE)
			{
				hstate &= ~HS_CODE;
				str_dyn_append_s(&sub, &slen, &scap, "</code>");
			}
			else
			{
				hstate |= HS_CODE;
				str_dyn_append_s(&sub, &slen, &scap, "<code>");
			}
			continue;
		}
		else if (HS_IS_TEXT(hstate)
		         && i < ub + 1
		         && !strncmp(&s[i], "**", 2))
		{
			++i;
			if (hstate & HS_BOLD)
			{
				hstate &= ~HS_BOLD;
				str_dyn_append_s(&sub, &slen, &scap, "</b>");
			}
			else
			{
				hstate |= HS_BOLD;
				str_dyn_append_s(&sub, &slen, &scap, "<b>");
			}
			continue;
		}
		else if (HS_IS_TEXT(hstate) && s[i] == '*')
		{
			if (hstate & HS_ITALIC)
			{
				hstate &= ~HS_ITALIC;
				str_dyn_append_s(&sub, &slen, &scap, "</i>");
			}
			else
			{
				hstate |= HS_ITALIC;
				str_dyn_append_s(&sub, &slen, &scap, "<i>");
			}
			continue;
		}
		
		// if not special, just add the character.
		{
			str_dyn_append_c(&sub, &slen, &scap, s[i]);
		}
	}
	
	// terminate any unterminated HTMLify states.
	switch (hstate)
	{
	case HS_NONE:
		break;
	case HS_LINK_REF:
		str_dyn_append_s(&sub, &slen, &scap, "\"></a>");
		break;
	case HS_LINK_TEXT:
		str_dyn_append_s(&sub, &slen, &scap, "</a>");
		break;
	case HS_CODE:
		str_dyn_append_s(&sub, &slen, &scap, "</code>");
		break;
	case HS_ITALIC:
		str_dyn_append_s(&sub, &slen, &scap, "</i>");
		break;
	case HS_BOLD:
		str_dyn_append_s(&sub, &slen, &scap, "</b>");
		break;
	}
	
	return sub;
}

static void
node_add_child(node_t *node, node_t *child)
{
	++node->nchildren;
	node->children = reallocarray(node->children, node->nchildren, sizeof(node_t));
	node->children[node->nchildren - 1] = *child;
}

static void
node_print(FILE *fp, node_t const *node, int depth)
{
	// pad out appropriate depth.
	{
		for (int i = 0; i < depth; ++i)
			fprintf(fp, "  ");
	}
	
	// write out node information.
	{
		static char const *type_lut[] =
		{
			"NT_ROOT",
			"NT_TITLE",
			"NT_PARAGRAPH",
			"NT_U_LIST",
			"NT_O_LIST",
			"NT_LIST_ITEM",
			"NT_IMAGE",
		};
		
		fprintf(fp,
		        "%s: %d - %s\n",
		        type_lut[node->type],
		        node->arg,
		        node->data ? node->data : "-");
	}
	
	// recursively print out children.
	{
		for (size_t i = 0; i < node->nchildren; ++i)
			node_print(fp, &node->children[i], depth + 1);
	}
}

static int
parse(void)
{
	root = (node_t)
	{
		.data = NULL,
		.children = NULL,
		.nchildren = 0,
		.type = NT_ROOT,
	};
	
	for (size_t i = 0; i < file_data.markup_len;)
	{
		node_t child;
		parse_status_t rc = parse_any(&child, &i);
		switch (rc)
		{
		case PS_OK:
			node_add_child(&root, &child);
			break;
		case PS_ERR:
			return 1;
		case PS_SKIP:
			break;
		}
	}
	
	return 0;
}

static parse_status_t
parse_any(node_t *out, size_t *i)
{
	if (!strncmp("DOC", &file_data.markup[*i], 3))
		return parse_doc(out, i);
	else if (file_data.markup[*i] == '=')
		return parse_title(out, i);
	else if (file_data.markup[*i] == '*')
		return parse_u_list(out, i);
	else if (file_data.markup[*i] == '#')
		return parse_o_list(out, i);
	else if (!strncmp("!()", &file_data.markup[*i], 3))
		return parse_image(out, i);
	else if (file_data.markup[*i] != '\n')
		return parse_paragraph(out, i);
	else
	{
		++*i;
		return PS_SKIP;
	}
}

static parse_status_t
parse_doc(node_t *out, size_t *i)
{
	if (!strncmp("DOC-TITLE ", &file_data.markup[*i], 10))
	{
		if (doc_data.title)
		{
			prog_err(*i, "cannot redefine document title!");
			return PS_ERR;
		}
		
		*i += 10;
		size_t begin = *i;
		while (file_data.markup[*i] && file_data.markup[*i] != '\n')
			++*i;
		
		doc_data.title = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	}
	else if (!strncmp("DOC-AUTHOR ", &file_data.markup[*i], 11))
	{
		if (doc_data.author)
		{
			prog_err(*i, "cannot redefine document author!");
			return PS_ERR;
		}
		
		*i += 11;
		size_t begin = *i;
		while (file_data.markup[*i] && file_data.markup[*i] != '\n')
			++*i;
		
		doc_data.author = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	}
	else if (!strncmp("DOC-CREATED ", &file_data.markup[*i], 12))
	{
		if (doc_data.created)
		{
			prog_err(*i, "cannot redefine document creation date!");
			return PS_ERR;
		}
		
		*i += 12;
		size_t begin = *i;
		while (file_data.markup[*i] && file_data.markup[*i] != '\n')
			++*i;
		
		doc_data.created = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	}
	else if (!strncmp("DOC-REVISED ", &file_data.markup[*i], 12))
	{
		if (doc_data.revised)
		{
			prog_err(*i, "cannot redefine document revision date!");
			return PS_ERR;
		}
		
		*i += 12;
		size_t begin = *i;
		while (file_data.markup[*i] && file_data.markup[*i] != '\n')
			++*i;
		
		doc_data.revised = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	}
	else
	{
		prog_err(*i, "unknown DOC directive!");
		return PS_ERR;
	}
	
	return PS_SKIP;
}

static parse_status_t
parse_image(node_t *out, size_t *i)
{
	*i += 3;
	size_t begin = *i;
	while (file_data.markup[*i] && file_data.markup[*i] != '\n')
		++*i;
	
	*out = (node_t)
	{
		.data = htmlified_substr(file_data.markup, begin, *i, HS_FORCE_RAW),
		.children = NULL,
		.nchildren = 0,
		.type = NT_IMAGE,
		.arg = 0,
	};
	
	return PS_OK;
}

static parse_status_t
parse_o_list(node_t *out, size_t *i)
{
	*out = (node_t)
	{
		.data = NULL,
		.children = NULL,
		.nchildren = 0,
		.type = NT_O_LIST,
		.arg = 0,
	};
	
	for (;;)
	{
		int depth = 0;
		while (file_data.markup[*i] == '#')
		{
			++*i;
			++depth;
		}
		
		size_t begin = *i;
		while (file_data.markup[*i]
		       && strncmp("\n\n", &file_data.markup[*i], 2)
		       && strncmp("\n#", &file_data.markup[*i], 2))
		{
			++*i;
		}
		
		node_t item =
		{
			.data = htmlified_substr(file_data.markup, begin, *i, HS_NONE),
			.children = NULL,
			.nchildren = 0,
			.type = NT_LIST_ITEM,
			.arg = depth,
		};
		node_add_child(out, &item);
		
		++*i;
		if (*i >= file_data.markup_len || file_data.markup[*i] == '\n')
			break;
	}
	
	return PS_OK;
}

static parse_status_t
parse_paragraph(node_t *out, size_t *i)
{
	*i += 4 * !strncmp("    ", &file_data.markup[*i], 4);
	size_t begin = *i;
	while (file_data.markup[*i]
	       && strncmp("\n\n", &file_data.markup[*i], 2)
	       && strncmp("\n    ", &file_data.markup[*i], 5))
	{
		++*i;
	}
	
	*out = (node_t)
	{
		.data = htmlified_substr(file_data.markup, begin, *i, HS_NONE),
		.children = NULL,
		.nchildren = 0,
		.type = NT_PARAGRAPH,
		.arg = 0,
	};
	
	return PS_OK;
}

static parse_status_t
parse_title(node_t *out, size_t *i)
{
	int hsize = 0;
	while (file_data.markup[*i] == '=')
	{
		++*i;
		++hsize;
	}
	
	size_t begin = *i;
	while (file_data.markup[*i] && strncmp("\n\n", &file_data.markup[*i], 2))
		++*i;
	
	*out = (node_t)
	{
		.data = htmlified_substr(file_data.markup, begin, *i, HS_NONE),
		.children = NULL,
		.nchildren = 0,
		.type = NT_TITLE,
		.arg = hsize,
	};
	
	return PS_OK;
}

static parse_status_t
parse_u_list(node_t *out, size_t *i)
{
	*out = (node_t)
	{
		.data = NULL,
		.children = NULL,
		.nchildren = 0,
		.type = NT_U_LIST,
		.arg = 0,
	};
	
	for (;;)
	{
		int depth = 0;
		while (file_data.markup[*i] == '*')
		{
			++*i;
			++depth;
		}
		
		size_t begin = *i;
		while (file_data.markup[*i]
		       && strncmp("\n\n", &file_data.markup[*i], 2)
		       && strncmp("\n*", &file_data.markup[*i], 2))
		{
			++*i;
		}
		
		node_t item =
		{
			.data = htmlified_substr(file_data.markup, begin, *i, HS_NONE),
			.children = NULL,
			.nchildren = 0,
			.type = NT_LIST_ITEM,
			.arg = depth,
		};
		node_add_child(out, &item);
		
		++*i;
		if (*i >= file_data.markup_len || file_data.markup[*i] == '\n')
			break;
	}
	
	return PS_OK;
}

static void
prog_err(size_t start, char const *msg)
{
	fprintf(stderr,
	        "%s[%zu] err: %s\n"
	        "%zu...    %s\n",
	        conf.markup_file,
	        start,
	        msg,
	        start,
	        single_line(file_data.markup, start));
}

// temporarily access a single line of a larger string as a null-terminated
// string; only suitable for temporary uses, e.g. error messages.
static char const *
single_line(char const *s, size_t start)
{
	static char buf[1024];
	
	size_t i;
	for (i = 0; i < sizeof(buf) - 1; ++i)
	{
		if (!s[start + i])
			break;
		
		if (s[start + i] == '\n')
			break;
		
		buf[i] = s[start + i];
	}
	buf[i] = 0;
	
	return buf;
}

static void
str_dyn_append_s(char **str, size_t *len, size_t *cap, char const *s)
{
	size_t slen = strlen(s);
	
	// grow dynamic string as necessary.
	{
		while (*len + slen + 1 >= *cap)
		{
			*cap *= 2;
			*str = realloc(*str, *cap);
		}
	}
	
	// write new data.
	{
		strcpy(&(*str)[*len], s);
		*len += slen;
	}
}

static void
str_dyn_append_c(char **str, size_t *len, size_t *cap, char c)
{
	// grow dynamic string as necessary.
	{
		if (*len + 1 >= *cap)
		{
			*cap *= 2;
			*str = realloc(*str, *cap);
		}
	}
	
	// write new data.
	{
		(*str)[*len] = c;
		(*str)[*len + 1] = 0;
		++*len;
	}
}

static void
usage(char const *name)
{
	printf("CMFC - Custom Markup Format Compiler\n"
	       "For more information, consult the manual at the\n"
	       "following link: https://tirimid.net/tirimid/cmfc.html\n"
	       "\n"
	       "usage:\n"
	       "\t%s [options] file\n"
	       "options:\n"
	       "\t-A       dump the AST of the parsed markup\n"
	       "\t-h       display this text\n"
	       "\t-o file  write output to the specified file\n"
	       "\t-s file  use the specified file as a stylesheet\n",
	       name);
}

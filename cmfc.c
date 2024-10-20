#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h>
#include <unistd.h>

// text - used for normal textual website data.
// raw - used for links and URLS.
#define HS_IS_TEXT(hstate) !HS_IS_RAW(hstate)
#define HS_IS_RAW(hstate) ((hstate) & (HS_LINK_REF | HS_FORCE_RAW | HS_FOOTNOTE_REF))

enum node_type
{
	NT_ROOT = 0,
	NT_TITLE,
	NT_PARAGRAPH,
	NT_U_LIST,
	NT_O_LIST,
	NT_LIST_ITEM,
	NT_IMAGE,
	NT_BLOCKQUOTE,
	NT_TABLE,
	NT_TABLE_ROW,
	NT_TABLE_ITEM,
	NT_FOOTNOTE,
	NT_LONG_CODE,
};

enum parse_status
{
	PS_OK = 0,
	PS_ERR,
	PS_SKIP,
};

enum htmlify_state
{
	HS_NONE = 0x0,
	HS_LINK_REF = 0x1,
	HS_LINK_TEXT = 0x2,
	HS_CODE = 0x4,
	HS_ITALIC = 0x8,
	HS_BOLD = 0x10,
	HS_FORCE_RAW = 0x20,
	HS_FOOTNOTE_REF = 0x40,
	HS_FOOTNOTE_TEXT = 0x80,
};

struct conf
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
};

struct file_data
{
	char *markup;
	size_t markup_len;
	
	char *style;
	size_t style_len;
};

struct node
{
	// how many strings of data are stored depends on the node in question.
	// e.g. footnotes have two data strings, while paragraphs have one.
	char *data[2];
	
	struct node *children;
	size_t nchildren;
	int arg; // type-dependent argument.
	unsigned char type;
};

struct doc_data
{
	char *title;
	char *author;
	char *created, *revised;
};

static int conf_read(int argc, char const *argv[]);
static void conf_quit(void);
static int doc_data_verify(void);
static char const *entity_char(char ch);
static int file_data_read(void);
static void gen_html(void);
static void gen_blockquote_html(struct node const *node);
static void gen_footnote_html(struct node const *node);
static void gen_image_html(struct node const *node);
static void gen_long_code_html(struct node const *node);
static void gen_o_list_html(struct node const *node);
static void gen_paragraph_html(struct node const *node);
static void gen_table_html(struct node const *node);
static void gen_title_html(struct node const *node);
static void gen_u_list_html(struct node const *node);
static char *htmlified_substr(char const *s, size_t lb, size_t ub, enum htmlify_state hstate);
static void node_add_child(struct node *node, struct node *child);
static void node_print(FILE *fp, struct node const *node, int depth);
static int parse(void);
static enum parse_status parse_any(struct node *out, size_t *i);
static enum parse_status parse_blockquote(struct node *out, size_t *i);
static enum parse_status parse_doc(struct node *out, size_t *i);
static enum parse_status parse_footnote(struct node *out, size_t *i);
static enum parse_status parse_image(struct node *out, size_t *i);
static enum parse_status parse_long_code(struct node *out, size_t *i);
static enum parse_status parse_o_list(struct node *out, size_t *i);
static enum parse_status parse_paragraph(struct node *out, size_t *i);
static enum parse_status parse_table(struct node *out, size_t *i);
static enum parse_status parse_table_row(struct node *out, size_t *i);
static enum parse_status parse_title(struct node *out, size_t *i);
static enum parse_status parse_u_list(struct node *out, size_t *i);
static void prog_err(size_t start, char const *msg);
static char const *single_line(char const *s, size_t start);
static void str_dyn_append_s(char **str, size_t *len, size_t *cap, char const *s);
static void str_dyn_append_c(char **str, size_t *len, size_t *cap, char c);
static void usage(char const *name);

static struct conf conf;
static struct doc_data doc_data;
static struct node doc_root;
static struct file_data file_data;

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
		node_print(conf.out_fp, &doc_root, 0);
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
		
		if (doc_data.revised && !doc_data.created)
		{
			fprintf(stderr, "err: document missing a creation date, only revision provided!\n");
			return 1;
		}
	}
	
	return 0;
}

static char const *
entity_char(char ch)
{
	switch (ch)
	{
	case '<':
		return "&lt;";
	case '>':
		return "&gt;";
	case '&':
		return "&amp;";
	case '"':
		return "&quot;";
	case '\'':
		return "&apos;";
	
	default:
		return NULL;
	}
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
		        "<div class=\"doc-title\">%s</div>\n",
		        doc_data.title,
		        conf.style_file ? file_data.style : "",
		        doc_data.title);
		
		// write out author.
		if (doc_data.author)
			fprintf(conf.out_fp, "<div class=\"doc-author\">%s</div>\n", doc_data.author);
		
		// write out creation / revision date.
		{
			if (doc_data.created)
				fprintf(conf.out_fp, "<div class=\"doc-date\">%s", doc_data.created);
			if (doc_data.revised)
				fprintf(conf.out_fp, " (rev. %s)", doc_data.revised);
			if (doc_data.created)
				fprintf(conf.out_fp, "</div>\n");
		}
	}
	
	// write out document contents and footnotes.
	{
		for (size_t i = 0; i < doc_root.nchildren; ++i)
		{
			switch (doc_root.children[i].type)
			{
			case NT_TITLE:
				gen_title_html(&doc_root.children[i]);
				break;
			case NT_PARAGRAPH:
				gen_paragraph_html(&doc_root.children[i]);
				break;
			case NT_U_LIST:
				gen_u_list_html(&doc_root.children[i]);
				break;
			case NT_O_LIST:
				gen_o_list_html(&doc_root.children[i]);
				break;
			case NT_IMAGE:
				gen_image_html(&doc_root.children[i]);
				break;
			case NT_BLOCKQUOTE:
				gen_blockquote_html(&doc_root.children[i]);
				break;
			case NT_TABLE:
				gen_table_html(&doc_root.children[i]);
				break;
			case NT_FOOTNOTE:
				gen_footnote_html(&doc_root.children[i]);
				break;
			case NT_LONG_CODE:
				gen_long_code_html(&doc_root.children[i]);
				break;
			}
		}
	}
	
	// write out postamble.
	{
		fprintf(conf.out_fp,
		        "</body>\n"
		        "</html>\n");
	}
}

static void
gen_blockquote_html(struct node const *node)
{
	fprintf(conf.out_fp, "<blockquote>%s</blockquote>\n", node->data[0]);
}

static void
gen_footnote_html(struct node const *node)
{
	fprintf(conf.out_fp, "<div class=\"footnote\" id=\"%s\">%s</div>\n", node->data[0], node->data[1]);
}

static void
gen_image_html(struct node const *node)
{
	fprintf(conf.out_fp, "<img src=\"%s\">\n", node->data[0]);
}

static void
gen_long_code_html(struct node const *node)
{
	fprintf(conf.out_fp, "<div class=\"long-code\">%s</div>\n", node->data[0]);
}

static void
gen_o_list_html(struct node const *node)
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
		
		fprintf(conf.out_fp, "<li>%s</li>\n", node->children[i].data[0]);
		
		cur_depth = node->children[i].arg;
	}
	
	while (cur_depth > 0)
	{
		fprintf(conf.out_fp, "</ol>\n");
		--cur_depth;
	}
}

static void
gen_paragraph_html(struct node const *node)
{
	fprintf(conf.out_fp, "<p>%s</p>\n", node->data[0]);
}

static void
gen_table_html(struct node const *node)
{
	fprintf(conf.out_fp, "<table>\n");
	for (size_t row = 0; row < node->nchildren; ++row)
	{
		fprintf(conf.out_fp, "<tr>\n");
		for (size_t col = 0; col < node->children[row].nchildren; ++col)
		{
			fprintf(conf.out_fp,
			        "<td>%s</td>\n",
			        node->children[row].children[col].data[0]);
		}
		fprintf(conf.out_fp, "</tr>\n");
	}
	fprintf(conf.out_fp, "</table>\n");
}

static void
gen_title_html(struct node const *node)
{
	fprintf(conf.out_fp, "<h%d>%s</h%d>\n", node->arg, node->data[0], node->arg);
}

static void
gen_u_list_html(struct node const *node)
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
		
		fprintf(conf.out_fp, "<li>%s</li>\n", node->children[i].data[0]);
		
		cur_depth = node->children[i].arg;
	}
	
	while (cur_depth > 0)
	{
		fprintf(conf.out_fp, "</ul>\n");
		--cur_depth;
	}
}

static char *
htmlified_substr(char const *s, size_t lb, size_t ub, enum htmlify_state hstate)
{
	char *sub = calloc(1, sizeof(char));
	size_t slen = 0, scap = 1;
	
	for (size_t i = lb; i < ub; ++i)
	{
		// handle special character sequences.
		if (i + 1 < ub && s[i] == '\\')
		{
			++i;
			
			if (HS_IS_TEXT(hstate) && entity_char(s[i]))
				str_dyn_append_s(&sub, &slen, &scap, entity_char(s[i]));
			else if (HS_IS_RAW(hstate) && s[i] == '"')
				str_dyn_append_s(&sub, &slen, &scap, "%22");
			else
				str_dyn_append_c(&sub, &slen, &scap, s[i]);
			
			continue;
		}
		else if (HS_IS_TEXT(hstate)
		         && i + 1 < ub
		         && !strncmp(&s[i], "@[", 2))
		{
			++i;
			str_dyn_append_s(&sub, &slen, &scap, "<a href=\"");
			hstate |= HS_LINK_REF;
			continue;
		}
		else if (HS_IS_TEXT(hstate)
		         && i + 1 < ub
		         && !strncmp(&s[i], "[^", 2))
		{
			++i;
			str_dyn_append_s(&sub, &slen, &scap, "<sup><a href=\"#");
			hstate |= HS_FOOTNOTE_REF;
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
		else if (hstate & HS_FOOTNOTE_REF && s[i] == '|')
		{
			hstate &= ~HS_FOOTNOTE_REF;
			hstate |= HS_FOOTNOTE_TEXT;
			str_dyn_append_s(&sub, &slen, &scap, "\">[");
			continue;
		}
		else if (hstate & HS_FOOTNOTE_TEXT && s[i] == ']')
		{
			hstate &= ~HS_FOOTNOTE_TEXT;
			str_dyn_append_s(&sub, &slen, &scap, "]</a></sup>");
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
		         && i + 1 < ub
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
		else if (HS_IS_TEXT(hstate) && entity_char(s[i]))
		{
			str_dyn_append_s(&sub, &slen, &scap, entity_char(s[i]));
			continue;
		}
		else if (HS_IS_RAW(hstate) && s[i] == '"')
		{
			str_dyn_append_s(&sub, &slen, &scap, "%22");
			continue;
		}
		
		// if not special, just add the character.
		{
			str_dyn_append_c(&sub, &slen, &scap, s[i]);
		}
	}
	
	// terminate any unterminated HTMLify states.
	{
		if (hstate & HS_LINK_REF)
			str_dyn_append_s(&sub, &slen, &scap, "\"></a>");
		else if (hstate & HS_LINK_TEXT)
			str_dyn_append_s(&sub, &slen, &scap, "</a>");
		
		if (hstate & HS_FOOTNOTE_REF)
			str_dyn_append_s(&sub, &slen, &scap, "\">[]</a></sup>");
		else if (hstate & HS_FOOTNOTE_TEXT)
			str_dyn_append_s(&sub, &slen, &scap, "]</a></sup>");
		
		if (hstate & HS_CODE)
			str_dyn_append_s(&sub, &slen, &scap, "</code>");
		if (hstate & HS_ITALIC)
			str_dyn_append_s(&sub, &slen, &scap, "</i>");
		if (hstate & HS_BOLD)
			str_dyn_append_s(&sub, &slen, &scap, "</b>");
	}
	
	return sub;
}

static void
node_add_child(struct node *node, struct node *child)
{
	++node->nchildren;
	node->children = reallocarray(node->children, node->nchildren, sizeof(struct node));
	node->children[node->nchildren - 1] = *child;
}

static void
node_print(FILE *fp, struct node const *node, int depth)
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
			"NT_BLOCKQUOTE",
			"NT_TABLE",
			"NT_TABLE_ROW",
			"NT_TABLE_ITEM",
			"NT_FOOTNOTE",
			"NT_LONG_CODE",
		};
		
		fprintf(fp, "%s: %d", type_lut[node->type], node->arg);
		for (size_t i = 0; i < sizeof(node->data) / sizeof(char *); ++i)
		{
			if (node->data[i])
				fprintf(fp, " %s", node->data[i]);
		}
		fprintf(fp, "\n");
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
	doc_root = (struct node)
	{
		.type = NT_ROOT,
	};
	
	for (size_t i = 0; i < file_data.markup_len;)
	{
		struct node child;
		enum parse_status rc = parse_any(&child, &i);
		switch (rc)
		{
		case PS_OK:
			node_add_child(&doc_root, &child);
			break;
		case PS_ERR:
			return 1;
		case PS_SKIP:
			break;
		}
	}
	
	return 0;
}

static enum parse_status
parse_any(struct node *out, size_t *i)
{
	if (!strncmp("DOC", &file_data.markup[*i], 3))
		return parse_doc(out, i);
	else if (file_data.markup[*i] == '=')
		return parse_title(out, i);
	else if (file_data.markup[*i] == '*')
		return parse_u_list(out, i);
	else if (file_data.markup[*i] == '#')
		return parse_o_list(out, i);
	else if (!strncmp("      ", &file_data.markup[*i], 6))
		return parse_blockquote(out, i);
	else if (!strncmp("```\n", &file_data.markup[*i], 4))
		return parse_long_code(out, i);
	else if (!strncmp("---", &file_data.markup[*i], 3))
		return parse_table(out, i);
	else if (!strncmp("!()", &file_data.markup[*i], 3))
		return parse_image(out, i);
	else if (!strncmp("[^", &file_data.markup[*i], 2))
		return parse_footnote(out, i);
	else if (file_data.markup[*i] != '\n')
		return parse_paragraph(out, i);
	else
	{
		++*i;
		return PS_SKIP;
	}
}

static enum parse_status
parse_blockquote(struct node *out, size_t *i)
{
	*i += 6;
	size_t begin = *i;
	while (file_data.markup[*i] && strncmp("\n\n", &file_data.markup[*i], 2))
		++*i;
	
	*out = (struct node)
	{
		.type = NT_BLOCKQUOTE,
	};
	out->data[0] = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	
	return PS_OK;
}

static enum parse_status
parse_doc(struct node *out, size_t *i)
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

static enum parse_status
parse_footnote(struct node *out, size_t *i)
{
	char *name;
	{
		*i += 2;
		size_t begin = *i;
		while (file_data.markup[*i] && file_data.markup[*i] != ']')
		{
			if (*i + 1 < file_data.markup_len
			    && file_data.markup[*i] == '\\')
			{
				++*i;
			}
			++*i;
		}
		
		name = htmlified_substr(file_data.markup, begin, *i, HS_FORCE_RAW);
	}
	
	char *text;
	{
		++*i;
		size_t begin = *i;
		while (file_data.markup[*i]
		       && strncmp("\n\n", &file_data.markup[*i], 2))
		{
			++*i;
		}
		
		text = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	}
	
	*out = (struct node)
	{
		.type = NT_FOOTNOTE,
	};
	out->data[0] = name;
	out->data[1] = text;
	
	return PS_OK;
}

static enum parse_status
parse_image(struct node *out, size_t *i)
{
	*i += 3;
	size_t begin = *i;
	while (file_data.markup[*i] && file_data.markup[*i] != '\n')
		++*i;
	
	*out = (struct node)
	{
		.type = NT_IMAGE,
	};
	out->data[0] = htmlified_substr(file_data.markup, begin, *i, HS_FORCE_RAW);
	
	return PS_OK;
}

static enum parse_status
parse_long_code(struct node *out, size_t *i)
{
	*i += 4;
	size_t begin = *i;
	while (file_data.markup[*i]
	       && strncmp(&file_data.markup[*i], "\n```\n", 4))
	{
		++*i;
	}
	
	*out = (struct node)
	{
		.type = NT_LONG_CODE,
	};
	out->data[0] = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	
	if (file_data.markup[*i])
		*i += 4;
	
	return PS_OK;
}

static enum parse_status
parse_o_list(struct node *out, size_t *i)
{
	*out = (struct node)
	{
		.type = NT_O_LIST,
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
		
		struct node item =
		{
			.type = NT_LIST_ITEM,
			.arg = depth,
		};
		item.data[0] = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
		
		node_add_child(out, &item);
		
		++*i;
		if (*i >= file_data.markup_len || file_data.markup[*i] == '\n')
			break;
	}
	
	return PS_OK;
}

static enum parse_status
parse_paragraph(struct node *out, size_t *i)
{
	*i += 4 * !strncmp("    ", &file_data.markup[*i], 4);
	size_t begin = *i;
	while (file_data.markup[*i]
	       && strncmp("\n\n", &file_data.markup[*i], 2)
	       && strncmp("\n    ", &file_data.markup[*i], 5))
	{
		++*i;
	}
	
	*out = (struct node)
	{
		.type = NT_PARAGRAPH,
	};
	out->data[0] = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	
	return PS_OK;
}

static enum parse_status
parse_table(struct node *out, size_t *i)
{
	// validate beginning of table.
	{
		while (file_data.markup[*i] && file_data.markup[*i] == '-')
			++*i;
		if (file_data.markup[*i] != '\n')
		{
			prog_err(*i, "expected valid table after ---!");
			return PS_ERR;
		}
	}
	
	*out = (struct node)
	{
		.type = NT_TABLE,
	};
	
	for (++*i; file_data.markup[*i] && file_data.markup[*i] != '\n';)
	{
		if (file_data.markup[*i] == '|')
		{
			struct node row;
			if (parse_table_row(&row, i))
				return PS_ERR;
			node_add_child(out, &row);
		}
		else
		{
			prog_err(*i, "expected either | or table end!");
			return PS_ERR;
		}
	}

	return PS_OK;
}

static enum parse_status
parse_table_row(struct node *out, size_t *i)
{
	*out = (struct node)
	{
		.type = NT_TABLE_ROW,
	};
	
	++*i;
	size_t col = 0;
	for (;;)
	{
		size_t begin = *i;
		while (file_data.markup[*i] && file_data.markup[*i] != '|')
		{
			if (*i + 1 < file_data.markup_len
			    && file_data.markup[*i] == '\\')
			{
				++*i;
			}
			++*i;
		}
		if (!file_data.markup[*i])
		{
			prog_err(*i, "incomplete table row data!");
			return PS_ERR;
		}
		
		char *sub = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
		if (col >= out->nchildren)
		{
			struct node item =
			{
				.type = NT_TABLE_ITEM,
			};
			item.data[0] = sub;
			
			node_add_child(out, &item);
		}
		else
		{
			size_t slen = strlen(out->children[col].data[0]);
			size_t scap = slen + 1;
			str_dyn_append_c(&out->children[col].data[0], &slen, &scap, ' ');
			str_dyn_append_s(&out->children[col].data[0], &slen, &scap, sub);
			free(sub);
		}
		
		++*i;
		if (file_data.markup[*i] == '\n')
		{
			++*i;
			col = 0;
			if (!file_data.markup[*i])
			{
				prog_err(*i, "unterminated table row!");
				return PS_ERR;
			}
			else if (file_data.markup[*i] == '-')
			{
				while (file_data.markup[*i]
				       && file_data.markup[*i] == '-')
				{
					++*i;
				}
				
				if (file_data.markup[*i]
				    && file_data.markup[*i] != '\n')
				{
					prog_err(*i, "table row improperly terminated!");
					return PS_ERR;
				}
				
				++*i;
				break;
			}
			else if (file_data.markup[*i] == '|')
				++*i;
			else
			{
				prog_err(*i, "expected row to either terminate or continue!");
				return PS_ERR;
			}
		}
		else
			++col;
	}
	
	return PS_OK;
}

static enum parse_status
parse_title(struct node *out, size_t *i)
{
	// get and validate header size.
	int hsize = 0;
	{
		size_t title_begin = *i;
		while (file_data.markup[*i] == '=')
		{
			++*i;
			++hsize;
		}
		
		if (hsize > 6)
		{
			prog_err(title_begin, "minimum title size is 6!");
			return PS_ERR;
		}
	}
	
	size_t begin = *i;
	while (file_data.markup[*i] && strncmp("\n\n", &file_data.markup[*i], 2))
		++*i;
	
	*out = (struct node)
	{
		.type = NT_TITLE,
		.arg = hsize,
	};
	out->data[0] = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
	
	return PS_OK;
}

static enum parse_status
parse_u_list(struct node *out, size_t *i)
{
	*out = (struct node)
	{
		.type = NT_U_LIST,
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
		
		struct node item =
		{
			.type = NT_LIST_ITEM,
			.arg = depth,
		};
		item.data[0] = htmlified_substr(file_data.markup, begin, *i, HS_NONE);
		
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

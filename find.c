#include <yed/plugin.h>
#include <yed/syntax.h>
#include <regex.h>

static yed_plugin *Self;

void find_handle_exec_error (const int status, const char *pat)
{
    switch (status) {
        case REG_ESPACE:
            yed_cerr("Out of memory!!!");
            break;
        case REG_NOMATCH:
            yed_cprint("Pattern not found: %s", pat);
            break;
    }
}

void find_handle_comp_error (const int status)
{
    switch (status) {
        case REG_BADBR:
            yed_cerr("Invalid curly bracket or brace usage!");
            break;
        case REG_BADPAT:
            yed_cerr("Syntax error in pattern!");
            break;
        case REG_BADRPT:
            yed_cerr("Repititon character, e.g. `?' or `*', appeared in bad position!");
            break;
        case REG_ECOLLATE:
            yed_cerr("Invalid collation!");
            break;
        case REG_ECTYPE:
            yed_cerr("Invalid class character name!");
            break;
        case REG_EESCAPE:
            yed_cerr("Invalid escape sequence!");
            break;
        case REG_ESUBREG:
            yed_cerr("Invalid number in the `\\digit' construct!");
            break;
        case REG_EBRACK:
            yed_cerr("Unbalanced square brackets!");
            break;
        case REG_EPAREN:
            yed_cerr("Unbalanced parentheses!");
            break;
        case REG_EBRACE:
            yed_cerr("Unbalanced curly bracket or brace!");
            break;
        case REG_ERANGE:
            yed_cerr("Endpoint of range expression invalid!");
            break;
        case REG_ESPACE:
            yed_cerr("Out of memory!!!");
            break;
    }
}

char* find_line_to_char_buffer() {
    if (!ys->active_frame || !ys->active_frame->buffer)
        return NULL;
    yed_frame *f = ys->active_frame;
    return yed_get_line_text(f->buffer, f->cursor_line);
}

void find_highlight_match (char *line, regmatch_t match) {
    static yed_syntax syn;
    line[match.rm_eo] = '\0';
    yed_cprint("Found match `%s'", line + match.rm_so);
    yed_syntax_start(&syn);
        yed_syntax_kwd(&syn, line + match.rm_so);
    yed_syntax_end(&syn);
}

void find_in_buffer(int n_args, char **args) {
    /* 
     * No flags for right now. Flags include using extended regex, ignoring
     * case, etc. There are also flags for compilation and execution.
     */
    static const int flags = 0;
    /* 
     * Right now we only have one matching buffer. For matching subexpressions
     * we would have to pass more, but I'm not sure how to get the number of
     * matches an expression matched.
     */
    static const size_t nmatches = 1;

    regmatch_t match[nmatches];
    regex_t    regex;
    int        status;
    char      *linebuff;

    if (n_args < 1) {
        yed_cerr("expected regular expression but got nothing");
        return;
    }

    status = regcomp(&regex, args[0], flags);
    if (status != 0) {
        find_handle_comp_error(status);
        return;
    }

    linebuff = find_line_to_char_buffer();
    if (!linebuff) {
        yed_cerr("Could not create search buffer for line!");
        return;
    }

    status = regexec(&regex, linebuff, nmatches, match, flags);
    if (status != 0) {
        find_handle_exec_error(status, args[0]);
        goto cleanup;
    }

    for (int i = 0; i < nmatches; i++)
        find_highlight_match(linebuff, match[i]);

cleanup:
    free(linebuff);
}

int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();
    Self = self;

    yed_plugin_set_command(Self, "find", find_in_buffer);

    return 0;
}

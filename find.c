#include <yed/plugin.h>
#include <regex.h>

static yed_plugin *Self;

void find_handle_exec_error (const int status)
{
    switch (status) {
        case REG_ESPACE:
            yed_cerr("Out of memory!!!");
            break;
        case REG_NOMATCH:
            yed_cprint("No matches found.");
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

void find_in_buffer(int n_args, char **args) {
    /* 
     * No flags for right now. Flags include using extended regex, ignoring
     * case, etc. There are also flags for compilation and execution.
     */
    static const int flags = 0;
    /* 
     * The first match is always the string itself. The second and on matches
     * are from the regex. Right now we're doing 2, not sure how this would
     * affect global searches, i.e. 's/foo/bar/g'
     */
    static const size_t nmatches = 2;

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
        find_handle_exec_error(status);
        goto cleanup;
    }

    /*
     * TODO: Finds matches but doesn't print correctly. Matches are just
     * offsets, but I'm doing something wrong.
     */
    linebuff[match[1].rm_eo + 1] = '\0';
    yed_cprint("Found match `%s'", linebuff + match[1].rm_so);

cleanup:
    free(linebuff);
}

int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();
    Self = self;

    yed_plugin_set_command(Self, "find", find_in_buffer);

    return 0;
}

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

typedef struct Match {
    regmatch_t match;
    int line;
} Match;
static Match match;

void find_highlight_handler(yed_event *event) {
    yed_frame *frame;

    if (!event->frame)
        return;
    frame = event->frame;

    if (frame != ys->active_frame || !frame->buffer)
        return;

    /*
     * Get the global matches. If there's a match (line > 0) and the event
     * row matches that line, highlight the match in the current frame.
     */
}

void find_set_matches (char *line, int nmatches, regmatch_t *matches)
{
    /* Set the global matches */
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

    find_set_matches(linebuff, nmatches, match);

cleanup:
    free(linebuff);
}

int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();
    Self = self;

    yed_event_handler h;
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_highlight_handler;
    yed_add_event_handler(h);

    yed_plugin_set_command(Self, "find", find_in_buffer);

    return 0;
}

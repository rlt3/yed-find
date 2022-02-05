#include <yed/plugin.h>
#include <yed/syntax.h>
#include <regex.h>

#define FIND_DEFAULT_NUM_MATCHES 16

static yed_plugin *Self;

typedef struct Match {
    regmatch_t off;
    int line;
} Match;

static array_t _matches;

void find_highlight_matches_handler(yed_event *event) {
    yed_attrs *attr, search, search_cursor, *set;
    yed_frame *frame;

    if (!event->frame)
        return;
    frame = event->frame;
    if (frame != ys->active_frame || !frame->buffer)
        return;

    /* get the current styles for the search and search cursor */
    search        = yed_active_style_get_search();
    search_cursor = yed_active_style_get_search_cursor();

    /*
     * TODO: Inefficient. On each line render, goes through each match in the
     * array to find any matches on this row's line.
     */
    Match *m;
    array_traverse(_matches, m) {
        if (m->line != event->row)
            continue;
        for (unsigned col = m->off.rm_so; col < m->off.rm_eo; col++) {
            /* if cursor is within the match, use its style */
            set = (event->row == frame->cursor_line && col == frame->cursor_col - 1)
                        ? &search_cursor
                        : &search;
            /* set the search styles or just raw highlight if not styling */
            attr = array_item(event->line_attrs, col);
            if (ys->active_style) {
                yed_combine_attrs(attr, set);
            } else {
                attr->flags ^= ATTR_INVERSE;
            }
        }
    }
}

void find_push_match (int row, int nmatches, regmatch_t *matches) {
    /*
     * Right now we're just using the first item in the array regardless of
     * length. Once we configure subexpressions (for replacing, later on), we
     * will revist this.
     */
    Match m;
    m.off = matches[0];
    m.line = row;
    array_grow_if_needed(_matches);
    array_push(_matches, m);
}

void find_handle_comp_error (const int status) {
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
    char      *pattern;
    yed_frame *frame;
    int        status;
    int        row;
    char      *line;

    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    if (n_args < 1) {
        yed_cerr("expected regular expression but got nothing");
        return;
    }
    pattern = args[0];

    /* Always clear out any matches on a new find */
    array_clear(_matches);

    status = regcomp(&regex, pattern, flags);
    if (status != 0) {
        find_handle_comp_error(status);
        return;
    }

    /*
     * TODO: Search within each frame.
     */
    row = 1;
    while (1) {
        line = yed_get_line_text(frame->buffer, row);
        if (!line)
            break;

        status = regexec(&regex, line, nmatches, match, flags);
        if (status == 0)
            find_push_match(row, nmatches, match);

        free(line);
        row++;
    }

    if (array_len(_matches) == 0)
        yed_cprint("Pattern not found: %s", pattern);
}

void find_unload (yed_plugin *self) {
    array_free(_matches);
    /* TODO: unload highlight handler */
}

void find_cursor_next_match(int n_args, char **args) {
    if (n_args > 0) {
        yed_cerr("Expected zero arguments.");
        return;
    }
}

void find_cursor_prev_match(int n_args, char **args) {
    if (n_args > 0) {
        yed_cerr("Expected zero arguments.");
        return;
    }
}

int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();
    Self = self;

    yed_plugin_set_unload_fn(Self, find_unload);
    _matches = array_make_with_cap(Match, FIND_DEFAULT_NUM_MATCHES);

    yed_event_handler h;
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_highlight_matches_handler;
    yed_add_event_handler(h);

    /*
     * TODO: Event handler for activating frame or loading frame, or otherwise
     * changing the frame. Matches need to be highlighted in other frames for
     * *any* search.
     */

    yed_plugin_set_command(Self, "find", find_in_buffer);
    yed_plugin_set_command(Self, "find-cursor-next-match", find_cursor_next_match);
    yed_plugin_set_command(Self, "find-cursor-prev-match", find_cursor_prev_match);

    return 0;
}

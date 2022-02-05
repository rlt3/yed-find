#include <yed/plugin.h>
#include <yed/syntax.h>
#include <regex.h>

#define FIND_DEFAULT_NUM_MATCHES 16
#define FIND_DEFAULT_PATTERN_LEN 16

static yed_plugin *Self;

typedef struct MatchFrame {
    /* which frame this holds information for */
    yed_frame *frame;
    /* the matches pertaining to this frame */
    array_t matches;
    /*
     * TODO: next/prev indexes
     */
} MatchFrame;

typedef struct Match {
    /* the line within the frame that this is a match for */
    int line;
    /* offsets in the line where the match starts and ends */
    size_t start;
    size_t end;
} Match;

static array_t _matches;
static array_t _pattern;
static regex_t _regex;
static unsigned _num_matching;

/*
 * Convenience functions for always having the pattern be null-terminated
 */
static inline void find_pattern_terminate () {
    /* end-of-string, helps when passing values into the macro-only array library */
    static char EOS = '\0';
    array_push(_pattern, EOS);
}

static inline void find_pattern_clear () {
    array_clear(_pattern);
    find_pattern_terminate();
}

static inline void find_pattern_replace (char *patt) {
    array_clear(_pattern);
    for (int i = 0; patt[i] != '\0'; i++)
        array_push(_pattern, patt[i]);
    find_pattern_terminate();
}

static inline void find_bad_pattern () {
    yed_cprint("Pattern not found: %s", array_data(_pattern));
}

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
        for (unsigned col = m->start; col < m->end; col++) {
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

int find_push_match (yed_frame *frame,
                     int row,
                     int offset,
                     int nmatches,
                     regmatch_t *matches)
{
    /*
     * Right now we're just using the first item in the array regardless of
     * length. Once we configure subexpressions (for replacing, later on), we
     * will revist this.
     */
    Match m;
    m.line = row;
    m.start = matches[0].rm_so + offset;
    m.end = matches[0].rm_eo + offset;
    array_grow_if_needed(_matches);
    array_push(_matches, m);
    /* returns an offset to where the line should be searched from next */
    return matches[0].rm_eo + 1;
}

void find_handle_comp_error (const int status) {
    switch (status) {
        case REG_BADBR:
            yed_cerr("[FIND] Invalid curly bracket or brace usage!");
            break;
        case REG_BADPAT:
            yed_cerr("[FIND] Syntax error in pattern!");
            break;
        case REG_BADRPT:
            yed_cerr("[FIND] Repititon character, e.g. `?' or `*', appeared in bad position!");
            break;
        case REG_ECOLLATE:
            yed_cerr("[FIND] Invalid collation!");
            break;
        case REG_ECTYPE:
            yed_cerr("[FIND] Invalid class character name!");
            break;
        case REG_EESCAPE:
            yed_cerr("[FIND] Invalid escape sequence!");
            break;
        case REG_ESUBREG:
            yed_cerr("[FIND] Invalid number in the `\\digit' construct!");
            break;
        case REG_EBRACK:
            yed_cerr("[FIND] Unbalanced square brackets!");
            break;
        case REG_EPAREN:
            yed_cerr("[FIND] Unbalanced parentheses!");
            break;
        case REG_EBRACE:
            yed_cerr("[FIND] Unbalanced curly bracket or brace!");
            break;
        case REG_ERANGE:
            yed_cerr("[FIND] Endpoint of range expression invalid!");
            break;
        case REG_ESPACE:
            yed_cerr("[FIND] Out of memory!!!");
            break;
    }
}

int find_regex_compile() {
    /* TODO: compilation flags */
    static const int flags = 0;
    return regcomp(&_regex, array_data(_pattern), flags);
}

int find_search_in_buffer(yed_frame *frame) {
    /* TODO: execute flags */
    static const int flags = 0;
    /* 
     * Right now we only have one matching buffer. For matching subexpressions
     * we would have to pass more, but I'm not sure how to get the number of
     * matches an expression matched.
     */
    static const size_t nmatches = 1;

    regmatch_t match[nmatches];
    int        status;
    int        row;
    char      *line;
    int        len;
    int        offset;

    /* always clear out any matches on a new search */
    array_clear(_matches);

    /* search within each line of the buffer */
    row = 1;
    while (1) {
        line = yed_get_line_text(frame->buffer, row);
        if (!line)
            break;

        /* find every match within each line */
        len = strlen(line);
        offset = 0;
        while (offset < len) {
            status = regexec(&_regex, line + offset, nmatches, match, flags);
            if (status != 0)
                break;
            offset += find_push_match(frame, row, offset, nmatches, match);
        }

        free(line);
        row++;
    }

    return array_len(_matches);
}

void find_search_interactive_start() {
    find_pattern_clear();

    ys->interactive_command = "find";
    /* TODO: Make the cmd_prompt a yed var for this plugin */
    ys->cmd_prompt = "/";

    ys->search_save_row = ys->active_frame->cursor_line;
    ys->search_save_col = ys->active_frame->cursor_col;

    yed_clear_cmd_buff();
    yed_cmd_line_readline_reset(ys->search_readline, &ys->search_hist);

    ys->current_search = array_data(_pattern);
}

void find_search_interactive_build_pattern(char key) {
    yed_cmd_line_readline_take_key(ys->search_readline, key);
    array_zero_term(ys->cmd_buff);
    ys->current_search = array_data(ys->cmd_buff);
    /* assuming that cmd_buff is null terminated */
    find_pattern_replace(array_data(ys->cmd_buff));
}

void find_search_interactive_cancel() {
    /* handle canceling a search part-way through */
    ys->interactive_command = NULL;
    ys->current_search      = NULL;
    yed_clear_cmd_buff();
    find_pattern_clear();
    _num_matching = 0;
}

void find_search_interactive_finish() {
    /* handle finalizing a search */
    ys->interactive_command = NULL;
    ys->current_search = NULL;
    yed_clear_cmd_buff();
}

void find_regex_search(int n_args, char **args) {
    regex_t    regex;
    yed_frame *frame;
    int        key;
    int        status;

    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    if (!ys->interactive_command) {
        if (n_args == 0) {
            /* YEXE("find") enters interactive mode */
            find_search_interactive_start();
            return;
        }
        /* if a pattern is given immediately, use that */
        find_pattern_replace(args[0]);
    } else {
        /* on interactive mode, build regex incrementally */
        sscanf(args[0], "%d", &key);
        switch (key) {
            case ESC:
            case CTRL_C:
                find_search_interactive_cancel();
                break;

            case ENTER:
                find_search_interactive_finish();
                break;

            default:
                find_search_interactive_build_pattern(key);
                break;
        }
    }

    status = find_regex_compile();
    if (status != 0) {
        if (!ys->interactive_command)
            find_handle_comp_error(status);
        return;
    }

    /*
     * TODO: travel to first instance of match in frame. if
     * canceling or pattern doesn't match anymore, go back to the
     * original searching place.
     */
    _num_matching = find_search_in_buffer(frame);
    if (_num_matching == 0 && !ys->interactive_command)
        find_bad_pattern();
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

void find_unload (yed_plugin *self) {
    array_free(_matches);
    array_free(_pattern);
    /* TODO: unload highlight handler */
}


int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();
    Self = self;

    yed_plugin_set_unload_fn(Self, find_unload);
    _matches = array_make_with_cap(Match, FIND_DEFAULT_NUM_MATCHES);
    _pattern = array_make_with_cap(char,  FIND_DEFAULT_PATTERN_LEN);

    yed_event_handler h;
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_highlight_matches_handler;
    yed_add_event_handler(h);

    /*
     * TODO: Event handler for activating frame or loading frame, or otherwise
     * changing the frame. Matches need to be highlighted in other frames for
     * *any* search.
     */

    yed_plugin_set_command(Self, "find", find_regex_search);
    yed_plugin_set_command(Self, "find-cursor-next-match", find_cursor_next_match);
    yed_plugin_set_command(Self, "find-cursor-prev-match", find_cursor_prev_match);

    return 0;
}

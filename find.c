#include <yed/plugin.h>
#include <yed/syntax.h>
#include <regex.h>

#define FIND_DEFAULT_NUM_MATCHFRAMES 16
#define FIND_DEFAULT_NUM_MATCHES 16
#define FIND_DEFAULT_PATTERN_LEN 16

typedef struct matchframe {
    /* which frame this holds information for */
    yed_frame *yed_frame;
    /* the matches pertaining to this frame */
    array_t matches;
    /*
     * TODO: next/prev indexes
     */
} matchframe;

typedef struct match {
    /* the line within the frame that this is a match for */
    int line;
    /* offsets in the line where the match starts and ends */
    size_t start;
    size_t end;
} match;

static array_t _matchframes;
static array_t _pattern;
static regex_t _regex;
static unsigned _num_matching;

static inline matchframe* find_matchframe_create(yed_frame *frame) {
    matchframe mf;
    mf.yed_frame = frame;
    mf.matches = array_make_with_cap(match, FIND_DEFAULT_NUM_MATCHES);
    array_push(_matchframes, mf);
    return array_last(_matchframes);
}

static inline matchframe* find_matchframe_get(yed_frame *frame) {
    matchframe *mf;
    array_traverse(_matchframes, mf) {
        if (mf->yed_frame == frame)
            return mf;
    }
    return NULL;
}

static inline matchframe* find_matchframe_get_or_create(yed_frame *frame) {
    matchframe *mf = find_matchframe_get(frame);
    if (!mf)
        mf = find_matchframe_create(frame);
    return mf;
}

static void find_matchframe_clear (matchframe *mf) {
    array_clear(mf->matches);
}

static int find_matchframe_num_matches (matchframe *mf) {
    return array_len(mf->matches);
}

int find_matchframe_cursor_nearest_match(matchframe *mf,
                                         int *row,
                                         int *col,
                                         int direction)
{
    match *m;
    int r, c;

    r = mf->yed_frame->cursor_line;
    c = mf->yed_frame->cursor_col;

    if (find_matchframe_num_matches(mf) == 0)
        return 1;

    /*
     * This relies on the fact that the array of matches is in sorted order in
     * terms of row and column because matches are found linearly through the
     * buffer.
     */

    if (direction > 0) {
        array_traverse(mf->matches, m) {
            if (m->line == r && m->start > c)
                goto found;
            if (m->line > r)
                goto found;
        }
        m = array_item(mf->matches, 0);
        yed_cprint("Search hit bottom, continuing at top");
    }
    else {
        array_rtraverse(mf->matches, m) {
            if (m->line == r && m->start < c - 1)
                goto found;
            if (m->line < r)
                goto found;
        }
        m = array_last(mf->matches);
        yed_cprint("Search hit top, continuing at bottom");
    }

found:
    *row = m->line;
    *col = m->start + 1;
    return 0;
}

static int find_matchframe_push_match (matchframe *mf,
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
    match m;
    m.line = row;
    m.start = matches[0].rm_so + offset;
    m.end = matches[0].rm_eo + offset;

    array_grow_if_needed(mf->matches);
    array_push(mf->matches, m);

    /* returns an offset to where the line should be searched from next */
    return matches[0].rm_eo + 1;
}

void find_matchframe_highlight_handler(yed_event *event) {
    matchframe *mf;
    match      *m;
    yed_attrs  *attr, search, search_cursor, *set;
    yed_frame  *frame;

    if (!event->frame)
        return;
    frame = event->frame;
    if (frame != ys->active_frame || !frame->buffer)
        return;

    /* if we don't have any matches for this frame, go next */
    mf = find_matchframe_get(frame);
    if (!mf)
        return;

    /* get the current styles for the search and search cursor */
    search        = yed_active_style_get_search();
    search_cursor = yed_active_style_get_search_cursor();

    /*
     * TODO: Inefficient. On each line render, goes through each match in the
     * array to find any matches on this row's line.
     */
    array_traverse(mf->matches, m) {
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



/*
 * Convenience functions for always having the pattern be null-terminated
 */
static inline void find_pattern_terminate() {
    /* end-of-string, helps when passing values into the macro-only array library */
    static char EOS = '\0';
    array_push(_pattern, EOS);
}

static inline void find_pattern_clear() {
    array_clear(_pattern);
    find_pattern_terminate();
}

static inline int find_pattern_exists() {
    /* if the pattern has anything more than the null terminator, it exists */
    return (array_len(_pattern) > 1);
}

static inline void find_pattern_replace(char *patt) {
    array_clear(_pattern);
    for (int i = 0; patt[i] != '\0'; i++)
        array_push(_pattern, patt[i]);
    find_pattern_terminate();
}

static inline void find_pattern_bad() {
    yed_cprint("Pattern not found: %s", array_data(_pattern));
}

static inline void find_pattern_error(const int status) {
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

    regmatch_t  match[nmatches];
    matchframe *mf;
    int         status;
    int         row;
    char       *line;
    int         len;
    int         offset;

    mf = find_matchframe_get_or_create(frame);
    /* always clear out any matches on a new search */
    find_matchframe_clear(mf);

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
            offset += find_matchframe_push_match(mf, row, offset, nmatches, match);
        }

        free(line);
        row++;
    }

    return find_matchframe_num_matches(mf);
}

void find_search_interactive_start() {
    find_pattern_clear();

    ys->interactive_command = "find-in-buffer";
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
            find_pattern_error(status);
        return;
    }

    /*
     * TODO: travel to first instance of match in frame. if
     * canceling or pattern doesn't match anymore, go back to the
     * original searching place.
     */
    _num_matching = find_search_in_buffer(frame);
    if (_num_matching == 0 && !ys->interactive_command)
        find_pattern_bad();
}

void find_cursor_nearest_match(int n_args, char **args, int direction) {
    int row, col;
    yed_frame  *frame;
    matchframe *mf;

    if (n_args > 0) {
        yed_cerr("Expected zero arguments.");
        return;
    }

    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    mf = find_matchframe_get(frame);
    if (!mf)
        return;

    if (find_matchframe_cursor_nearest_match(mf, &row, &col, direction) == 0)
        yed_set_cursor_far_within_frame(frame, row, col);
}

void find_cursor_next_match(int n_args, char **args) {
    find_cursor_nearest_match(n_args, args, 1);
}

void find_cursor_prev_match(int n_args, char **args) {
    find_cursor_nearest_match(n_args, args, -1);
}

void find_set_search_all_frames(int n_args, char **args) {
}

void find_set_search_prompt(int n_args, char **args) {
}

void find_unload (yed_plugin *self) {
    yed_event_handler h;

    array_free(_matchframes);
    array_free(_pattern);

    /* remove our custom search highlighter */
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_matchframe_highlight_handler;
    yed_delete_event_handler(h);

    /* re-load the default search highlighter */
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = yed_search_line_handler;
    yed_add_event_handler(h);
}


int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    yed_plugin_set_unload_fn(self, find_unload);
    _matchframes = array_make_with_cap(matchframe, FIND_DEFAULT_NUM_MATCHFRAMES);
    _pattern = array_make_with_cap(char,  FIND_DEFAULT_PATTERN_LEN);

    /* remove default search highlighter */
    yed_event_handler h;
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = yed_search_line_handler;
    yed_delete_event_handler(h);

    /* add our custom search highlighter */
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_matchframe_highlight_handler;
    yed_add_event_handler(h);

    /*
     * TODO: Event handler for activating frame or loading frame, or otherwise
     * changing the frame. Matches need to be highlighted in other frames for
     * *any* search.
     */

    yed_plugin_set_command(self, "find-in-buffer", find_regex_search);
    yed_plugin_set_command(self, "find-next-in-buffer", find_cursor_next_match);
    yed_plugin_set_command(self, "find-prev-in-buffer", find_cursor_prev_match);
    yed_plugin_set_command(self, "find-set-search-all-frames", find_set_search_all_frames);
    yed_plugin_set_command(self, "find-set-search-prompt", find_set_search_prompt);

    return 0;
}

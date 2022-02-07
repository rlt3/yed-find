#include <yed/plugin.h>
#include <yed/syntax.h>
#include <regex.h>

#define FIND_DEFAULT_NUM_MATCHFRAMES 16
#define FIND_DEFAULT_NUM_MATCHES 16
#define FIND_DEFAULT_PATTERN_LEN 16
#define FIND_DEFAULT_CMD_PROMPT "(find-in-buffer) "

/**
 * PROPERTIES
 */

typedef struct replace_properties {
    int is_all_lines;    /* replace matches on all lines? */
    int is_single_line;  /* replace only on a single line? */
    int is_confirm;      /* confirm before each replace? */
    int is_global;       /* replace multiple matches on each line? */
    int is_ignore_case;  /* ignore character case when searching? */
    int start_line;      /* the starting and ending lines of the replacement */
    int end_line;
    char *replacement;   /* the string replacing the matches */
} replace_properties;

typedef struct matchframe {
    /* which frame this holds information for */
    yed_frame *yed_frame;
    /* the matches pertaining to this frame */
    array_t matches;
} matchframe;

typedef struct match {
    /* the line within the frame that this is a match for */
    int line;
    /* offsets in the line where the match starts and ends */
    size_t start;
    size_t end;
} match;

/* all frames and the matches therein */
static array_t _matchframes;

/*
 * the string pattern and its compiled representation. only one pattern (a
 * search) exists at a time.
 */
static array_t _pattern;
static regex_t _regex;

static char *_cmd_prompt;

/**
 * MATCHFRAME
 */

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

static void find_matchframe_clear(matchframe *mf) {
    array_clear(mf->matches);
}

static int find_matchframe_num_matches(matchframe *mf) {
    return array_len(mf->matches);
}

void find_matchframe_highlight_handler(yed_event *event) {
    matchframe *mf;
    match      *m;
    yed_attrs  *attr, search, search_cursor, *set;
    yed_frame  *frame;

    if (!event->frame)
        return;

    /* if we don't have any matches for this frame, go next */
    mf = find_matchframe_get(event->frame);
    if (!mf)
        return;

    frame = mf->yed_frame;
    if (frame != ys->active_frame || !frame->buffer)
        return;


    /* get the current styles for the search and search cursor */
    search        = yed_active_style_get_search();
    search_cursor = yed_active_style_get_search_cursor();

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
 * Given row and column `r', `c', search for nearest match in a particular
 * direction (up or down the buffer). Sets the position of the match in `row'
 * and `col' and returns 1 if there are matches. Otherwise, `row' and `col' are
 * not touched and this returns 0.
 */
int find_matchframe_cursor_nearest_match(matchframe *mf,
                                         int r,
                                         int c,
                                         int *row,
                                         int *col,
                                         int direction)
{
    match *m;

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

static inline int find_matchframe_push_match(matchframe *mf,
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

int find_matchframe_search_in_buffer(matchframe *mf, int is_global) {
    /* TODO: execute flags */
    static const int flags = 0;
    /*
     * Right now we only have one matching buffer. For matching subexpressions
     * we would have to pass more, but I'm not sure how to get the number of
     * matches an expression matched.
     */
    static const size_t nmatches = 1;

    regmatch_t  match[nmatches];
    int         status;
    int         row;
    char       *line;
    int         len;
    int         offset;

    /* always clear out any matches on a new search */
    find_matchframe_clear(mf);

    /* search within each line of the buffer */
    row = 1;
    while (1) {
        line = yed_get_line_text(mf->yed_frame->buffer, row);
        if (!line)
            break;

        /* find every match within each line */
        len = strlen(line);
        offset = 0;
        while (offset < len) {
            status = regexec(&_regex, line + offset, nmatches, match, flags);
            if (status != 0 || !is_global)
                break;
            offset += find_matchframe_push_match(mf, row, offset, nmatches, match);
        }

        free(line);
        row++;
    }

    return find_matchframe_num_matches(mf);
}

/**
 * PATTERN
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

int find_pattern_compile(int is_ignore_case) {
    int flags = 0;
    if (is_ignore_case)
        flags |= REG_ICASE;
    return regcomp(&_regex, array_data(_pattern), flags);
}

/**
 * INTERACTIVE SEARCH HANDLERS
 */

void find_search_interactive_start() {
    find_pattern_clear();

    ys->interactive_command = "find-in-buffer";
    /* TODO: Make the cmd_prompt a yed var for this plugin */
    ys->cmd_prompt = _cmd_prompt;

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

void find_search_interactive_cancel(matchframe *mf) {
    /* handle canceling a search part-way through */
    ys->interactive_command = NULL;
    ys->current_search      = NULL;
    yed_clear_cmd_buff();

    find_pattern_clear();
    find_matchframe_clear(mf);
}

void find_search_interactive_finish() {
    /* handle finalizing a search */
    ys->interactive_command = NULL;
    ys->current_search = NULL;
    yed_clear_cmd_buff();
}

/**
 * YED BINDINGS
 */

void find_regex_search(int n_args, char **args) {
    yed_frame  *frame;
    matchframe *mf;
    int         key;
    int         status;
    int         row, col;
    int         num_matches;

    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    mf = find_matchframe_get_or_create(frame);

    if (!ys->interactive_command) {
        if (n_args == 0) {
            /* YEXE("find-in-buffer") enters interactive mode */
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
                find_search_interactive_cancel(mf);
                goto reset_cursor;

            case ENTER:
                find_search_interactive_finish();
                break;

            default:
                find_search_interactive_build_pattern(key);
                break;
        }
    }

    status = find_pattern_compile(0);
    if (status != 0) {
        if (!ys->interactive_command)
            find_pattern_error(status);
        return;
    }

    num_matches = find_matchframe_search_in_buffer(mf, 1);
    if (num_matches == 0) {
        if (!ys->interactive_command)
            find_pattern_bad();
        /* otherwise, just reset the row, col to original location */
reset_cursor:
        row = ys->search_save_row;
        col = ys->search_save_col;
    } else {
        /* use the saved location of the cursor to find nearest match */
        find_matchframe_cursor_nearest_match(mf,
                ys->search_save_row, ys->search_save_col,
                &row, &col,
                1); /* TODO: use a direction when searching backwards */
    }

    yed_set_cursor_far_within_frame(frame, row, col);
}

void find_fill_match_buff(char *buff, int size, char *str, regmatch_t match) {
    int len = match.rm_eo - match.rm_so;
    if (len == 0) {
        buff[0] = '\0';
        return;
    }
    if (len > size)
        len = size;
    strncpy(buff, str + match.rm_so, len);
    buff[len] = '\0';
}

/*
 * Use regex for parsing a regex
 */
int find_parse_replace_expression(matchframe *mf,
                                  replace_properties *rp,
                                  char *exp)
{
    /*
     * Matches into 6 groups:
     * 0) the entire match
     * 1) beginning line or % for all lines
     * 2) end line
     * 3) find expression
     * 4) replace expression
     * 5) search modifiers
     */
    static const char *pattern = "([0-9]+|%)?,?([0-9]+)?s\\/(.*)\\/(.*)\\/(\\w+)?";
    static const size_t nmatches = 6;
    regmatch_t match[nmatches];
    regex_t regex;
    char buff[256];
    int status;

    /* defaults for the find & replace properties */
    rp->is_all_lines = 0;
    rp->is_single_line = 0;
    rp->is_global = 0;
    rp->is_confirm = 0;
    rp->is_ignore_case = 0;
    rp->start_line = -1;
    rp->end_line = -1;
    rp->replacement = NULL;

    status = regcomp(&regex, pattern, REG_EXTENDED);
    if (status != 0) {
        regerror(status, &regex, buff, 256);
        yed_cerr("%s", buff);
        return 1;
    }

    status = regexec(&regex, exp, nmatches, match, 0);
    if (status != 0) {
        yed_cerr("Invalid replace expression!");
        return 1;
    }

    /*
     * If the 1st match is empty then check the 2nd match. If that is empty,
     * then we're doing a find & replace on the current line, otherwise we're
     * doing a find and replace on the given line.
     */
    find_fill_match_buff(buff, 256, exp, match[1]);
    if (buff[0] == '\0') {
        find_fill_match_buff(buff, 256, exp, match[2]);
        if (buff[0] != '\0')
            sscanf(buff, "%d", &rp->start_line);
        else
            rp->start_line = mf->yed_frame->cursor_line;
        rp->is_single_line = 1;
    }
    /*
     * If the 1st match is '%' then we are searching all lines. Verify that the
     * 2nd is empty.
     */
    else if (strcmp(buff, "%") == 0) {
        find_fill_match_buff(buff, 256, exp, match[2]);
        if (buff[0] != '\0') {
            yed_cerr("Expression cannot provide both '\%' and ending line number!");
            return 1;
        }
        rp->is_all_lines = 1;
    }
    /*
     * The 1st match must be a number which is the starting line for the
     * search.  Verify the 2nd match isn't empty. The 2nd match is the ending
     * line for the search.
     */
    else {
        sscanf(buff, "%d", &rp->start_line);
        find_fill_match_buff(buff, 256, exp, match[2]);
        if (buff[0] != '\0') {
            yed_cerr("Expression provided starting line number but no ending!");
            return 1;
        }
        sscanf(buff, "%d", &rp->end_line);
    }

    /*
     * If the 3rd match wasn't provided then we need to use the internally
     * saved pattern (from find-in-buffer). Otherwise, we replace the internal
     * pattern with the provided one.
     */
    find_fill_match_buff(buff, 256, exp, match[3]);
    if (buff[0] != '\0')
        find_pattern_replace(buff);

    /*
     * If the 4th pattern wasn't provided, then we will replace with nothing,
     * or remove the matches found from the buffer.
     */
    find_fill_match_buff(buff, 256, exp, match[4]);
    rp->replacement = strdup(buff);

    /*
     * If the 5th match exists, then we need to match specific search options.
     * 'g' -> replace every match in the line
     * 'c' -> confirm the replacement before changing int
     * 'i' -> ignore case
     */
    find_fill_match_buff(buff, 256, exp, match[5]);
    if (buff[0] != '\0') {
        if (strchr(buff, 'g') != NULL)
            rp->is_global = 1;
        if (strchr(buff, 'c') != NULL)
            rp->is_confirm = 1;
        if (strchr(buff, 'i') != NULL)
            rp->is_ignore_case = 1;
    }

    return 0;
}

void find_regex_replace(int n_args, char **args) {
    replace_properties rp;
    yed_frame         *frame;
    yed_buffer        *buffer;
    matchframe        *mf;
    match             *m;
    int                num_matches;

    if (n_args == 0 || n_args > 1) {
        yed_cerr("Expected 1 argument, received %d", n_args);
        return;
    }

    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    mf = find_matchframe_get_or_create(frame);
    if (find_parse_replace_expression(mf, &rp, args[0]) != 0)
        return;

    find_pattern_compile(rp.is_ignore_case);
    /* TODO: search from start to end line, or specific lines */
    num_matches = find_matchframe_search_in_buffer(mf, rp.is_global);
    if (num_matches == 0) {
        find_pattern_bad();
        return;
    }

    buffer = mf->yed_frame->buffer;
    array_traverse(mf->matches, m) {
        /*
         * TODO:
         * This works for removing the entire match if only one match exists
         * in the line. Removing and inserting characters into the line affects
         * the match column offsets.
         */
        for (unsigned col = m->start; col < m->end; col++) {
            yed_delete_from_line(buffer, m->line, m->start + 1);
        }
        //for (int i = 0; rp.replacement[i] != '\0'; i++) {
        //    yed_insert_into_line(buffer, m->line, m->line + i, G(rp.replacement[i]));
        //}
    }

    find_matchframe_clear(mf);
}

void find_cursor_nearest_match(int n_args, char **args, int direction) {
    int row, col;
    int r, c;
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

    r = frame->cursor_line;
    c = frame->cursor_col;

    if (find_matchframe_cursor_nearest_match(mf, r, c, &row, &col, direction) == 0)
        yed_set_cursor_far_within_frame(frame, row, col);
}

void find_cursor_next_match(int n_args, char **args) {
    find_cursor_nearest_match(n_args, args, 1);
}

void find_cursor_prev_match(int n_args, char **args) {
    find_cursor_nearest_match(n_args, args, -1);
}

void find_set_search_all_frames(int n_args, char **args) {
    yed_cerr("Not implemented!");
}

void find_set_search_prompt(int n_args, char **args) {
    if (n_args < 1) {
        yed_cerr("Expected one argument.");
        return;
    }

    free(_cmd_prompt);
    _cmd_prompt = strdup(args[0]);
}

void find_unload(yed_plugin *self) {
    yed_event_handler h;
    matchframe *mf;

    /* replace the default search highlighter */
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = yed_search_line_handler;
    yed_add_event_handler(h);

    array_traverse(_matchframes, mf) {
        array_free(mf->matches);
    }
    array_free(_matchframes);
    array_free(_pattern);

    free(_cmd_prompt);

    regfree(&_regex);
}

int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    yed_plugin_set_unload_fn(self, find_unload);
    _matchframes = array_make_with_cap(matchframe, FIND_DEFAULT_NUM_MATCHFRAMES);
    _pattern = array_make_with_cap(char,  FIND_DEFAULT_PATTERN_LEN);
    _cmd_prompt = strdup(FIND_DEFAULT_CMD_PROMPT);

    /* remove default search highlighter */
    yed_event_handler h;
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = yed_search_line_handler;
    yed_delete_event_handler(h);

    /* add our custom search highlighter */
    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_matchframe_highlight_handler;
    yed_plugin_add_event_handler(self, h);

    /*
     * TODO: Event handler for activating frame or loading frame, or otherwise
     * changing the frame. Matches need to be highlighted in other frames for
     * *any* search.
     */

    yed_plugin_set_command(self, "find-in-buffer", find_regex_search);
    yed_plugin_set_command(self, "find-and-replace", find_regex_replace);
    yed_plugin_set_command(self, "find-next-in-buffer", find_cursor_next_match);
    yed_plugin_set_command(self, "find-prev-in-buffer", find_cursor_prev_match);
    yed_plugin_set_command(self, "find-set-search-all-frames", find_set_search_all_frames);
    yed_plugin_set_command(self, "find-set-search-prompt", find_set_search_prompt);

    return 0;
}

#include <yed/plugin.h>
#include <yed/syntax.h>
#include <regex.h>

#define FIND_DEFAULT_ARRAY_LEN 16
#define FIND_DEFAULT_FIND_PROMPT "(find-in-buffer) "
#define FIND_DEFAULT_REPLACE_PROMPT "(replace-current-search) "

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
    array_t replacement; /* the string replacing the matches */
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

/*
 * Used globally to hold replacement data. This data can be built
 * interactively, so it needs to be persistent, hence global.
 */
static replace_properties _replace_properties;

yed_cmd_line_readline_ptr_t  _search_readline;
array_t _search_hist;
static int _search_save_row;
static int _search_save_col;

enum find_command {
    FIND_IN_BUFFER,
    REPLACE_CURRENT_SEARCH,
    FIND_NEXT_IN_BUFFER,
    FIND_PREV_IN_BUFFER,
    FIND_AND_REPLACE,
};

/*
 * Because we optionally overwrite default yed commands, this is a useful
 * interface for setting and getting the command names.
 */
char* find_get_command(enum find_command cmd) {
    int replaced = 0;
    if (strcmp(yed_get_var("find-regex-replace-default-commands"), "true") == 0)
        replaced = 1;

    if (!replaced) {
        switch (cmd) {
            case FIND_IN_BUFFER: return "find-in-buffer-regex";
            case REPLACE_CURRENT_SEARCH: return "replace-current-search-regex";
            case FIND_NEXT_IN_BUFFER: return "find-next-in-buffer-regex";
            case FIND_PREV_IN_BUFFER: return "find-prev-in-buffer-regex";
            case FIND_AND_REPLACE: return "find-and-replace-regex";
        }
    } else {
        switch (cmd) {
            case FIND_IN_BUFFER: return "find-in-buffer";
            case REPLACE_CURRENT_SEARCH: return "replace-current-search";
            case FIND_NEXT_IN_BUFFER: return "find-next-in-buffer";
            case FIND_PREV_IN_BUFFER: return "find-prev-in-buffer";
            /* find-and-replace always has -regex appended */
            case FIND_AND_REPLACE: return "find-and-replace-regex";
        }
    }

    return "BAD";
}

/**
 * ARRAY
 */

void find_array_terminate(array_t *arr) {
    /* end-of-string, helps when passing values into the macro-only array library */
    static char EOS = '\0';
    array_push(*arr, EOS);
}

void find_array_replace(array_t *arr, char *s) {
    array_clear(*arr);
    for (int i = 0; s[i] != '\0'; i++)
        array_push(*arr, s[i]);
    find_array_terminate(arr);
}

/**
 * REPLACE PROPERTIES
 */

replace_properties* find_replace_properties_reset() {
    _replace_properties.is_all_lines = 0;
    _replace_properties.is_single_line = 0;
    _replace_properties.is_global = 0;
    _replace_properties.is_confirm = 0;
    _replace_properties.is_ignore_case = 0;
    _replace_properties.start_line = -1;
    _replace_properties.end_line = -1;
    array_clear(_replace_properties.replacement);
    return &_replace_properties;
}

replace_properties* find_replace_properties_get() {
    return &_replace_properties;
}

/**
 * PATTERN
 */

void find_pattern_clear() {
    array_clear(_pattern);
    find_array_terminate(&_pattern);
}

int find_pattern_exists() {
    /* if the pattern has anything more than the null terminator, it exists */
    return (array_len(_pattern) > 1);
}

void find_pattern_bad() {
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

    if (find_matchframe_num_matches(mf) == 0) {
        if (find_pattern_exists())
            find_pattern_bad();
        return 1;
    }

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
            if (status != 0)
                break;
            offset += find_matchframe_push_match(mf, row, offset, nmatches, match);
            if (!is_global)
                break;
        }

        free(line);
        row++;
    }

    return find_matchframe_num_matches(mf);
}

void find_matchframe_replace(matchframe *mf) {
    replace_properties *rp;
    yed_buffer *buffer;
    match      *m;
    char       *replacement;
    int         replacement_len;
    int         num_matches;
	int		    status;
    int         match_len;
    int         offset;
    int         last_line;

    rp = find_replace_properties_get();

    status = find_pattern_compile(rp->is_ignore_case);
    if (status != 0) {
        find_pattern_error(status);
        return;
    }

    /* TODO: search from start to end line, or specific lines */
    num_matches = find_matchframe_search_in_buffer(mf, rp->is_global);
    if (num_matches == 0) {
        find_pattern_bad();
        return;
    }

    buffer = mf->yed_frame->buffer;
    replacement = array_data(rp->replacement);
    replacement_len = array_len(rp->replacement) - 1;

    last_line = -1;
    offset = 0;
    array_traverse(mf->matches, m) {
        match_len = m->end - m->start;

        /*
         * Matches are stored as offsets into the line, but we are deleting and
         * inserting from that line which moves those offsets. We keep a local
         * offset for each match in a line to deal with where the replacement
         * should go versus the match's position.
         */
        if (last_line != m->line)
            offset = 0;
        last_line = m->line;

        /* delete the match one character at a time */
        for (unsigned i = m->start; i < m->end; i++)
            yed_delete_from_line(buffer, m->line, m->start + 1 + offset);

        /* insert replacement one character at a time, in reverse */
        if (replacement[0] != '\0') {
            for (int i = replacement_len - 1; i >= 0; i--) {
                yed_insert_into_line(buffer,
                        m->line,
                        m->start + 1 + offset,
                        G(replacement[i]));
            }
            offset += replacement_len - match_len;
        } else {
            offset -= match_len;
        }
    }

    find_matchframe_clear(mf);
}

/**
 * INTERACTIVE SEARCH HANDLERS
 */

void find_interactive_mode_start(int is_find) {
    if (is_find) {
        ys->interactive_command = find_get_command(FIND_IN_BUFFER);
        ys->cmd_prompt = yed_get_var("find-regex-search-prompt");
    } else {
        ys->interactive_command = find_get_command(REPLACE_CURRENT_SEARCH);
        ys->cmd_prompt = yed_get_var("find-regex-replace-prompt");
    }

    _search_save_row = ys->active_frame->cursor_line;
    _search_save_col = ys->active_frame->cursor_col;

    yed_clear_cmd_buff();
    yed_cmd_line_readline_reset(_search_readline, &_search_hist);
}

void find_interactive_mode_build_array(array_t *arr, char key) {
    yed_cmd_line_readline_take_key(_search_readline, key);
    array_zero_term(ys->cmd_buff);
    find_array_replace(arr, array_data(ys->cmd_buff));
}

void find_interactive_mode_cancel() {
    /* handle canceling a search part-way through */
    ys->interactive_command = NULL;
    yed_clear_cmd_buff();
}

void find_interactive_mode_finish() {
    /* handle finalizing a search */
    ys->interactive_command = NULL;
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
            find_interactive_mode_start(1);
            find_pattern_clear();
            return;
        }
        /* if a pattern is given immediately, use that */
        find_array_replace(&_pattern, args[0]);
    } else {
        /* on interactive mode, build regex incrementally */
        sscanf(args[0], "%d", &key);
        switch (key) {
            case ESC:
            case CTRL_C:
                find_interactive_mode_cancel();
                find_pattern_clear();
                find_matchframe_clear(mf);
                goto reset_cursor;

            case ENTER:
                find_interactive_mode_finish();
                break;

            default:
                find_interactive_mode_build_array(&_pattern, key);
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
reset_cursor:
        row = _search_save_row;
        col = _search_save_col;
    } else {
        /* use the saved location of the cursor to find nearest match */
        find_matchframe_cursor_nearest_match(mf,
                _search_save_row, _search_save_col,
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
int find_parse_sed_expression(matchframe *mf, char *exp)
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

    replace_properties *rp;
    regmatch_t match[nmatches];
    regex_t regex;
    char buff[256];
    int status;

    rp = find_replace_properties_reset();

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
        find_array_replace(&_pattern, buff);

    /*
     * If the 4th pattern wasn't provided, then we will replace with nothing,
     * or remove the matches found from the buffer.
     */
    find_fill_match_buff(buff, 256, exp, match[4]);
    array_push_n(rp->replacement, buff, (match[4].rm_eo - match[4].rm_so) + 1);

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

/* So a find & replace using a sed expression */
void find_regex_sed_replace(int n_args, char **args) {
    yed_frame  *frame;
    matchframe *mf;

    if (n_args == 0 || n_args > 1) {
        yed_cerr("Expected 1 argument, received %d", n_args);
        return;
    }
    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    mf = find_matchframe_get_or_create(frame);
    if (find_parse_sed_expression(mf, args[0]) != 0)
        return;

    find_matchframe_replace(mf);
}

/* Replace the current matches in the buffer with the given string */
void find_regex_replace(int n_args, char **args) {
    replace_properties *rp;
    yed_frame          *frame;
    matchframe         *mf;
    int                 key;

    if (!find_pattern_exists()) {
        yed_cerr("No matches to replace!");
        return;
    }

    if (!ys->active_frame || !ys->active_frame->buffer)
        return;
    frame = ys->active_frame;

    mf = find_matchframe_get_or_create(frame);
    rp = find_replace_properties_get();

    if (!ys->interactive_command) {
        find_replace_properties_reset();
        rp->is_global = 1;

        if (n_args == 0) {
            find_interactive_mode_start(0);
            return;
        }

        find_array_replace(&rp->replacement, args[0]);
        goto replace;
    } else {
        sscanf(args[0], "%d", &key);
        switch (key) {
            case ESC:
            case CTRL_C:
                find_interactive_mode_cancel();
                return;

            case ENTER:
                find_interactive_mode_finish();
                break;

            default:
                find_interactive_mode_build_array(&rp->replacement, key);
                return;
        }
    }

    /*
     * We only make it here when interactive mode is finished, when building
     * the replacement is done.
     */
replace:
    find_matchframe_replace(mf);
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

void find_unload(yed_plugin *self) {
    matchframe *mf;

    array_traverse(_matchframes, mf) {
        array_free(mf->matches);
    }
    array_free(_matchframes);
    array_free(_pattern);
    array_free(_search_hist);
    array_free(_replace_properties.replacement);
    free(_search_readline);
    regfree(&_regex);
}

int yed_plugin_boot(yed_plugin *self) {
    yed_event_handler h;

    YED_PLUG_VERSION_CHECK();

    yed_plugin_set_unload_fn(self, find_unload);

    _matchframes = array_make_with_cap(matchframe, FIND_DEFAULT_ARRAY_LEN);
    _pattern = array_make_with_cap(char, FIND_DEFAULT_ARRAY_LEN);
    _replace_properties.replacement = array_make_with_cap(char, FIND_DEFAULT_ARRAY_LEN);

    _search_hist     = array_make(char*);
    _search_readline = malloc(sizeof(*ys->search_readline));
    yed_cmd_line_readline_make(_search_readline, &_search_hist);

    h.kind = EVENT_LINE_PRE_DRAW;
    h.fn   = find_matchframe_highlight_handler;
    yed_plugin_add_event_handler(self, h);

    /*
     * TODO: Event handler for activating frame or loading frame, or otherwise
     * changing the frame. Matches need to be highlighted in other frames for
     * *any* search.
     */

    if (!yed_get_var("find-regex-replace-default-commands"))
        yed_set_var("find-regex-replace-default-commands", "false");

    if (!yed_get_var("find-regex-search-prompt"))
        yed_set_var("find-regex-search-prompt", FIND_DEFAULT_FIND_PROMPT);

    if (!yed_get_var("find-regex-replace-prompt"))
        yed_set_var("find-regex-replace-prompt", FIND_DEFAULT_REPLACE_PROMPT);

    if (!yed_get_var("find-regex-search-all-frames"))
        yed_set_var("find-regex-search-all-frames", "true");


    yed_plugin_set_command(self, find_get_command(FIND_IN_BUFFER), find_regex_search);
    yed_plugin_set_command(self, find_get_command(REPLACE_CURRENT_SEARCH), find_regex_replace);
    yed_plugin_set_command(self, find_get_command(FIND_NEXT_IN_BUFFER), find_cursor_next_match);
    yed_plugin_set_command(self, find_get_command(FIND_PREV_IN_BUFFER), find_cursor_prev_match);
    yed_plugin_set_command(self, find_get_command(FIND_AND_REPLACE), find_regex_sed_replace);

    return 0;
}

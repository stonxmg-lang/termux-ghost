/*
 * termux-ghost: history autosuggestions for bash on Termux.
 *
 * A bash loadable builtin that hooks GNU Readline from inside the shell.
 * No per-key bash code, no forks, no key rebinding of ordinary characters.
 *
 *   rl_redisplay_function  paints the suggestion after readline has drawn.
 *   rl_getc_function       removes it before a non-printing key is handled,
 *                          so Enter, Tab, Ctrl-L etc. never see stale text.
 *   SIGINT / SIGWINCH      are wrapped so the text is removed first, then the
 *                          original handler runs unchanged. Readline prints ^C
 *                          from its signal path, before any hook could run.
 *
 * The redisplay hook is live only while a key is being processed. Readline
 * treats any custom redisplay function as a dumb terminal when it (re)reads the
 * terminal database, and takes a cruder path on resize, so its own function
 * stays in place during input waits and while commands run.
 *
 * Readline never learns about the ghost text: it is drawn after readline's
 * own output and the cursor is restored with relative moves, which stay
 * correct even if drawing scrolled the screen.
 */
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <wchar.h>
#include <readline/readline.h>
#include <readline/history.h>

/* Layout of bash's `struct builtin`; stable across bash 4.x and 5.x. */
struct builtin {
  char *name;
  int (*function)(void *);
  int flags;
  char *const *long_doc;
  const char *short_doc;
  char *handle;
};
#define BUILTIN_ENABLED 1
#define EXECUTION_SUCCESS 0
#define EXECUTION_FAILURE 1
#define EX_USAGE 258

/* Exported by readline and bash; weak so a future build can't break loading. */
extern int _rl_last_c_pos __attribute__((weak));
extern char *current_prompt_string __attribute__((weak));
extern char *ps1_prompt __attribute__((weak));

#define BUF_MAX 8192

static int enabled = 1;
static int ghost_visible = 0;
static volatile sig_atomic_t suppress_paint = 0;
static int keys_bound = 0;
static const char *style = "\033[90m";

static rl_voidfunc_t *orig_redisplay;
static rl_getc_func_t *orig_getc;
static rl_voidfunc_t *orig_deprep;

/* Search cache: newest match for prefix P that also starts with P+x is also
   the newest match for P+x, so extending a line needs no new scan. */
static char *cached;
static size_t cached_prefix_len;
static int cached_history_len = -1;
static int cached_history_base = -1;

static void out(const char *s, size_t n)
{
  FILE *f = rl_outstream ? rl_outstream : stdout;
  fflush(f);
  int fd = fileno(f);
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) return;
    s += w;
    n -= (size_t)w;
  }
}

static void erase_ghost(void)
{
  if (ghost_visible) {
    ghost_visible = 0;
    out("\033[J", 3);
  }
}

static void cache_clear(void)
{
  free(cached);
  cached = NULL;
  cached_prefix_len = 0;
}

/* ---- ignore patterns ---- */

/* History lines matching any of these globs are never suggested, so a command
   that once carried a password or token is not shown again on screen.
   Matching is case-insensitive: patterns and lines are compared in ASCII
   lowercase, which leaves UTF-8 bytes untouched. */
#define IGNORE_MAX 64
#define PATTERN_MAX 256
static const char *const default_ignores[] = {
  "*password*", "*passwd*", "*secret*", "*token*", "*api_key*", "*apikey*",
  "*authorization:*", "*ghp_*", "*github_pat_*", NULL
};
static char *ignores[IGNORE_MAX];
static int n_ignores;

static char *lower_dup(const char *s)
{
  char *d = strdup(s);
  if (d)
    for (char *p = d; *p; p++)
      if ((unsigned char)*p < 0x80) *p = (char)tolower((unsigned char)*p);
  return d;
}

static void ignores_changed(void)
{
  cache_clear();
  cached_history_len = -1;
}

static void ignore_clear(void)
{
  for (int i = 0; i < n_ignores; i++) free(ignores[i]);
  n_ignores = 0;
  ignores_changed();
}

static int ignore_find(const char *lower)
{
  for (int i = 0; i < n_ignores; i++)
    if (!strcmp(ignores[i], lower)) return i;
  return -1;
}

/* 0 added or already present, 1 list full, 2 too long or out of memory. */
static int ignore_add(const char *pattern)
{
  if (strlen(pattern) >= PATTERN_MAX) return 2;
  char *p = lower_dup(pattern);
  if (!p) return 2;
  if (ignore_find(p) >= 0) { free(p); return 0; }
  if (n_ignores >= IGNORE_MAX) { free(p); return 1; }
  ignores[n_ignores++] = p;
  ignores_changed();
  return 0;
}

static int ignore_remove(const char *pattern)
{
  char *p = lower_dup(pattern);
  if (!p) return 0;
  int i = ignore_find(p);
  free(p);
  if (i < 0) return 0;
  free(ignores[i]);
  memmove(&ignores[i], &ignores[i + 1], (size_t)(n_ignores - i - 1) * sizeof *ignores);
  n_ignores--;
  ignores_changed();
  return 1;
}

static void ignore_reset(void)
{
  ignore_clear();
  for (int i = 0; default_ignores[i]; i++) ignore_add(default_ignores[i]);
}

static int is_ignored(const char *line)
{
  if (n_ignores == 0) return 0;
  char *l = lower_dup(line);
  if (!l) return 1;   /* can't check it, so don't show it */
  int hit = 0;
  for (int i = 0; i < n_ignores && !hit; i++)
    hit = fnmatch(ignores[i], l, 0) == 0;
  free(l);
  return hit;
}

static int single_line_printable(const char *s)
{
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || *s == 0x7f) return 0;
  return 1;
}

static const char *find_suggestion(const char *line, size_t len)
{
  if (history_length != cached_history_len || history_base != cached_history_base) {
    cache_clear();
    cached_history_len = history_length;
    cached_history_base = history_base;
  } else if (cached && cached_prefix_len <= len && strncmp(cached, line, len) == 0) {
    return cached[len] ? cached : NULL;
  }

  cache_clear();
  HIST_ENTRY **list = history_list();
  if (!list) return NULL;
  for (int i = history_length - 1; i >= 0; i--) {
    const char *h = list[i]->line;
    if (strncmp(h, line, len) == 0 && h[len] && single_line_printable(h) && !is_ignored(h)) {
      cached = strdup(h);
      cached_prefix_len = len;
      return cached;
    }
  }
  return NULL;
}

static int suggestions_allowed(void)
{
  if (!enabled || rl_point != rl_end || rl_end == 0 || rl_done) return 0;
  /* `read -e` clears this; its input has nothing to do with shell history. */
  if (!rl_attempted_completion_function) return 0;
  if (RL_ISSTATE(RL_STATE_ISEARCH | RL_STATE_NSEARCH | RL_STATE_SEARCH |
                 RL_STATE_NUMERICARG | RL_STATE_COMPLETING | RL_STATE_VIMOTION |
                 RL_STATE_MULTIKEY | RL_STATE_MACROINPUT | RL_STATE_MOREINPUT))
    return 0;
  /* PS2 continuation lines are not whole commands. */
  if (&current_prompt_string && &ps1_prompt && current_prompt_string != ps1_prompt) return 0;
  /* Mid-word in a key sequence or a paste: the next redisplay will handle it. */
  struct pollfd p = { .fd = fileno(rl_instream ? rl_instream : stdin), .events = POLLIN };
  if (poll(&p, 1, 0) > 0) return 0;
  return 1;
}

static size_t utf8_decode(const char *s, wchar_t *wc)
{
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80) { *wc = u[0]; return 1; }
  if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
    *wc = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    return 2;
  }
  if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
    *wc = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return 3;
  }
  if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
      (u[3] & 0xC0) == 0x80) {
    *wc = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    return 4;
  }
  *wc = 0xFFFD;
  return 1;
}

static void paint_ghost(const char *tail)
{
  int rows, cols;
  rl_get_screen_size(&rows, &cols);
  if (cols <= 0 || rows <= 1 || !&_rl_last_c_pos) return;

  int start = _rl_last_c_pos;
  if (start < 0 || start >= cols) return;

  /* Walk the text exactly as the terminal will, to know where it ends. */
  int col = start, down = 0;
  size_t copied = 0;
  const char *p = tail;
  static char buf[BUF_MAX + 64];
  size_t n = 0;
  size_t sl = strlen(style);
  memcpy(buf, style, sl);
  n = sl;

  while (*p && copied < BUF_MAX) {
    wchar_t wc;
    size_t len = utf8_decode(p, &wc);
    int w = wcwidth(wc);
    if (w < 0) w = 1;
    if (col + w > cols) {
      if (down + 1 >= rows - 1) break;   /* never exceed the screen */
      down++;
      col = 0;
    }
    memcpy(buf + n, p, len);
    n += len;
    copied += len;
    col += w;
    p += len;
  }
  if (copied == 0) return;

  n += (size_t)snprintf(buf + n, 8, "\033[0m");
  /* A full last column leaves the terminal in "pending wrap"; the cursor
     stays on that row, so `down` above is already right. */
  n += (size_t)snprintf(buf + n, 32, "\r");
  if (down) n += (size_t)snprintf(buf + n, 16, "\033[%dA", down);
  if (start) n += (size_t)snprintf(buf + n, 16, "\033[%dC", start);
  out(buf, n);
  ghost_visible = 1;
}

static void ghost_redisplay(void)
{
  erase_ghost();
  orig_redisplay();
  if (suppress_paint || !suggestions_allowed()) return;
  const char *s = find_suggestion(rl_line_buffer, (size_t)rl_end);
  if (s) paint_ghost(s + rl_end);
}

/* ---- accepting ---- */

static const char *current_tail(void)
{
  if (!enabled || rl_point != rl_end || rl_end == 0) return NULL;
  if (!rl_attempted_completion_function) return NULL;
  const char *s = find_suggestion(rl_line_buffer, (size_t)rl_end);
  return s ? s + rl_end : NULL;
}

static int accept_all_or(rl_command_func_t *fallback, int count, int key)
{
  const char *t = current_tail();
  if (t) {
    rl_insert_text(t);
    return 0;
  }
  return fallback(count, key);
}

static int ghost_forward_char(int c, int k) { return accept_all_or(rl_forward_char, c, k); }
static int ghost_end_of_line(int c, int k) { return accept_all_or(rl_end_of_line, c, k); }

static int ghost_forward_word(int c, int k)
{
  const char *t = current_tail();
  if (!t) return rl_forward_word(c, k);
  size_t n = 0;
  while (t[n] == ' ') n++;
  while (t[n] && t[n] != ' ') n++;
  char *word = strndup(t, n);
  if (!word) return 1;
  rl_insert_text(word);
  free(word);
  return 0;
}

static void rebind(Keymap map, rl_command_func_t *from, rl_command_func_t *to)
{
  /* Takes over wherever the user's own config has that action, so custom
     keys keep working and nothing unrelated is touched. */
  char **seqs = rl_invoking_keyseqs_in_map(from, map);
  if (!seqs) return;
  for (int i = 0; seqs[i]; i++) {
    rl_bind_keyseq_in_map(seqs[i], to, map);
    free(seqs[i]);
  }
  free(seqs);
}

static void bind_keys(void)
{
  Keymap maps[] = { emacs_standard_keymap, vi_insertion_keymap };
  rl_add_defun("ghost-forward-char", ghost_forward_char, -1);
  rl_add_defun("ghost-accept", ghost_end_of_line, -1);
  rl_add_defun("ghost-accept-word", ghost_forward_word, -1);
  for (size_t i = 0; i < sizeof maps / sizeof *maps; i++) {
    rebind(maps[i], rl_forward_char, ghost_forward_char);
    rebind(maps[i], rl_end_of_line, ghost_end_of_line);
    rebind(maps[i], rl_forward_word, ghost_forward_word);
    /* The keys Termux's extra-keys row actually sends. */
    static const struct { const char *seq; rl_command_func_t *fn; } k[] = {
      { "\033[C", ghost_forward_char }, { "\033OC", ghost_forward_char },
      { "\033[F", ghost_end_of_line },  { "\033OF", ghost_end_of_line },
      { "\033[4~", ghost_end_of_line },
      { "\033[1;3C", ghost_forward_word }, { "\033[1;5C", ghost_forward_word },
    };
    for (size_t j = 0; j < sizeof k / sizeof *k; j++) {
      rl_command_func_t *cur = rl_function_of_keyseq(k[j].seq, maps[i], NULL);
      if (!cur || cur == rl_forward_char || cur == rl_end_of_line || cur == rl_forward_word ||
          cur == rl_do_lowercase_version)
        rl_bind_keyseq_in_map(k[j].seq, k[j].fn, maps[i]);
    }
  }
}

/* ---- signals ---- */

/* Readline re-installs its handlers on every readline() call, so these are
   checked again before each wait for a key. */
static const int wrapped_signals[] = { SIGINT, SIGWINCH };
#define N_SIGNALS (sizeof wrapped_signals / sizeof *wrapped_signals)
static struct sigaction chained[N_SIGNALS];
static volatile sig_atomic_t waiting_for_key = 0;

static void ghost_signal(int sig)
{
  int saved_errno = errno;
  /* Only while blocked on a key is the cursor known to be at the ghost text. */
  /* After Ctrl-C readline redraws once more before it prints ^C. */
  if (sig == SIGINT) suppress_paint = 1;
  if (waiting_for_key && ghost_visible) {
    ghost_visible = 0;
    FILE *f = rl_outstream ? rl_outstream : stdout;
    ssize_t ignored = write(fileno(f), "\033[J", 3);
    (void)ignored;
  }
  for (size_t i = 0; i < N_SIGNALS; i++) {
    if (wrapped_signals[i] == sig) {
      void (*h)(int) = chained[i].sa_handler;
      errno = saved_errno;
      if (h != SIG_DFL && h != SIG_IGN) h(sig);
      break;
    }
  }
}

static void wrap_signals(void)
{
  for (size_t i = 0; i < N_SIGNALS; i++) {
    struct sigaction cur;
    if (sigaction(wrapped_signals[i], NULL, &cur) != 0) continue;
    if ((cur.sa_flags & SA_SIGINFO) || cur.sa_handler == ghost_signal ||
        cur.sa_handler == SIG_DFL || cur.sa_handler == SIG_IGN)
      continue;
    chained[i] = cur;
    struct sigaction mine = cur;
    mine.sa_handler = ghost_signal;
    sigaction(wrapped_signals[i], &mine, NULL);
  }
}

static void unwrap_signals(void)
{
  for (size_t i = 0; i < N_SIGNALS; i++) {
    struct sigaction cur;
    if (sigaction(wrapped_signals[i], NULL, &cur) == 0 &&
        !(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == ghost_signal)
      sigaction(wrapped_signals[i], &chained[i], NULL);
  }
}

/* ---- input hooks ---- */

static int ghost_getc(FILE *stream)
{
  if (!keys_bound) {
    keys_bound = 1;
    bind_keys();
  }
  wrap_signals();
  rl_redisplay_function = orig_redisplay;
  waiting_for_key = 1;
  int c = orig_getc(stream);
  waiting_for_key = 0;
  suppress_paint = 0;
  rl_redisplay_function = ghost_redisplay;
  /* Printable keys and Backspace always redisplay, which erases atomically
     with the update and avoids flicker. Anything else might print below the
     line or move away, so clear first. */
  if (ghost_visible && (c == EOF || (c < 0x20 && c >= 0) || RL_ISSTATE(RL_STATE_MULTIKEY)))
    erase_ghost();
  return c;
}

/* Runs whenever readline hands the terminal back, including on Enter. */
static void ghost_deprep(void)
{
  rl_redisplay_function = orig_redisplay;
  if (orig_deprep) orig_deprep();
}

static void install(void)
{
  orig_redisplay = rl_redisplay_function ? rl_redisplay_function : rl_redisplay;
  if (rl_deprep_term_function != ghost_deprep) {
    orig_deprep = rl_deprep_term_function;
    rl_deprep_term_function = ghost_deprep;
  }
  if (rl_getc_function != ghost_getc) {
    orig_getc = rl_getc_function ? rl_getc_function : rl_getc;
    rl_getc_function = ghost_getc;
  }
}

/* ---- the `ghost` builtin ---- */

static char style_buf[64];

static int ghost_builtin(void *list)
{
  /* WORD_LIST is { next, { word, flags } * }. */
  struct wd { char *word; int flags; };
  struct wl { struct wl *next; struct wd *word; };
  struct wl *l = list;
  const char *cmd = l ? l->word->word : "status";

  if (!strcmp(cmd, "on")) {
    enabled = 1;
  } else if (!strcmp(cmd, "off")) {
    enabled = 0;
    cache_clear();
  } else if (!strcmp(cmd, "status")) {
    printf("ghost: %s\n", enabled ? "on" : "off");
  } else if (!strcmp(cmd, "ignore")) {
    struct wl *a = l->next;
    if (!a) {
      for (int i = 0; i < n_ignores; i++) printf("%s\n", ignores[i]);
    } else if (!strcmp(a->word->word, "--clear") && !a->next) {
      ignore_clear();
    } else if (!strcmp(a->word->word, "--reset") && !a->next) {
      ignore_reset();
    } else {
      for (; a; a = a->next) {
        int r = ignore_add(a->word->word);
        if (r == 1) {
          fprintf(stderr, "ghost: ignore list is full (%d patterns)\n", IGNORE_MAX);
          return EXECUTION_FAILURE;
        }
        if (r == 2) {
          fprintf(stderr, "ghost: pattern too long (max %d bytes)\n", PATTERN_MAX - 1);
          return EXECUTION_FAILURE;
        }
      }
    }
  } else if (!strcmp(cmd, "unignore")) {
    if (!l->next) {
      fprintf(stderr, "ghost: unignore takes the pattern to remove\n");
      return EX_USAGE;
    }
    int status = EXECUTION_SUCCESS;
    for (struct wl *a = l->next; a; a = a->next)
      if (!ignore_remove(a->word->word)) {
        fprintf(stderr, "ghost: not in the ignore list: %s\n", a->word->word);
        status = EXECUTION_FAILURE;
      }
    return status;
  } else if (!strcmp(cmd, "color")) {
    /* e.g. `ghost color 90` or `ghost color 38;5;244` */
    const char *v = l->next ? l->next->word->word : NULL;
    if (!v || strspn(v, "0123456789;") != strlen(v) || strlen(v) > 40) {
      fprintf(stderr, "ghost: color takes SGR numbers, e.g. 90 or 38;5;244\n");
      return EXECUTION_FAILURE;
    }
    snprintf(style_buf, sizeof style_buf, "\033[%sm", v);
    style = style_buf;
  } else {
    fprintf(stderr, "usage: ghost [on|off|status|color SGR|ignore [PATTERN...|--clear|--reset]|unignore PATTERN...]\n");
    return EX_USAGE;
  }
  return EXECUTION_SUCCESS;
}

int ghost_builtin_load(char *name)
{
  (void)name;
  if (n_ignores == 0) ignore_reset();
  install();
  return 1;
}

/* Restore every hook so `enable -d ghost` leaves bash exactly as it was. */
void ghost_builtin_unload(char *name)
{
  (void)name;
  erase_ghost();
  rl_redisplay_function = orig_redisplay;
  if (rl_deprep_term_function == ghost_deprep) rl_deprep_term_function = orig_deprep;
  if (rl_getc_function == ghost_getc) rl_getc_function = orig_getc;
  unwrap_signals();
  if (keys_bound) {
    Keymap maps[] = { emacs_standard_keymap, vi_insertion_keymap };
    for (size_t i = 0; i < 2; i++) {
      rebind(maps[i], ghost_forward_char, rl_forward_char);
      rebind(maps[i], ghost_end_of_line, rl_end_of_line);
      rebind(maps[i], ghost_forward_word, rl_forward_word);
    }
  }
  ignore_clear();
}

static char *const ghost_doc[] = {
  "History autosuggestions for the command line.",
  "",
  "ghost on|off      enable or disable suggestions",
  "ghost status      show the current state",
  "ghost color SGR   set the colour, e.g. 90 or 38;5;244",
  "ghost ignore      list patterns that are never suggested",
  "ghost ignore PAT  never suggest history lines matching PAT, e.g. '*token*'",
  "                  (quote it, or bash may expand it to file names)",
  "ghost ignore --clear | --reset   remove all patterns | restore defaults",
  "ghost unignore PAT               remove one pattern",
  NULL
};

struct builtin ghost_struct = {
  "ghost", ghost_builtin, BUILTIN_ENABLED, ghost_doc,
  "ghost [on|off|status|color SGR|ignore [PATTERN...]|unignore PATTERN...]", 0
};

/*
 * Unit tests for the parts of ghost.c that don't need a terminal:
 * history search, the suggestion cache and the ignore list.
 * Builds on any Linux with readline:
 *   cc -fsanitize=address,undefined -g tests/unit.c -lreadline -o unit && ./unit
 * The end-to-end tests in tests/run.sh cover everything on screen.
 */
#include "../ghost.c"

struct wd { char *word; int flags; };
struct wl { struct wl *next; struct wd *word; };

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int run(int n, char **argv)
{
  struct wd w[8];
  struct wl l[8];
  for (int i = 0; i < n; i++) {
    w[i].word = argv[i];
    w[i].flags = 0;
    l[i].word = &w[i];
    l[i].next = i + 1 < n ? &l[i + 1] : NULL;
  }
  return ghost_builtin(n ? &l[0] : NULL);
}
#define GHOST(...) ({ char *_a[] = { __VA_ARGS__ }; run(sizeof _a / sizeof *_a, _a); })

static const char *sug(const char *prefix) { return find_suggestion(prefix, strlen(prefix)); }
static int is(const char *got, const char *want) { return got && !strcmp(got, want); }

int main(void)
{
  using_history();
  add_history("export GH_USER=stonx");
  add_history("export GH_TOKEN=ghp_abc123");
  add_history("curl -H 'Authorization: Bearer x' api.example.com");
  add_history("curl https://example.com");
  add_history("mysql -u root --Password=hunter2");
  add_history("echo مرحبا");
  add_history("printf 'a\\nb'");
  add_history("multi\nline");

  ghost_builtin_load("ghost");
  CHECK(n_ignores == 9);

  /* plain search */
  CHECK(is(sug("echo "), "echo مرحبا"));
  CHECK(sug("echo مرحبا") == NULL);   /* nothing left to suggest */
  CHECK(sug("multi") == NULL);        /* control characters are never shown */
  CHECK(sug("zzz") == NULL);

  /* defaults skip secrets and fall back to older safe lines */
  CHECK(is(sug("export GH_"), "export GH_USER=stonx"));
  CHECK(sug("export GH_T") == NULL);
  CHECK(sug("curl -") == NULL);       /* Authorization: matched case-insensitively */
  CHECK(is(sug("curl "), "curl https://example.com"));
  CHECK(sug("mysql") == NULL);        /* --Password */

  /* changing the list invalidates the cache */
  CHECK(sug("curl h") != NULL);
  CHECK(GHOST("ignore", "*example.com*") == EXECUTION_SUCCESS);
  CHECK(sug("curl h") == NULL);
  CHECK(GHOST("ignore", "*EXAMPLE.COM*") == EXECUTION_SUCCESS);
  CHECK(n_ignores == 10);             /* duplicates are not added twice */
  CHECK(GHOST("unignore", "*Example.com*") == EXECUTION_SUCCESS);
  CHECK(sug("curl h") != NULL);
  CHECK(GHOST("unignore", "*nope*") == EXECUTION_FAILURE);
  CHECK(GHOST("unignore") == EX_USAGE);

  /* new history entries are picked up */
  add_history("curl https://example.org/new");
  CHECK(is(sug("curl h"), "curl https://example.org/new"));

  CHECK(GHOST("ignore", "--clear") == EXECUTION_SUCCESS && n_ignores == 0);
  CHECK(is(sug("export GH_"), "export GH_TOKEN=ghp_abc123"));
  CHECK(sug("mysql") != NULL);
  CHECK(GHOST("ignore", "--reset") == EXECUTION_SUCCESS && n_ignores == 9);
  CHECK(is(sug("export GH_"), "export GH_USER=stonx"));

  /* limits */
  char big[300];
  memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = 0;
  CHECK(GHOST("ignore", big) == EXECUTION_FAILURE);
  for (int i = n_ignores; i < IGNORE_MAX; i++) {
    char p[16];
    snprintf(p, sizeof p, "p%d", i);
    CHECK(GHOST("ignore", p) == EXECUTION_SUCCESS);
  }
  CHECK(GHOST("ignore", "one-more") == EXECUTION_FAILURE);
  CHECK(n_ignores == IGNORE_MAX);

  /* other commands */
  CHECK(GHOST("color", "38;5;244") == EXECUTION_SUCCESS);
  CHECK(GHOST("color", "red") == EXECUTION_FAILURE);
  CHECK(GHOST("off") == EXECUTION_SUCCESS && current_tail() == NULL);
  CHECK(GHOST("on") == EXECUTION_SUCCESS);
  CHECK(GHOST("bogus") == EX_USAGE);

  ghost_builtin_unload("ghost");
  CHECK(n_ignores == 0);

  if (failures) {
    fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  puts("all unit tests passed");
  return 0;
}
